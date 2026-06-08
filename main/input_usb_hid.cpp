#include "input_usb_hid.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/hid.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#include "hid_translate.hpp"   // shared scancode → terminal-byte translator

namespace tab5 {

namespace {

constexpr const char* kTag = "usb_hid";

ByteSink g_sink;

// Connected keyboard interface. Multiple simultaneous keyboards aren't
// supported in this minimal driver.
hid_host_device_handle_t g_dev = nullptr;

// Previous keycode set, for newly-pressed-key detection.
uint8_t g_prev_keys[6] = {0};
uint8_t g_prev_mod     = 0;

// Look for `code` in a 6-key array.
bool contains(const uint8_t arr[6], uint8_t code) {
    for (int i = 0; i < 6; ++i) if (arr[i] == code) return true;
    return false;
}

// Translate one newly-pressed key into wire bytes and push to the sink.
// Mapping lives in hid_translate (shared with the Tab5 Keyboard driver).
void emit_key(uint8_t code, uint8_t mod) {
    if (!g_sink) return;
    uint8_t buf[hid_translate::kMaxBytes];
    size_t n = hid_translate::emit_key(code, mod, buf);
    if (n) g_sink(std::span<const uint8_t>(buf, n));
}

void interface_event_cb(hid_host_device_handle_t hid_dev_handle,
                        hid_host_interface_event_t event, void*) {
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t buf[16] = {0};
        size_t  got = 0;
        esp_err_t err = hid_host_device_get_raw_input_report_data(
            hid_dev_handle, buf, sizeof(buf), &got);
        if (err != ESP_OK || got < 8) return;
        uint8_t mod = buf[0];
        const uint8_t* keys = &buf[2];   // bytes 2..7 are keycodes
        for (int i = 0; i < 6; ++i) {
            uint8_t code = keys[i];
            if (code == 0 || code == 0x01 /* ErrorRollOver */) continue;
            if (!contains(g_prev_keys, code)) {
                emit_key(code, mod);
            }
        }
        memcpy(g_prev_keys, keys, 6);
        g_prev_mod = mod;
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        ESP_LOGI(kTag, "keyboard disconnected");
        hid_host_device_close(hid_dev_handle);
        if (hid_dev_handle == g_dev) g_dev = nullptr;
        memset(g_prev_keys, 0, sizeof(g_prev_keys));
        g_prev_mod = 0;
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(kTag, "transfer error");
    }
}

void driver_event_cb(hid_host_device_handle_t hid_dev_handle,
                     hid_host_driver_event_t event, void*) {
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;
    hid_host_dev_params_t params{};
    if (hid_host_device_get_params(hid_dev_handle, &params) != ESP_OK) return;
    ESP_LOGI(kTag, "HID connected: subclass=%d proto=%d iface=%d",
             params.sub_class, params.proto, params.iface_num);
    if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE ||
        params.proto    != HID_PROTOCOL_KEYBOARD) {
        ESP_LOGI(kTag, "not a boot-protocol keyboard — ignoring");
        return;
    }
    hid_host_device_config_t dcfg{
        .callback = interface_event_cb,
        .callback_arg = nullptr,
    };
    if (hid_host_device_open(hid_dev_handle, &dcfg) != ESP_OK) {
        ESP_LOGE(kTag, "device_open failed");
        return;
    }
    // Request boot protocol explicitly so the report layout is fixed
    // (8 bytes: modifier, reserved, 6 keycodes).
    hid_class_request_set_protocol(hid_dev_handle, HID_REPORT_PROTOCOL_BOOT);
    hid_class_request_set_idle(hid_dev_handle, /*duration=*/0, /*report_id=*/0);
    if (hid_host_device_start(hid_dev_handle) != ESP_OK) {
        ESP_LOGE(kTag, "device_start failed");
        hid_host_device_close(hid_dev_handle);
        return;
    }
    g_dev = hid_dev_handle;
    ESP_LOGI(kTag, "keyboard ready");
}

}  // namespace

term::Result<void> start_usb_hid_input(ByteSink sink) {
    g_sink = std::move(sink);

    // The USB host library is brought up by start_usb_host_root() at
    // boot; we only register as a class driver here.
    hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority          = tskIDLE_PRIORITY + 3,
        .stack_size             = 4096,
        .core_id                = tskNO_AFFINITY,
        .callback               = driver_event_cb,
        .callback_arg           = nullptr,
    };
    esp_err_t err = hid_host_install(&hid_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "hid_host_install -> %s", esp_err_to_name(err));
        return std::unexpected(term::Error::NotInitialized);
    }
    ESP_LOGI(kTag, "USB HID host ready (plug a keyboard into USB-A)");
    return {};
}

}  // namespace tab5
