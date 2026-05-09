// Phase 2 step 2: USB-Serial-JTAG byte input flows into the VT100 core,
// with cooked-mode mapping (Enter -> CRLF, BS/DEL -> visible erase) and
// a blinking block cursor at the terminal cursor position.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cooked_input.hpp"
#include "cursor_renderer.hpp"
#include "display_m5gfx.hpp"
#include "input_usb_jtag.hpp"
#include "term_core/terminal.hpp"

namespace {

constexpr const char* kTag = "tab5_term";
constexpr uint16_t kCols = 80;
constexpr uint16_t kRows = 30;
constexpr int64_t kBlinkPeriodUs = 500 * 1000;

// Single mutex serialises terminal / cursor / display access between the
// USB-JTAG input task and the esp_timer task that drives the cursor blink.
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

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "Tab5 terminal — Phase 2 step 2 boot");

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
    static tab5::CookedInputFilter cooked;
    g_cursor = &cursor;

    using namespace std::string_view_literals;
    constexpr auto kBoot =
        "\x1b[2J\x1b[H"
        "\x1b[1;32mTab5 Terminal\x1b[0m  \x1b[2m(USB-JTAG input)\x1b[0m\r\n"
        "\x1b[33mtype here; Enter / Backspace work\x1b[0m\r\n"
        "\r\n"sv;
    {
        Lock lk;
        (void)terminal.feed(kBoot);
        (void)terminal.render_dirty();
        cursor.draw();
    }

    // Cursor blink (500 ms half-period -> 1 Hz visible flash).
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

    auto sink = [](std::span<const uint8_t> bytes) {
        static std::vector<uint8_t> mapped;
        mapped.clear();
        cooked.process(bytes, mapped);

        Lock lk;
        cursor.erase();
        (void)terminal.feed(std::span<const uint8_t>(mapped.data(),
                                                     mapped.size()));
        (void)terminal.render_dirty();
        cursor.draw();
    };
    if (auto rc = tab5::start_usb_jtag_input(sink); !rc) {
        ESP_LOGE(kTag, "USB-JTAG input start failed");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
