#include "input_uart.hpp"

#include <cstdint>
#include <span>
#include <utility>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tab5 {

namespace {

constexpr const char* kTag = "tab5_uart_in";
constexpr int kRxBufSize = 1024;
constexpr int kTxBufSize = 0;             // we don't transmit yet
constexpr size_t kReadChunk = 128;
constexpr uint32_t kReadTimeoutMs = 100;

struct ReaderCtx {
    uart_port_t port;
    ByteSink sink;
};

[[noreturn]] void reader_task(void* arg) {
    auto* ctx = static_cast<ReaderCtx*>(arg);
    uint8_t buf[kReadChunk];
    while (true) {
        int n = uart_read_bytes(ctx->port, buf, sizeof(buf),
                                pdMS_TO_TICKS(kReadTimeoutMs));
        if (n > 0) {
            ctx->sink(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
        }
    }
}

}  // namespace

term::Result<void> start_uart_input(const UartInputConfig& cfg, ByteSink sink) {
    if (!sink) {
        return std::unexpected(term::Error::NotInitialized);
    }

    uart_port_t port = static_cast<uart_port_t>(cfg.port);

    uart_config_t uart_cfg{};
    uart_cfg.baud_rate = cfg.baud;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(port, kRxBufSize, kTxBufSize,
                                        0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_driver_install(%d): %s", port,
                 esp_err_to_name(err));
        return std::unexpected(term::Error::BackendError);
    }
    ESP_ERROR_CHECK(uart_param_config(port, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(port, cfg.tx_gpio, cfg.rx_gpio,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    static ReaderCtx ctx;
    ctx.port = port;
    ctx.sink = std::move(sink);

    constexpr uint32_t kStack = 4096;
    constexpr UBaseType_t kPrio = 5;
    BaseType_t ok = xTaskCreate(reader_task, "uart_in", kStack, &ctx, kPrio,
                                nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "task create failed");
        return std::unexpected(term::Error::BackendError);
    }
    ESP_LOGI(kTag, "UART%d reader started: tx=%d rx=%d %d baud",
             port, cfg.tx_gpio, cfg.rx_gpio, cfg.baud);
    return {};
}

}  // namespace tab5
