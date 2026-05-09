#include "input_usb_jtag.hpp"

#include <cstdint>
#include <span>
#include <utility>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tab5 {

namespace {

constexpr const char* kTag = "tab5_usbjtag";
constexpr size_t kRxBufSize = 1024;
constexpr size_t kReadChunk = 128;
constexpr uint32_t kReadTimeoutMs = 100;

struct ReaderCtx {
    ByteSink sink;
};

[[noreturn]] void reader_task(void* arg) {
    auto* ctx = static_cast<ReaderCtx*>(arg);
    uint8_t buf[kReadChunk];
    while (true) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf),
                                           pdMS_TO_TICKS(kReadTimeoutMs));
        if (n > 0) {
            ctx->sink(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
        }
    }
}

}  // namespace

term::Result<void> start_usb_jtag_input(ByteSink sink) {
    if (!sink) {
        return std::unexpected(term::Error::NotInitialized);
    }
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = kRxBufSize;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "driver install failed: %s", esp_err_to_name(err));
        return std::unexpected(term::Error::BackendError);
    }

    static ReaderCtx ctx;
    ctx.sink = std::move(sink);

    constexpr uint32_t kStack = 4096;
    constexpr UBaseType_t kPrio = 5;
    BaseType_t ok = xTaskCreate(reader_task, "usb_jtag_in", kStack, &ctx,
                                kPrio, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "task create failed");
        return std::unexpected(term::Error::BackendError);
    }
    ESP_LOGI(kTag, "USB-Serial-JTAG input reader started");
    return {};
}

}  // namespace tab5
