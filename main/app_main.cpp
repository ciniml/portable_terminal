// Phase 2 step 3: byte input from USB-Serial-JTAG and an external UART
// (Tab5 Port-A by default) flow into the VT100 core. Each source has
// its own cooked-input filter so cross-source CR/LF state stays
// independent. Cursor renderer + 1 Hz blink unchanged from step 2.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "boot_progress.hpp"
#include "byte_input.hpp"
#include "cooked_input.hpp"
#include "cursor_renderer.hpp"
#include "display_m5gfx.hpp"
#include "input_touch.hpp"
#include "input_tab5_kbd.hpp"
#include "input_usb_hid.hpp"
#include "input_usb_jtag.hpp"
#include "menu.hpp"
#include "soft_keyboard.hpp"
#include "status_bar.hpp"
#include "ui_root.hpp"
#if CONFIG_TAB5_UART_INPUT_ENABLED
#include "input_uart.hpp"
#endif
#if CONFIG_TAB5_WIFI_ENABLED
#include "wifi_config.hpp"
#include "wifi_setup.hpp"
#endif
#include "vpn.hpp"
#include "connection.hpp"
#include "profiles.hpp"
#if CONFIG_TAB5_BLE_CONFIG_ENABLED
#include "ble_config/ble_config.hpp"
#endif
#include "reconnect.hpp"
#if CONFIG_TAB5_SSH_ENABLED
#include "ssh_connection.hpp"
#endif
#if CONFIG_TAB5_TELNET_ENABLED
#include "telnet_connection.hpp"
#endif
#include "usbserial_connection.hpp"
#include "usb_host_root.hpp"
#include "term_core/terminal.hpp"

namespace {

constexpr const char* kTag = "tab5_term";
constexpr uint16_t kCols = 80;
constexpr uint16_t kRows = 30;
// When the soft keyboard is visible it consumes the bottom of the LCD;
// shrink the terminal grid so all cells stay above it. The panel is at
// y=336..720, so 14 rows × 24 px = 336 px terminal fits exactly above.
constexpr uint16_t kRowsKbd = 14;
constexpr int64_t kBlinkPeriodUs = 500 * 1000;

SemaphoreHandle_t g_mutex = nullptr;
tab5::CursorRenderer* g_cursor = nullptr;

// Recursive so the same task can re-enter the critical section: e.g.
// touch_task holds Lock around kbd.handle_touch(), which may invoke
// the keyboard's sink → usb_sink → make_source_sink, which itself
// takes Lock. With a non-recursive mutex that path self-deadlocks
// whenever no remote connection is up (so usb_sink isn't rebound to
// the lock-free remote_send variant).
class Lock {
public:
    Lock() { xSemaphoreTakeRecursive(g_mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(g_mutex); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

void on_blink_tick(void* /*arg*/) {
    if (!g_cursor) return;
    Lock lk;
    if (tab5::ui_root::overlay_active()) return;
    g_cursor->toggle_blink();
}

// Build a sink that owns its own CookedInputFilter so each input source
// (USB-JTAG, UART, USB-HID, soft keyboard) tracks CR/LF state independently
// while no remote session is up. The sink itself dispatches:
//   - if a remote IConnection is active → forward raw bytes to it (the
//     remote echoes / handles BS itself; running CR→CRLF / BS-Space-BS
//     locally would double-edit);
//   - otherwise → run the cooked filter, lock the UI mutex, and apply()
//     into the local terminal so the device still behaves as a typing
//     surface when no remote is connected.
// The active-connection check happens at every byte batch, so sinks
// captured at boot keep working after a later auto-connect / supervisor
// reconnect without needing to be rebound.
template <class TerminalApply>
tab5::ByteSink make_source_sink(TerminalApply&& apply) {
    auto state = std::make_shared<tab5::CookedInputFilter>();
    return [state, apply](std::span<const uint8_t> bytes) mutable {
        // When the settings menu is on screen it captures all physical
        // keyboard input — arrow keys for nav, Enter to activate, ESC
        // to close, plus printable/BS for the focused text field.
        // Bytes are swallowed here so they don't bleed through to the
        // remote session or the local terminal beneath.
        if (tab5::menu.visible()) {
            Lock lk;
            tab5::menu.feed(bytes);
            return;
        }
        if (auto* c = tab5::active_connection()) {
            c->send(bytes);
            return;
        }
        std::vector<uint8_t> mapped;
        mapped.reserve(bytes.size() * 3);
        state->process(bytes, mapped);
        Lock lk;
        apply(std::span<const uint8_t>(mapped.data(), mapped.size()));
    };
}

// Active connection + the profile that produced it. The connection's
// config structs (SshConfig / TelnetConfig) hold raw const char* into
// the profile's char[] fields, so the profile must outlive the
// connection — keep both at file scope.
std::unique_ptr<tab5::IConnection> g_conn;
std::optional<tab5::Profile>       g_active_profile;

// Callbacks the boot task needs (closures over the terminal / cursor
// statics in app_main). Heap-allocated and handed to xTaskCreate as
// void* so the task can drain them and free.
struct BootDeps {
    std::function<void(std::string_view)>            term_write;
    std::function<void(std::span<const uint8_t>)>    remote_rx;
    std::function<std::pair<uint16_t, uint16_t>()>   get_pty_size;
    uint16_t cols;
    uint16_t rows;
};

void do_boot_sequence(const BootDeps& d) {
    using namespace std::string_view_literals;
    using Stage = tab5::boot_progress::Stage;
    tab5::boot_progress::clear_cancel();

#if CONFIG_TAB5_WIFI_ENABLED
    if (tab5::wifi_config::has_credentials()) {
        auto wc = tab5::wifi_config::get();
        tab5::boot_progress::set(Stage::WifiConnecting, wc.ssid);
        char line[160];
        std::snprintf(line, sizeof(line),
                      "\x1b[2mWi-Fi: connecting to %s ...\x1b[0m\r\n", wc.ssid);
        d.term_write(line);

        auto rc = tab5::wifi_sta_connect(wc.ssid, wc.psk, wc.timeout_s);
        if (tab5::boot_progress::cancel_requested()) {
            tab5::boot_progress::set(Stage::Cancelled, "wifi");
            d.term_write("\x1b[33mWi-Fi connect cancelled\x1b[0m\r\n\r\n"sv);
            return;
        }
        if (!rc) {
            tab5::boot_progress::set(Stage::Failed, "wifi");
            d.term_write("\x1b[31mWi-Fi connect failed\x1b[0m\r\n\r\n"sv);
            return;
        }
        auto st = tab5::wifi_status();
        uint8_t a = (st.ip4 >>  0) & 0xFF;
        uint8_t b = (st.ip4 >>  8) & 0xFF;
        uint8_t c = (st.ip4 >> 16) & 0xFF;
        uint8_t e = (st.ip4 >> 24) & 0xFF;
        std::snprintf(line, sizeof(line),
                      "\x1b[32mWi-Fi connected\x1b[0m  IP=%u.%u.%u.%u\r\n",
                      a, b, c, e);
        d.term_write(line);

        // VPN bring-up.
        if (tab5::vpn::kind() != tab5::vpn::Kind::None) {
            const char* kind_lbl =
                (tab5::vpn::kind() == tab5::vpn::Kind::Tailscale)
                    ? "tailscale" : "wireguard";
            tab5::boot_progress::set(Stage::VpnConnecting, kind_lbl);
            bool up = tab5::vpn::start(
                /*timeout_s=*/30,
                [](tab5::vpn::StartStage s, const char* det) {
                    switch (s) {
                    case tab5::vpn::StartStage::SyncingClock:
                        tab5::boot_progress::set(Stage::VpnSyncingClock, det);
                        break;
                    case tab5::vpn::StartStage::Connecting:
                        tab5::boot_progress::set(Stage::VpnConnecting, det);
                        break;
                    case tab5::vpn::StartStage::AwaitingAuth:
                        tab5::boot_progress::set(Stage::VpnAwaitAuth, det);
                        break;
                    }
                },
                [] { return tab5::boot_progress::cancel_requested(); });
            if (tab5::boot_progress::cancel_requested()) {
                tab5::boot_progress::set(Stage::Cancelled, "vpn");
                d.term_write("\x1b[33mVPN connect cancelled\x1b[0m\r\n\r\n"sv);
                return;
            }
            if (up) {
                char vip[32] = {0};
                if (tab5::vpn::get_tailscale_ip(vip, sizeof(vip))) {
                    std::snprintf(line, sizeof(line),
                                  "\x1b[32mTailscale up\x1b[0m  %s\r\n\r\n", vip);
                    d.term_write(line);
                } else {
                    d.term_write("\x1b[32mVPN up\x1b[0m\r\n\r\n"sv);
                }
            } else {
                char url[256] = {0};
                if (tab5::vpn::get_pending_auth_url(url, sizeof(url))) {
                    char buf[320];
                    std::snprintf(buf, sizeof(buf),
                                  "\x1b[33mTailscale awaiting approval:\x1b[0m\r\n"
                                  "  %s\r\n\r\n", url);
                    d.term_write(buf);
                } else {
                    d.term_write("\x1b[33mVPN: not up\x1b[0m\r\n\r\n"sv);
                }
            }
        }
    }
#endif

    // Remote auto-connect from the selected profile.
    if (tab5::profiles.count() == 0) {
        tab5::boot_progress::set(Stage::Done);
        return;
    }
    auto pp = tab5::profiles.get(tab5::profiles.selected());
    bool need_wifi = pp && (pp->proto == tab5::ConnProto::SSH ||
                            pp->proto == tab5::ConnProto::Telnet);
    if (need_wifi && !tab5::wifi_status().connected) pp = std::nullopt;
    if (pp && need_wifi &&
        tab5::vpn::kind() != tab5::vpn::Kind::None && !tab5::vpn::is_up()) {
        d.term_write("\x1b[33mVPN not up — skipping remote auto-connect.\r\n"
                     "Open the menu to retry once VPN comes up.\x1b[0m\r\n\r\n"sv);
        pp = std::nullopt;
    }
    if (!pp) {
        tab5::boot_progress::set(Stage::Done);
        return;
    }

    // Keep the profile alive at file scope — the connection's config
    // struct holds raw pointers into the profile's char[] fields.
    g_active_profile = *pp;
    const tab5::Profile& P = *g_active_profile;

    tab5::boot_progress::set(Stage::RemoteConnecting, P.host);
    if (tab5::boot_progress::cancel_requested()) {
        tab5::boot_progress::set(Stage::Cancelled, "remote");
        d.term_write("\x1b[33mRemote cancel — skipping\x1b[0m\r\n\r\n"sv);
        return;
    }

    char line[160];
    /* Build the IConnection unconditionally. The first start() is best-
     * effort; if it fails (transient DNS, peer not yet routable while
     * WG flips off DERP, etc.) we still hand the same connection to the
     * reconnect supervisor, which retries with exponential backoff. */
    std::unique_ptr<tab5::IConnection> c;
#if CONFIG_TAB5_SSH_ENABLED
    if (P.proto == tab5::ConnProto::SSH) {
        std::snprintf(line, sizeof(line),
                      "\x1b[2mSSH: connecting to %s@%s:%d ...\x1b[0m\r\n",
                      P.user, P.host, P.port);
        d.term_write(line);
        tab5::SshConfig scfg{
            .host = P.host,
            .port = P.port,
            .user = P.user,
            .password = P.password,
            .cols = d.cols,
            .rows = d.rows,
            .task_stack_bytes = CONFIG_TAB5_SSH_RX_TASK_STACK,
        };
        c = std::make_unique<tab5::SshConnection>(scfg);
    }
#endif
#if CONFIG_TAB5_TELNET_ENABLED
    if (!c && P.proto == tab5::ConnProto::Telnet) {
        std::snprintf(line, sizeof(line),
                      "\x1b[2mTelnet: connecting to %s:%d ...\x1b[0m\r\n",
                      P.host, P.port);
        d.term_write(line);
        tab5::TelnetConfig tcfg{
            .host = P.host,
            .port = P.port,
            .cols = d.cols,
            .rows = d.rows,
            .task_stack_bytes = CONFIG_TAB5_TELNET_RX_TASK_STACK,
        };
        c = std::make_unique<tab5::TelnetConnection>(tcfg);
    }
#endif
    if (!c && P.proto == tab5::ConnProto::UsbSerial) {
        d.term_write("\x1b[2mUSB-serial: opening FTDI ...\x1b[0m\r\n"sv);
        tab5::UsbSerialConfig ucfg{
            .baud      = CONFIG_TAB5_USB_SERIAL_BAUD,
            .data_bits = 8,
            .stop_bits = 0,
            .parity    = 0,
        };
        c = std::make_unique<tab5::UsbSerialConnection>(ucfg);
    }

    if (!c) {
        d.term_write("\x1b[31mRemote: unsupported protocol\x1b[0m\r\n"sv);
        tab5::boot_progress::set(Stage::Failed, "remote");
        return;
    }

    /* Attempt 1: in-line. */
    bool up = c->start(d.remote_rx);
    if (up) {
        std::snprintf(line, sizeof(line),
                      "\x1b[32m%s connected\x1b[0m  %s\r\n",
                      c->kind(), c->host_label());
        d.term_write(line);
        tab5::boot_progress::set(Stage::Done, "remote up");
    } else {
        std::snprintf(line, sizeof(line),
                      "\x1b[33m%s connect failed — supervisor will retry\x1b[0m\r\n",
                      c->kind());
        d.term_write(line);
        /* Still mark Done so the boot-progress overlay clears; the
         * supervisor below owns the retry loop and the status panel
         * already shows the SSH state via active_connection(). */
        tab5::boot_progress::set(Stage::Done, "remote retry");
    }

    /* Wire the connection in either way. The supervisor will keep
     * trying start() on it with the reconnect.cpp backoff policy
     * (1 s → 30 s). Once a session lands, RX bytes flow through
     * d.remote_rx and active_connection() lets keystrokes reach the
     * remote. */
    tab5::set_active_connection(c.get());
    tab5::start_reconnect_supervisor(c.get(), d.remote_rx, d.get_pty_size);
    g_conn = std::move(c);
}

void boot_task(void* arg) {
    std::unique_ptr<BootDeps> deps{static_cast<BootDeps*>(arg)};
    do_boot_sequence(*deps);
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Tab5 terminal — Phase 2 step 3 boot");

    // Initialise the default NVS partition before anything that opens
    // an NVS namespace (profiles, wifi_config, ssh TOFU). Wi-Fi setup
    // also calls this later but it's idempotent.
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
            err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);
    }

    g_mutex = xSemaphoreCreateRecursiveMutex();

    // USB host stack must be installed once, before HID / FTDI class
    // drivers register themselves. Idempotent.
    (void)tab5::start_usb_host_root();

    static tab5::M5GfxDisplay display(kCols, kRows);
    if (!display.init()) {
        ESP_LOGE(kTag, "display init failed");
        return;
    }
    ESP_LOGI(kTag, "display ready: %u cols x %u rows",
             display.cols(), display.rows());

    static term::Terminal terminal(kCols, kRows, display);
    static tab5::CursorRenderer cursor(terminal.screen(), display);
    g_cursor = &cursor;

    using namespace std::string_view_literals;
    constexpr auto kBoot =
        "\x1b[2J\x1b[H"
        "\x1b[1;32mTab5 Terminal\x1b[0m  "
        "\x1b[2m(USB-JTAG + UART input)\x1b[0m\r\n"
        "\x1b[33mtype here — Enter / Backspace work\x1b[0m\r\n"
        "\x1b[36m日本語: \x1b[1;33mあいうえお漢字\x1b[0m  "
        "\x1b[35m한글\x1b[0m  \x1b[32m中文\x1b[0m\r\n"
        "\r\n"sv;
    {
        Lock lk;
        (void)terminal.feed(kBoot);
        (void)terminal.render_dirty();
        cursor.draw();
    }

    auto term_write = [](std::string_view s) {
        Lock lk;
        cursor.erase();
        (void)terminal.feed(s);
        (void)terminal.render_dirty();
        cursor.draw();
    };

#if CONFIG_TAB5_WIFI_ENABLED
    // Wi-Fi NVS namespace init only — the actual connect runs in the
    // boot task after UI / status panel / touch are up so the user
    // sees the progress and can [Cancel].
    tab5::wifi_config::init();
#endif

#if CONFIG_TAB5_BLE_CONFIG_ENABLED
    // BLE provisioning service. Non-fatal — if the C6 slave firmware
    // wasn't built with BT controller support, nimble_port_init fails
    // and we log a warning. Profiles must be initialised first because
    // the GATT service snapshots the active profile on init() for the
    // encrypted-READ snapshot.
    tab5::profiles.init();
    if (auto rc = tab5::ble_config::start(); !rc) {
        ESP_LOGW(kTag, "BLE config service start failed (error=%d)",
                 static_cast<int>(rc.error()));
    } else {
        ESP_LOGI(kTag, "BLE config service ready — pair from settings.html");
    }
#endif

    const esp_timer_create_args_t blink_args = {
        .callback = &on_blink_tick,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "term_blink",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t blink_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&blink_args, &blink_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(blink_timer, kBlinkPeriodUs));

    auto apply = [](std::span<const uint8_t> mapped) {
        // mutex already held by caller (Lock in make_source_sink)
        if (tab5::ui_root::overlay_active()) {
            (void)terminal.feed(mapped);
            return;
        }
        cursor.erase();
        (void)terminal.feed(mapped);
        (void)terminal.render_dirty();
        cursor.draw();
    };

    tab5::ByteSink usb_sink = make_source_sink(apply);
#if CONFIG_TAB5_UART_INPUT_ENABLED
    tab5::ByteSink uart_sink = make_source_sink(apply);
#endif

    // Connection profiles live in NVS; profile 0 is seeded from Kconfig
    // on a virgin device. The boot task spawned at the end of app_main
    // picks the selected one and dials.
    tab5::profiles.init();

    // End-to-end render benchmark hook: watch the RX byte stream for
    // literal "BENCH_START\n" and "BENCH_END\n" markers and timestamp
    // them *after* terminal.render_dirty() returns. Users wrap a test
    // command with `echo BENCH_START; cmd; echo BENCH_END` and read
    // the resulting "bench took NNN ms" line from the monitor — that
    // measures the full chain (host stdout → SSH → libssh2 → terminal
    // feed → LCD render), not just host-side execution time.
    //
    // Simple position-based matcher. Both markers have no internal
    // repeating prefix, so on mismatch we only need to retry the byte
    // against the marker's first char.
    struct PerfMarker {
        const char* tag;
        const size_t len;
        size_t pos;
    };
    // No trailing newline — see comment in ssh_connection.cpp (SSH PTY
    // converts \n to \r\n, so a "...\n" match never fires).
    static PerfMarker mk_start = { "BENCH_START", 11, 0 };
    static PerfMarker mk_end   = { "BENCH_END",    9, 0 };
    static int64_t bench_start_us = 0;
    auto perf_feed_one = [](PerfMarker& m, uint8_t b) -> bool {
        if (b == static_cast<uint8_t>(m.tag[m.pos])) {
            m.pos++;
            if (m.pos == m.len) { m.pos = 0; return true; }
        } else {
            m.pos = (b == static_cast<uint8_t>(m.tag[0])) ? 1 : 0;
        }
        return false;
    };

    // RX path the connection writes incoming bytes through (held by the
    // I/O task and by the reconnect supervisor). Captures terminal /
    // cursor by reference; they're statics in app_main.
    auto remote_rx = [perf_feed_one](std::span<const uint8_t> bytes) {
        {
            Lock lk;
            if (tab5::ui_root::overlay_active()) {
                (void)terminal.feed(bytes);
            } else {
                cursor.erase();
                (void)terminal.feed(bytes);
                (void)terminal.render_dirty();
                cursor.draw();
            }
        }
        // Marker scan AFTER the render has settled. The timestamp here
        // is "this chunk is fully on the LCD".
        for (uint8_t b : bytes) {
            if (perf_feed_one(mk_start, b)) {
                bench_start_us = esp_timer_get_time();
                ESP_LOGI(kTag, "bench: START at %lld us", (long long)bench_start_us);
            }
            if (perf_feed_one(mk_end, b)) {
                int64_t now = esp_timer_get_time();
                if (bench_start_us) {
                    int64_t delta_ms = (now - bench_start_us) / 1000;
                    ESP_LOGI(kTag, "bench: END at %lld us, took %lld ms",
                             (long long)now, (long long)delta_ms);
                } else {
                    ESP_LOGI(kTag, "bench: END at %lld us (no START seen)",
                             (long long)now);
                }
            }
        }
    };
    auto get_pty_size = [] {
        Lock lk;
        return std::pair<uint16_t, uint16_t>{
            terminal.screen().cols(),
            terminal.screen().rows()};
    };

    tab5::start_status_bar([](std::function<void()> body) {
        Lock lk;
        body();
    });

    // Soft keyboard. The byte sink writes through usb_sink, which itself
    // self-routes to active_connection() when one is up and falls back
    // to the local-echo path otherwise — no rebind needed.
    static tab5::SoftKeyboard kbd([&](std::span<const uint8_t> bytes) {
        usb_sink(bytes);
    });
    namespace ui = tab5::ui_root;

    // --- Layer registry ----------------------------------------------
    // z=10 terminal+cursor (always visible, rows depend on kbd state)
    ui::register_layer(10, [](const ui::Rect& d) {
        // Terminal grid is centred at x=160..1120; height tracks the
        // current screen row count.
        int term_h = terminal.screen().rows() * 24;
        ui::Rect tb = {160, 0, 1120, term_h};
        auto is = ui::intersect(d, tb);
        if (ui::empty(is)) return;
        int r0 = std::max(0, is.y0 / 24);
        int r1 = std::min<int>(terminal.screen().rows(), (is.y1 + 23) / 24);
        int c0 = std::max(0, (is.x0 - 160) / 12);
        int c1 = std::min<int>(terminal.screen().cols(),
                               (is.x1 - 160 + 11) / 12);
        if (r0 < r1 && c0 < c1) {
            terminal.screen().mark_region(static_cast<uint16_t>(r0),
                                          static_cast<uint16_t>(c0),
                                          static_cast<uint16_t>(r1),
                                          static_cast<uint16_t>(c1));
        }
        cursor.erase();
        (void)terminal.render_dirty();
        if (!ui::overlay_active()) cursor.draw();
    });

    // z=20 right-margin status panel
    ui::register_layer(20, [](const ui::Rect& d) {
        ui::Rect sb = {tab5::kStatusPanelX, 0,
                       tab5::kStatusPanelX + tab5::kStatusPanelW, 720};
        if (ui::empty(ui::intersect(d, sb))) return;
        tab5::status_render();
    });

    // z=30 always-visible left-margin buttons (kbd toggle + Menu)
    ui::register_layer(30, [](const ui::Rect& d) {
        auto b = kbd.buttons_rect();
        ui::Rect br = {b.x0, b.y0, b.x1, b.y1};
        if (ui::empty(ui::intersect(d, br))) return;
        kbd.render_buttons();
    });

    // z=40 kbd panel (only when visible)
    ui::register_layer(40, [](const ui::Rect& d) {
        if (!kbd.visible()) return;
        auto p = kbd.panel_rect();
        ui::Rect pr = {p.x0, p.y0, p.x1, p.y1};
        if (ui::empty(ui::intersect(d, pr))) return;
        kbd.render_panel();
    });

    // z=100 modal menu (paints full screen when visible)
    ui::register_layer(100, [](const ui::Rect&) {
        if (tab5::menu.visible()) tab5::menu.render();
    });

    // --- Visibility-transition repaints ------------------------------
    // Soft keyboard show/hide: resize the terminal grid (and the remote
    // pty via SSH WINCH) so cells stay above the panel; then invalidate
    // the panel region so the compositor repaints whatever's exposed.
    static auto repaint_kbd = [] {
        uint16_t want_rows = kbd.visible() ? kRowsKbd : kRows;
        if (terminal.screen().rows() != want_rows) {
            terminal.resize(kCols, want_rows);
            display.set_grid_size(kCols, want_rows);
            if (auto* c = tab5::active_connection()) {
                c->resize(kCols, want_rows);
            }
        }
        auto p = kbd.panel_rect();
        ui::invalidate({p.x0, p.y0, p.x1, p.y1});
    };
    kbd.set_repaint(repaint_kbd);

    // Menu open/close: full-screen invalidate. The overlay_active flag
    // also gates terminal RX rendering and cursor blink so they don't
    // bleed through the modal.
    static auto repaint_menu = [] {
        ui::set_overlay_active(tab5::menu.visible());
        ui::invalidate(ui::kFullScreen);
    };
    tab5::menu.set_repaint(repaint_menu);
    tab5::menu.bind_keyboard(&kbd);
    kbd.set_on_menu([] { tab5::menu.open(); });

    // Initial paint of every layer in z order.
    { Lock lk; ui::invalidate(ui::kFullScreen); }

    tab5::start_touch_input([](const tab5::TouchPoint& p) {
        // Boot-progress [Cancel] runs first so a tap on the status-panel
        // button during a long Wi-Fi / VPN / SSH connect is captured
        // before the menu / keyboard get a chance. handle_touch already
        // hit-tests against the published button rect.
        if (tab5::boot_progress::handle_touch(p)) return;
        Lock lk;
        if (tab5::menu.visible() && tab5::menu.handle_touch(p)) return;
        // Menu may return false to let the soft keyboard receive the
        // tap while the profile editor is up (keyboard area).
        kbd.handle_touch(p);
    });

    if (auto rc = tab5::start_usb_jtag_input(usb_sink); !rc) {
        ESP_LOGE(kTag, "USB-JTAG input start failed");
    }

#if CONFIG_TAB5_UART_INPUT_ENABLED
    tab5::UartInputConfig uart_cfg{
        .port = CONFIG_TAB5_UART_INPUT_PORT,
        .tx_gpio = CONFIG_TAB5_UART_INPUT_TX_GPIO,
        .rx_gpio = CONFIG_TAB5_UART_INPUT_RX_GPIO,
        .baud = CONFIG_TAB5_UART_INPUT_BAUD,
    };
    if (auto rc = tab5::start_uart_input(uart_cfg, uart_sink); !rc) {
        ESP_LOGE(kTag, "UART input start failed");
    }
#endif

    // USB HID keyboard plugged into the Tab5 USB-A port. Shares the
    // same byte sink as USB-JTAG so keystrokes go through whichever
    // input filter / SSH connection the others use.
#if CONFIG_TAB5_USB_HID_INPUT_ENABLED
    if (auto rc = tab5::start_usb_hid_input(usb_sink); !rc) {
        ESP_LOGE(kTag, "USB HID input start failed");
    }
#endif

    // Tab5 clip-on keyboard on ExtPort1 (I2C 0x6D). Shares the same
    // byte sink as USB-JTAG / UART / USB-HID — the HID-mode events are
    // translated through main/hid_translate, so a single mapping covers
    // both physical keyboards.
#if CONFIG_TAB5_KEYBOARD_INPUT_ENABLED
    if (auto rc = tab5::start_tab5_kbd_input(usb_sink); !rc) {
        ESP_LOGE(kTag, "Tab5 keyboard input start failed");
    }
#endif

    // Boot sequence (Wi-Fi → VPN → remote auto-connect) runs on its own
    // task so app_main can return to the idle loop and the user can see
    // the status-panel progress / tap [Cancel] from second one. 6 KB
    // stack covers the Wi-Fi STA assoc + SNTP + Tailscale handshake
    // path; the SSH / Telnet dial spawns its own task internally.
    {
        auto* deps = new BootDeps{
            .term_write   = term_write,
            .remote_rx    = remote_rx,
            .get_pty_size = get_pty_size,
            .cols         = kCols,
            .rows         = kRows,
        };
        BaseType_t ok = xTaskCreate(&boot_task, "boot_seq", 6144, deps,
                                    tskIDLE_PRIORITY + 2, nullptr);
        if (ok != pdPASS) {
            ESP_LOGE(kTag, "boot task create failed");
            delete deps;
        }
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
