// Phase 1b entry point: initialize Tab5 LCD via M5Unified, drive the VT100
// core through it, and display a fixed boot string.

#include <cstdint>
#include <string_view>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display_m5gfx.hpp"
#include "term_core/terminal.hpp"

namespace {
constexpr const char* kTag = "tab5_term";
constexpr uint16_t kCols = 80;
constexpr uint16_t kRows = 30;
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Tab5 terminal — Phase 1b boot");

    static tab5::M5GfxDisplay display(kCols, kRows);
    if (!display.init()) {
        ESP_LOGE(kTag, "display init failed");
        return;
    }
    ESP_LOGI(kTag, "display ready: %u cols x %u rows", display.cols(), display.rows());

    static term::Terminal terminal(kCols, kRows, display);

    using namespace std::string_view_literals;
    constexpr auto kBoot =
        "\x1b[2J\x1b[H"
        "\x1b[1;32mTab5 Terminal Phase 1b\x1b[0m\r\n"
        "\x1b[33mhello, world\x1b[0m\r\n"
        "\r\n"
        "  \x1b[31m red \x1b[32mgreen \x1b[33myellow \x1b[34mblue "
        "\x1b[35mmagenta \x1b[36mcyan \x1b[37mwhite\x1b[0m\r\n"
        "  \x1b[1;31mbold-red \x1b[7minverse\x1b[0m\r\n"sv;

    (void)terminal.feed(kBoot);
    (void)terminal.render_dirty();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
