#include "input_touch.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "M5Unified.h"

namespace tab5 {

namespace {

constexpr const char* kTag      = "touch";
constexpr TickType_t kPollMs    = 16;   // ~60 Hz

TouchSink g_sink;

void touch_task(void*) {
    bool last_pressed = false;
    int16_t last_x = 0;
    int16_t last_y = 0;

    for (;;) {
        M5.update();

        bool pressed = false;
        int16_t x = last_x, y = last_y;
        if (M5.Touch.getCount() > 0) {
            const auto& d = M5.Touch.getDetail(0);
            if (d.isPressed()) {
                pressed = true;
                x = d.x;
                y = d.y;
            }
        }

        if (pressed && !last_pressed) {
            if (g_sink) g_sink({TouchEvent::Down, x, y});
        } else if (pressed && (x != last_x || y != last_y)) {
            if (g_sink) g_sink({TouchEvent::Move, x, y});
        } else if (!pressed && last_pressed) {
            if (g_sink) g_sink({TouchEvent::Up, last_x, last_y});
        }

        last_pressed = pressed;
        last_x = x;
        last_y = y;
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

}  // namespace

void start_touch_input(TouchSink sink) {
    if (!M5.Touch.isEnabled()) {
        ESP_LOGW(kTag, "touch not enabled — Touch_Class not bound to a gfx device");
    }
    g_sink = std::move(sink);
    BaseType_t rc = xTaskCreate(&touch_task, "touch", 4096, nullptr,
                                tskIDLE_PRIORITY + 2, nullptr);
    if (rc != pdPASS) {
        ESP_LOGE(kTag, "task create failed");
    }
}

}  // namespace tab5
