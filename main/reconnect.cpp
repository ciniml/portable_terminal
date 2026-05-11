#include "reconnect.hpp"

#include <algorithm>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tab5 {

namespace {

constexpr const char* kTag = "reconnect";

struct SupervisorArgs {
    IConnection*    conn;
    ByteSink        rx_sink;
    GridSizeGetter  get_size;
    ReconnectConfig cfg;
};

void supervisor_task(void* raw) {
    auto* args = static_cast<SupervisorArgs*>(raw);
    uint32_t backoff = args->cfg.initial_backoff_ms;
    bool was_connected = args->conn->is_connected();

    for (;;) {
        bool now = args->conn->is_connected();
        if (was_connected && !now) {
            ESP_LOGW(kTag, "%s session dropped — will reconnect",
                     args->conn->kind());
            backoff = args->cfg.initial_backoff_ms;
        }
        if (!now) {
            vTaskDelay(pdMS_TO_TICKS(backoff));
            ESP_LOGI(kTag, "attempting %s reconnect", args->conn->kind());
            if (args->conn->start(args->rx_sink)) {
                ESP_LOGI(kTag, "%s reconnected", args->conn->kind());
                if (args->get_size) {
                    auto [cols, rows] = args->get_size();
                    args->conn->resize(cols, rows);
                }
                backoff = args->cfg.initial_backoff_ms;
            } else {
                backoff = std::min(backoff * args->cfg.backoff_factor,
                                   args->cfg.max_backoff_ms);
                ESP_LOGW(kTag, "reconnect failed; next attempt in %lu ms",
                         static_cast<unsigned long>(backoff));
            }
        }
        was_connected = args->conn->is_connected();
        vTaskDelay(pdMS_TO_TICKS(args->cfg.poll_period_ms));
    }
}

}  // namespace

void start_reconnect_supervisor(IConnection* conn,
                                ByteSink rx_sink,
                                GridSizeGetter get_size,
                                const ReconnectConfig& cfg) {
    auto* args = new SupervisorArgs{conn, std::move(rx_sink),
                                    std::move(get_size), cfg};
    BaseType_t rc = xTaskCreate(&supervisor_task, "reconnect",
                                cfg.task_stack_bytes, args,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (rc != pdPASS) {
        ESP_LOGE(kTag, "supervisor task spawn failed");
        delete args;
    }
}

}  // namespace tab5
