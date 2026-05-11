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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "byte_input.hpp"
#include "cooked_input.hpp"
#include "cursor_renderer.hpp"
#include "display_m5gfx.hpp"
#include "input_usb_jtag.hpp"
#if CONFIG_TAB5_UART_INPUT_ENABLED
#include "input_uart.hpp"
#endif
#if CONFIG_TAB5_WIFI_ENABLED
#include "wifi_setup.hpp"
#endif
#include "term_core/terminal.hpp"

namespace {

constexpr const char* kTag = "tab5_term";
constexpr uint16_t kCols = 80;
constexpr uint16_t kRows = 30;
constexpr int64_t kBlinkPeriodUs = 500 * 1000;

SemaphoreHandle_t g_mutex = nullptr;
tab5::CursorRenderer* g_cursor = nullptr;

class Lock {
public:
    Lock() { xSemaphoreTake(g_mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_mutex); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

void on_blink_tick(void* /*arg*/) {
    if (!g_cursor) return;
    Lock lk;
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

    g_mutex = xSemaphoreCreateMutex();

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
    {
        char line[128];
        snprintf(line, sizeof(line),
                 "\x1b[2mWi-Fi: connecting to %s ...\x1b[0m\r\n",
                 CONFIG_TAB5_WIFI_SSID);
        term_write(line);

        auto rc = tab5::wifi_sta_connect(CONFIG_TAB5_WIFI_SSID,
                                         CONFIG_TAB5_WIFI_PSK,
                                         CONFIG_TAB5_WIFI_CONNECT_TIMEOUT_S);
        if (rc) {
            auto st = tab5::wifi_status();
            uint8_t a = (st.ip4 >>  0) & 0xFF;
            uint8_t b = (st.ip4 >>  8) & 0xFF;
            uint8_t c = (st.ip4 >> 16) & 0xFF;
            uint8_t d = (st.ip4 >> 24) & 0xFF;
            snprintf(line, sizeof(line),
                     "\x1b[32mWi-Fi connected\x1b[0m  IP=%u.%u.%u.%u\r\n\r\n",
                     a, b, c, d);
            term_write(line);
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
        cursor.erase();
        (void)terminal.feed(mapped);
        (void)terminal.render_dirty();
        cursor.draw();
    };

    if (auto rc = tab5::start_usb_jtag_input(make_source_sink(apply)); !rc) {
        ESP_LOGE(kTag, "USB-JTAG input start failed");
    }

#if CONFIG_TAB5_UART_INPUT_ENABLED
    tab5::UartInputConfig uart_cfg{
        .port = CONFIG_TAB5_UART_INPUT_PORT,
        .tx_gpio = CONFIG_TAB5_UART_INPUT_TX_GPIO,
        .rx_gpio = CONFIG_TAB5_UART_INPUT_RX_GPIO,
        .baud = CONFIG_TAB5_UART_INPUT_BAUD,
    };
    if (auto rc = tab5::start_uart_input(uart_cfg, make_source_sink(apply));
        !rc) {
        ESP_LOGE(kTag, "UART input start failed");
    }
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
