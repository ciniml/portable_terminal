#include "input_usb_hid.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/hid.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

namespace tab5 {

namespace {

constexpr const char* kTag = "usb_hid";

// HID modifier bit masks (report byte 0)
constexpr uint8_t kModLCtrl  = 0x01;
constexpr uint8_t kModLShift = 0x02;
constexpr uint8_t kModLAlt   = 0x04;
constexpr uint8_t kModRCtrl  = 0x10;
constexpr uint8_t kModRShift = 0x20;
constexpr uint8_t kModRAlt   = 0x40;

// US-layout maps. Each entry is (unshifted, shifted) for HID usage codes
// 0x04..0x38 (the printable range). Special keys handled out of band.
struct CharMap {
    char base;
    char shifted;
};
constexpr CharMap kMap[0x39] = {
    {0, 0},  {0, 0},  {0, 0},  {0, 0},                       // 0x00..0x03
    {'a','A'}, {'b','B'}, {'c','C'}, {'d','D'}, {'e','E'},   // 0x04..0x08
    {'f','F'}, {'g','G'}, {'h','H'}, {'i','I'}, {'j','J'},   // 0x09..0x0D
    {'k','K'}, {'l','L'}, {'m','M'}, {'n','N'}, {'o','O'},   // 0x0E..0x12
    {'p','P'}, {'q','Q'}, {'r','R'}, {'s','S'}, {'t','T'},   // 0x13..0x17
    {'u','U'}, {'v','V'}, {'w','W'}, {'x','X'}, {'y','Y'},   // 0x18..0x1C
    {'z','Z'},                                               // 0x1D
    {'1','!'}, {'2','@'}, {'3','#'}, {'4','$'}, {'5','%'},   // 0x1E..0x22
    {'6','^'}, {'7','&'}, {'8','*'}, {'9','('}, {'0',')'},   // 0x23..0x27
    {0, 0},   // Enter — handled specially      0x28
    {0, 0},   // Esc                            0x29
    {0, 0},   // Backspace                      0x2A
    {0, 0},   // Tab                            0x2B
    {' ',' '}, // Space                         0x2C
    {'-','_'}, {'=','+'},                                    // 0x2D, 0x2E
    {'[','{'}, {']','}'}, {'\\','|'},                        // 0x2F..0x31
    {0, 0},                                                  // 0x32 non-US hash
    {';',':'}, {'\'','"'}, {'`','~'},                        // 0x33..0x35
    {',','<'}, {'.','>'}, {'/','?'},                         // 0x36..0x38
};

// Per-key escape sequences (for special keys above 0x39). Returns a
// non-null pointer when this key produces a fixed escape sequence; the
// pointer is to a static string with length in *len_out.
const char* special_seq(uint8_t code, int* len_out) {
    *len_out = 0;
    auto lit = [&](const char* s, int n) { *len_out = n; return s; };
    switch (code) {
        case HID_KEY_RIGHT:       return lit("\x1B[C", 3);
        case HID_KEY_LEFT:        return lit("\x1B[D", 3);
        case HID_KEY_DOWN:        return lit("\x1B[B", 3);
        case HID_KEY_UP:          return lit("\x1B[A", 3);
        case HID_KEY_HOME:        return lit("\x1B[H", 3);
        case HID_KEY_END:         return lit("\x1B[F", 3);
        case HID_KEY_INSERT:      return lit("\x1B[2~", 4);
        case HID_KEY_DELETE:      return lit("\x1B[3~", 4);
        case HID_KEY_PAGEUP:      return lit("\x1B[5~", 4);
        case HID_KEY_PAGEDOWN:    return lit("\x1B[6~", 4);
        case HID_KEY_F1:          return lit("\x1BOP", 3);
        case HID_KEY_F2:          return lit("\x1BOQ", 3);
        case HID_KEY_F3:          return lit("\x1BOR", 3);
        case HID_KEY_F4:          return lit("\x1BOS", 3);
        case HID_KEY_F5:          return lit("\x1B[15~", 5);
        case HID_KEY_F6:          return lit("\x1B[17~", 5);
        case HID_KEY_F7:          return lit("\x1B[18~", 5);
        case HID_KEY_F8:          return lit("\x1B[19~", 5);
        case HID_KEY_F9:          return lit("\x1B[20~", 5);
        case HID_KEY_F10:         return lit("\x1B[21~", 5);
        case HID_KEY_F11:         return lit("\x1B[23~", 5);
        case HID_KEY_F12:         return lit("\x1B[24~", 5);
        default: return nullptr;
    }
}

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
void emit_key(uint8_t code, uint8_t mod) {
    bool shift = mod & (kModLShift | kModRShift);
    bool ctrl  = mod & (kModLCtrl  | kModRCtrl);
    bool alt   = mod & (kModLAlt   | kModRAlt);

    uint8_t prefix[1] = { 0x1B };

    auto push = [&](const uint8_t* p, size_t n) {
        if (!g_sink || n == 0) return;
        if (alt) g_sink(std::span<const uint8_t>(prefix, 1));
        g_sink(std::span<const uint8_t>(p, n));
    };

    // Fixed-byte specials
    switch (code) {
        case HID_KEY_ENTER:      { uint8_t b = '\r'; push(&b, 1); return; }
        case HID_KEY_ESC:        { uint8_t b = 0x1B; push(&b, 1); return; }
        case HID_KEY_DEL:              { uint8_t b = 0x7F; push(&b, 1); return; }
        case HID_KEY_TAB:        { uint8_t b = '\t'; push(&b, 1); return; }
        default: break;
    }

    // Multi-byte specials (arrows / F-keys / Home / End / Ins / Del / PgUp/Dn)
    {
        int len = 0;
        if (const char* s = special_seq(code, &len)) {
            push(reinterpret_cast<const uint8_t*>(s), static_cast<size_t>(len));
            return;
        }
    }

    // Printable range
    if (code >= 0x04 && code < 0x39) {
        char ch = shift ? kMap[code].shifted : kMap[code].base;
        if (ch == 0) return;
        if (ctrl) {
            // Standard Ctrl- mapping.
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 1);
            else if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 1);
            else if (ch >= '@' && ch <= '_') ch = static_cast<char>(ch - '@');
            // else: pass through
        }
        uint8_t b = static_cast<uint8_t>(ch);
        push(&b, 1);
    }
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
