// Phase 2 step 1: bring up the LCD, drive the VT100 core, and forward
// USB-Serial-JTAG bytes from the host TTY into terminal.feed(). Anything
// typed in `idf.py monitor` (or picocom on /dev/ttyACMx) appears on the
// Tab5 screen.

#include <cstdint>
#include <span>
#include <string_view>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display_m5gfx.hpp"
#include "input_usb_jtag.hpp"
#include "term_core/terminal.hpp"

namespace {

constexpr const char* kTag = "tab5_term";
constexpr uint16_t kCols = 80;
constexpr uint16_t kRows = 30;

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Tab5 terminal — Phase 2 step 1 boot");

    static tab5::M5GfxDisplay display(kCols, kRows);
    if (!display.init()) {
        ESP_LOGE(kTag, "display init failed");
        return;
    }
    ESP_LOGI(kTag, "display ready: %u cols x %u rows",
             display.cols(), display.rows());

    static term::Terminal terminal(kCols, kRows, display);

    using namespace std::string_view_literals;
    constexpr auto kBoot =
        "\x1b[2J\x1b[H"
        "\x1b[1;32mTab5 Terminal\x1b[0m  \x1b[2m(USB-JTAG input)\x1b[0m\r\n"
        "\x1b[33mtype on the host TTY; bytes flow here\x1b[0m\r\n"
        "\r\n"sv;
    (void)terminal.feed(kBoot);
    (void)terminal.render_dirty();

    auto sink = [](std::span<const uint8_t> bytes) {
        (void)terminal.feed(bytes);
        (void)terminal.render_dirty();
    };
    if (auto rc = tab5::start_usb_jtag_input(sink); !rc) {
        ESP_LOGE(kTag, "USB-JTAG input start failed");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
