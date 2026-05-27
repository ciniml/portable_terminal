// Phase 2 step 3: byte input from USB-Serial-JTAG and an external UART
// (Tab5 Port-A by default) flow into the VT100 core. Each source has
// its own cooked-input filter so cross-source CR/LF state stays
// independent. Cursor renderer + 1 Hz blink unchanged from step 2.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "byte_input.hpp"
#include "cooked_input.hpp"
#include "cursor_renderer.hpp"
#include "display_m5gfx.hpp"
#include "input_touch.hpp"
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
// (USB-JTAG, UART, future Telnet/SSH) tracks CR/LF state independently.
// The sink itself locks the global mutex and forwards through the
// shared terminal + cursor.
template <class TerminalApply>
tab5::ByteSink make_source_sink(TerminalApply&& apply) {
    auto state = std::make_shared<tab5::CookedInputFilter>();
    return [state, apply](std::span<const uint8_t> bytes) mutable {
        std::vector<uint8_t> mapped;
        mapped.reserve(bytes.size() * 3);
        state->process(bytes, mapped);
        Lock lk;
        apply(std::span<const uint8_t>(mapped.data(), mapped.size()));
    };
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
    tab5::wifi_config::init();
    if (tab5::wifi_config::has_credentials()) {
        auto wc = tab5::wifi_config::get();
        char line[128];
        snprintf(line, sizeof(line),
                 "\x1b[2mWi-Fi: connecting to %s ...\x1b[0m\r\n", wc.ssid);
        term_write(line);

        auto rc = tab5::wifi_sta_connect(wc.ssid, wc.psk, wc.timeout_s);
        if (rc) {
            auto st = tab5::wifi_status();
            uint8_t a = (st.ip4 >>  0) & 0xFF;
            uint8_t b = (st.ip4 >>  8) & 0xFF;
            uint8_t c = (st.ip4 >> 16) & 0xFF;
            uint8_t d = (st.ip4 >> 24) & 0xFF;
            snprintf(line, sizeof(line),
                     "\x1b[32mWi-Fi connected\x1b[0m  IP=%u.%u.%u.%u\r\n",
                     a, b, c, d);
            term_write(line);

            // Bring up the VPN (Tailscale or WireGuard, depending on
            // Kconfig). No-op if disabled at compile time.
            if (tab5::vpn::start(/*timeout_s=*/30)) {
                char vip[32] = {0};
                if (tab5::vpn::get_tailscale_ip(vip, sizeof(vip))) {
                    snprintf(line, sizeof(line),
                             "\x1b[32mTailscale up\x1b[0m  %s\r\n\r\n", vip);
                    term_write(line);
                } else {
                    term_write("\x1b[32mVPN up\x1b[0m\r\n\r\n"sv);
                }
            } else {
                char url[256] = {0};
                if (tab5::vpn::get_pending_auth_url(url, sizeof(url))) {
                    char buf[320];
                    snprintf(buf, sizeof(buf),
                             "\x1b[33mTailscale awaiting approval:\x1b[0m\r\n"
                             "  %s\r\n\r\n", url);
                    term_write(buf);
                } else if (tab5::vpn::kind() != tab5::vpn::Kind::None) {
                    term_write("\x1b[33mVPN: not up\x1b[0m\r\n\r\n"sv);
                } else {
                    term_write("\r\n"sv);
                }
            }
        } else {
            term_write("\x1b[31mWi-Fi connect failed\x1b[0m\r\n\r\n"sv);
        }
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
    // on a virgin device. Pick the selected one for boot auto-connect.
    tab5::profiles.init();

    static std::unique_ptr<tab5::IConnection> conn;
    if (tab5::profiles.count() > 0) {
        auto pp = tab5::profiles.get(tab5::profiles.selected());
        // SSH / Telnet need Wi-Fi; USB-serial doesn't.
        bool need_wifi = pp && (pp->proto == tab5::ConnProto::SSH ||
                                pp->proto == tab5::ConnProto::Telnet);
        if (need_wifi && !tab5::wifi_status().connected) {
            pp = std::nullopt;
        }
        // When a VPN backend is compiled in, gate the remote auto-connect
        // on it being up — tailnet hostname resolution and routes only
        // work once Tailscale / WireGuard is established. USB-serial is
        // out-of-band and doesn't care.
        if (pp && need_wifi &&
            tab5::vpn::kind() != tab5::vpn::Kind::None &&
            !tab5::vpn::is_up()) {
            term_write("\x1b[33mVPN not up — skipping remote auto-connect.\r\n"
                       "Open the menu to retry once VPN comes up.\x1b[0m\r\n\r\n"sv);
            pp = std::nullopt;
        }

        auto remote_rx = [](std::span<const uint8_t> bytes) {
            Lock lk;
            if (tab5::ui_root::overlay_active()) {
                (void)terminal.feed(bytes);
                return;
            }
            cursor.erase();
            (void)terminal.feed(bytes);
            (void)terminal.render_dirty();
            cursor.draw();
        };

        if (false) {}
#if CONFIG_TAB5_SSH_ENABLED
        else if (pp && pp->proto == tab5::ConnProto::SSH) {
            char line[160];
            snprintf(line, sizeof(line),
                     "\x1b[2mSSH: connecting to %s@%s:%d ...\x1b[0m\r\n",
                     pp->user, pp->host, pp->port);
            term_write(line);
            tab5::SshConfig scfg{
                .host = pp->host,
                .port = pp->port,
                .user = pp->user,
                .password = pp->password,
                .cols = kCols,
                .rows = kRows,
                .task_stack_bytes = CONFIG_TAB5_SSH_RX_TASK_STACK,
            };
            auto c = std::make_unique<tab5::SshConnection>(scfg);
            if (c->start(remote_rx)) conn = std::move(c);
        }
#endif
#if CONFIG_TAB5_TELNET_ENABLED
        else if (pp && pp->proto == tab5::ConnProto::Telnet) {
            char line[160];
            snprintf(line, sizeof(line),
                     "\x1b[2mTelnet: connecting to %s:%d ...\x1b[0m\r\n",
                     pp->host, pp->port);
            term_write(line);
            tab5::TelnetConfig tcfg{
                .host = pp->host,
                .port = pp->port,
                .cols = kCols,
                .rows = kRows,
                .task_stack_bytes = CONFIG_TAB5_TELNET_RX_TASK_STACK,
            };
            auto c = std::make_unique<tab5::TelnetConnection>(tcfg);
            if (c->start(remote_rx)) conn = std::move(c);
        }
#endif
        else if (pp && pp->proto == tab5::ConnProto::UsbSerial) {
            char line[160];
            snprintf(line, sizeof(line),
                     "\x1b[2mUSB-serial: opening FTDI ...\x1b[0m\r\n");
            term_write(line);
            tab5::UsbSerialConfig ucfg{
                .baud      = CONFIG_TAB5_USB_SERIAL_BAUD,
                .data_bits = 8,
                .stop_bits = 0,   // 1 stop bit
                .parity    = 0,   // none
            };
            auto c = std::make_unique<tab5::UsbSerialConnection>(ucfg);
            if (c->start(remote_rx)) conn = std::move(c);
        }

        if (conn) {
            char line[160];
            snprintf(line, sizeof(line),
                     "\x1b[32m%s connected\x1b[0m  %s\r\n",
                     conn->kind(), conn->host_label());
            term_write(line);
            tab5::set_active_connection(conn.get());

            // Supervisor: re-call start() on the same IConnection if the
            // session drops (Wi-Fi blip, server reboot, idle timeout
            // hitting TCP keepalive). Replays the current pty size so
            // the remote starts at the right geometry after reconnect.
            tab5::start_reconnect_supervisor(
                conn.get(), remote_rx,
                [] {
                    Lock lk;
                    return std::pair<uint16_t, uint16_t>{
                        terminal.screen().cols(),
                        terminal.screen().rows()};
                });
            // While the remote session is up, bypass the cooked-input
            // filter — the remote echoes / handles BS itself, so doing
            // CR→CRLF / BS-Space-BS locally would double-edit.
            auto remote_send = [](std::span<const uint8_t> bytes) {
                if (auto* c = tab5::active_connection()) c->send(bytes);
            };
            usb_sink = remote_send;
#if CONFIG_TAB5_UART_INPUT_ENABLED
            uart_sink = remote_send;
#endif
        } else if (pp) {
            term_write("\x1b[31mRemote connect failed\x1b[0m  "
                       "(falling back to local echo)\r\n");
        }
    }

    tab5::start_status_bar([](std::function<void()> body) {
        Lock lk;
        body();
    });

    // Soft keyboard. The byte sink writes through whatever usb_sink points
    // at — SSH when connected, local-echo otherwise. (Both already bypass
    // the cooked filter where appropriate.) Captured by reference so a
    // post-boot sink change would be visible, though in practice we set it
    // once above.
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

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
