#include "usb_host_root.hpp"

#include <atomic>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace tab5 {

namespace {

constexpr const char* kTag = "usb_root";

std::atomic<bool> g_installed{false};

void usb_host_task(void*) {
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

}  // namespace

term::Result<void> start_usb_host_root() {
    if (g_installed.load()) return {};

    usb_host_config_t cfg{};
    cfg.skip_phy_setup = false;
    cfg.root_port_unpowered = false;
    cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "usb_host_install -> %s", esp_err_to_name(err));
        return std::unexpected(term::Error::NotInitialized);
    }

    if (xTaskCreate(usb_host_task, "usb_host", 4096, nullptr,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "usb_host_task create failed");
        return std::unexpected(term::Error::NotInitialized);
    }

    g_installed.store(true);
    ESP_LOGI(kTag, "USB host library up");
    return {};
}

}  // namespace tab5
