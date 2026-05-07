// Phase 1a entry point.
//
// term_core is exercised on-device via a no-op IDisplay so we can prove the
// component links cleanly under ESP-IDF and that the C++23 toolchain is
// healthy. Actual LCD bring-up (Phase 1b) will replace NullDisplay with a
// real esp_lcd / M5Unified-backed implementation.

#include <cstdint>
#include <span>
#include <string_view>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "term_core/idisplay.hpp"
#include "term_core/terminal.hpp"

namespace {

constexpr const char* kTag = "tab5_term";

class NullDisplay final : public term::IDisplay {
public:
    NullDisplay(uint16_t cols, uint16_t rows) : cols_(cols), rows_(rows) {}
    term::Result<void> init() override { return {}; }
    uint16_t cols() const override { return cols_; }
    uint16_t rows() const override { return rows_; }
    term::Result<void> draw_cells(uint16_t row, uint16_t col,
                                  std::span<const term::Cell> cells) override {
        (void)row; (void)col; (void)cells;
        return {};
    }
    term::Result<void> flush(term::DamageRect) override { return {}; }

private:
    uint16_t cols_;
    uint16_t rows_;
};

void log_first_row(const term::Screen& s) {
    char line[81];
    uint16_t cols = s.cols() < 80 ? s.cols() : 80;
    for (uint16_t c = 0; c < cols; ++c) {
        char32_t cp = s.at(0, c).ch;
        line[c] = (cp >= 0x20 && cp < 0x7F) ? static_cast<char>(cp) : '.';
    }
    line[cols] = '\0';
    ESP_LOGI(kTag, "row 0: |%s|", line);
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Tab5 terminal — Phase 1a boot");

    constexpr uint16_t kCols = 80;
    constexpr uint16_t kRows = 30;
    NullDisplay display(kCols, kRows);
    term::Terminal terminal(kCols, kRows, display);

    using namespace std::string_view_literals;
    constexpr auto kBoot =
        "\x1b[2J\x1b[H"
        "\x1b[1;32mTab5 Terminal Phase 1a\x1b[0m\r\n"
        "\x1b[33mhello, world\x1b[0m\r\n"sv;

    (void)terminal.feed(kBoot);
    (void)terminal.render_dirty();

    log_first_row(terminal.screen());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
