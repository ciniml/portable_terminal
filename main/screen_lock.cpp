// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "screen_lock.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "M5Unified.h"

#include "ap_qr_screen.hpp"

namespace tab5::screen_lock {

namespace {

constexpr const char* kTag = "scrlock";

// ---- NVS ---------------------------------------------------------------

constexpr const char* kNvsNamespace  = "scrlock";
constexpr const char* kKeyPinHash    = "pin_hash";     // 32-byte blob
constexpr const char* kKeyEnabled    = "enabled";      // u8
constexpr const char* kKeyTimeoutMin = "timeout_min";  // u16

constexpr uint16_t kDefaultTimeoutMin = 10;
constexpr uint16_t kMaxTimeoutMin     = 1440;

// ---- Config state (shared with the HTTP server task) -------------------

std::mutex           g_cfg_mutex;   // guards g_pin_hash / g_pin_set
uint8_t              g_pin_hash[32] = {};
bool                 g_pin_set = false;
std::atomic<bool>    g_enabled{false};
std::atomic<uint16_t> g_timeout_min{kDefaultTimeoutMin};

// ---- Runtime state -----------------------------------------------------

std::atomic<int64_t> g_last_activity_us{0};
std::atomic<bool>    g_locked{false};

// Everything below is only touched under the global UI lock.
bool    g_awake = true;             // backlight lit
uint8_t g_saved_brightness = 128;   // pre-lock brightness to restore
char    g_entry[9] = {};            // PIN entry buffer
size_t  g_entry_len = 0;
int     g_attempts = 0;             // consecutive wrong PINs (RAM only)
int64_t g_lockout_until_us = 0;     // 0 = no lockout
int     g_shown_countdown_s = -1;   // last rendered countdown value
bool    g_error = false;            // "wrong PIN" message pending
UiHook  g_on_lock;
UiHook  g_on_unlock;

// ---- Layout ------------------------------------------------------------

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;
constexpr int kCx      = kScreenW / 2;

constexpr uint16_t kBg        = 0x10A2;  // same dark slate as the menu
constexpr uint16_t kFieldBg   = 0x18C3;
constexpr uint16_t kFieldEdge = 0x4208;
constexpr uint16_t kKeyBg     = 0x2965;
constexpr uint16_t kKeyFg     = 0xFFFF;
constexpr uint16_t kOkBg      = 0x0410;
constexpr uint16_t kTitleFg   = 0xFFFF;
constexpr uint16_t kHintFg    = 0x7BEF;
constexpr uint16_t kErrFg     = 0xF9E7;  // light red
constexpr uint16_t kWarnFg    = 0xFDE0;  // amber

constexpr int kTitleY  = 80;
constexpr int kFieldW  = 400;
constexpr int kFieldH  = 56;
constexpr int kFieldY  = 168;
constexpr int kMsgY    = 244;
constexpr int kMsgH    = 34;

constexpr int kKeyW    = 140;
constexpr int kKeyH    = 76;
constexpr int kKeyGapX = 14;
constexpr int kKeyGapY = 14;
constexpr int kPadX0   = kCx - (3 * kKeyW + 2 * kKeyGapX) / 2;  // 416
constexpr int kPadY0   = 300;

// Key layout: index 0..11 → 1 2 3 / 4 5 6 / 7 8 9 / ← 0 OK
constexpr int kKeyBackspace = 9;
constexpr int kKeyZero      = 10;
constexpr int kKeyOk        = 11;

// ---- Helpers -----------------------------------------------------------

bool pin_valid(const char* pin) {
    if (!pin) return false;
    size_t n = std::strlen(pin);
    if (n < 4 || n > 8) return false;
    for (size_t i = 0; i < n; ++i) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    return true;
}

void hash_pin(const char* pin, uint8_t out[32]) {
    // mbedtls_sha256 returns 0 on success; a failure would leave `out`
    // undefined, so zero it first (a zeroed hash never matches — the
    // stored hash comes from the same function succeeding).
    std::memset(out, 0, 32);
    (void)mbedtls_sha256(reinterpret_cast<const unsigned char*>(pin),
                         std::strlen(pin), out, /*is224=*/0);
}

bool nvs_persist(const uint8_t* hash /*nullable*/, bool erase_hash,
                 bool enabled, uint16_t timeout_min) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    if (hash) {
        err = nvs_set_blob(nvs, kKeyPinHash, hash, 32);
    } else if (erase_hash) {
        err = nvs_erase_key(nvs, kKeyPinHash);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_set_u8(nvs, kKeyEnabled, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u16(nvs, kKeyTimeoutMin, timeout_min);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs write failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

int lockout_remaining_s() {
    if (g_lockout_until_us == 0) return 0;
    int64_t now = esp_timer_get_time();
    if (now >= g_lockout_until_us) return 0;
    return static_cast<int>((g_lockout_until_us - now + 999999) / 1000000);
}

// ---- Partial repaints (UI lock held) -----------------------------------

void render_field() {
    auto& d = M5.Display;
    int x = kCx - kFieldW / 2;
    d.fillRoundRect(x, kFieldY, kFieldW, kFieldH, 10, kFieldBg);
    d.drawRoundRect(x, kFieldY, kFieldW, kFieldH, 10, kFieldEdge);
    if (g_entry_len > 0) {
        char dots[8 * 3 + 1] = {};  // "●" is 3 bytes in UTF-8
        for (size_t i = 0; i < g_entry_len; ++i) {
            std::memcpy(dots + i * 3, "\xE2\x97\x8F", 3);
        }
        d.setFont(&fonts::lgfxJapanGothic_24);
        d.setTextColor(kTitleFg, kFieldBg);
        d.setTextDatum(lgfx::textdatum_t::middle_center);
        d.drawString(dots, kCx, kFieldY + kFieldH / 2);
    }
}

void render_message() {
    auto& d = M5.Display;
    d.fillRect(0, kMsgY, kScreenW, kMsgH, kBg);
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.setTextDatum(lgfx::textdatum_t::middle_center);

    char buf[96];
    int remain = lockout_remaining_s();
    if (remain > 0) {
        std::snprintf(buf, sizeof(buf),
                      "入力回数超過 — %d 秒待ってください", remain);
        d.setTextColor(kWarnFg, kBg);
        d.drawString(buf, kCx, kMsgY + kMsgH / 2);
        g_shown_countdown_s = remain;
    } else if (g_error) {
        std::snprintf(buf, sizeof(buf), "PIN が違います (%d 回目)", g_attempts);
        d.setTextColor(kErrFg, kBg);
        d.drawString(buf, kCx, kMsgY + kMsgH / 2);
        g_shown_countdown_s = -1;
    } else {
        d.setTextColor(kHintFg, kBg);
        d.drawString("PIN を入力して OK", kCx, kMsgY + kMsgH / 2);
        g_shown_countdown_s = -1;
    }
    // Restore terminal defaults (status_bar.cpp convention).
    d.setTextDatum(lgfx::textdatum_t::top_left);
}

// ---- Entry / verification (UI lock held) -------------------------------

void do_unlock() {
    g_locked.store(false, std::memory_order_release);
    g_attempts = 0;
    g_lockout_until_us = 0;
    g_entry_len = 0;
    std::memset(g_entry, 0, sizeof(g_entry));
    g_error = false;
    activity_note();
    ESP_LOGI(kTag, "unlocked");
    if (g_on_unlock) g_on_unlock();
}

void submit_entry() {
    g_entry[g_entry_len] = '\0';
    const bool ok = g_entry_len >= 4 && verify_pin(g_entry);
    g_entry_len = 0;
    std::memset(g_entry, 0, sizeof(g_entry));
    if (ok) {
        do_unlock();
        return;
    }
    ++g_attempts;
    g_error = true;
    if (g_attempts >= 5) {
        // Escalating lockout: 30 s doubling per further failure, capped
        // at 240 s. RAM only — a reboot resets the counter.
        int shift = g_attempts - 5;
        if (shift > 3) shift = 3;
        int64_t lock_s = 30LL << shift;
        g_lockout_until_us = esp_timer_get_time() + lock_s * 1000000;
        ESP_LOGW(kTag, "wrong PIN #%d — %lld s lockout",
                 g_attempts, static_cast<long long>(lock_s));
    } else {
        ESP_LOGW(kTag, "wrong PIN #%d", g_attempts);
    }
    render_field();
    render_message();
}

void press_key(int key) {
    if (lockout_remaining_s() > 0) return;  // pad disabled during lockout
    if (key == kKeyOk) {
        submit_entry();
        return;
    }
    if (key == kKeyBackspace) {
        if (g_entry_len > 0) {
            --g_entry_len;
            g_entry[g_entry_len] = '\0';
            render_field();
        }
        return;
    }
    int digit = (key == kKeyZero) ? 0 : key + 1;  // keys 0..8 → digits 1..9
    if (g_entry_len < 8) {
        if (g_error) {
            g_error = false;
            render_message();
        }
        g_entry[g_entry_len++] = static_cast<char>('0' + digit);
        render_field();
    }
}

int hit_key(int x, int y) {
    if (x < kPadX0 || y < kPadY0) return -1;
    int col = (x - kPadX0) / (kKeyW + kKeyGapX);
    int row = (y - kPadY0) / (kKeyH + kKeyGapY);
    if (col > 2 || row > 3) return -1;
    // Reject taps in the gaps.
    if ((x - kPadX0) % (kKeyW + kKeyGapX) >= kKeyW) return -1;
    if ((y - kPadY0) % (kKeyH + kKeyGapY) >= kKeyH) return -1;
    return row * 3 + col;
}

}  // namespace

// ---- Configuration API -------------------------------------------------

Config get_config() {
    return {g_enabled.load(std::memory_order_relaxed),
            g_timeout_min.load(std::memory_order_relaxed)};
}

bool set_config(bool enabled, uint16_t timeout_min) {
    if (timeout_min < 1) timeout_min = 1;
    if (timeout_min > kMaxTimeoutMin) timeout_min = kMaxTimeoutMin;
    std::lock_guard<std::mutex> lg(g_cfg_mutex);
    if (!nvs_persist(nullptr, /*erase_hash=*/false, enabled, timeout_min)) {
        return false;
    }
    g_enabled.store(enabled, std::memory_order_relaxed);
    g_timeout_min.store(timeout_min, std::memory_order_relaxed);
    // Re-arm the idle clock so a fresh enable doesn't lock instantly
    // off a stale timestamp.
    activity_note();
    ESP_LOGI(kTag, "config: enabled=%d timeout=%u min",
             enabled ? 1 : 0, static_cast<unsigned>(timeout_min));
    return true;
}

bool set_pin(const char* pin_digits) {
    std::lock_guard<std::mutex> lg(g_cfg_mutex);
    const bool clear = (pin_digits == nullptr || pin_digits[0] == '\0');
    uint8_t hash[32];
    if (!clear) {
        if (!pin_valid(pin_digits)) return false;
        hash_pin(pin_digits, hash);
    }
    if (!nvs_persist(clear ? nullptr : hash, /*erase_hash=*/clear,
                     g_enabled.load(std::memory_order_relaxed),
                     g_timeout_min.load(std::memory_order_relaxed))) {
        return false;
    }
    if (clear) {
        std::memset(g_pin_hash, 0, sizeof(g_pin_hash));
        g_pin_set = false;
        ESP_LOGI(kTag, "PIN cleared");
    } else {
        std::memcpy(g_pin_hash, hash, sizeof(g_pin_hash));
        g_pin_set = true;
        ESP_LOGI(kTag, "PIN updated");
    }
    return true;
}

bool pin_is_set() {
    std::lock_guard<std::mutex> lg(g_cfg_mutex);
    return g_pin_set;
}

bool verify_pin(const char* pin_digits) {
    if (!pin_valid(pin_digits)) return false;
    uint8_t hash[32];
    hash_pin(pin_digits, hash);
    std::lock_guard<std::mutex> lg(g_cfg_mutex);
    if (!g_pin_set) return false;
    // Constant-time-ish compare (this is casual protection, but cheap).
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(hash); ++i) diff |= hash[i] ^ g_pin_hash[i];
    return diff == 0;
}

// ---- Runtime -----------------------------------------------------------

void init() {
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t en = 0;
        uint16_t tm = kDefaultTimeoutMin;
        (void)nvs_get_u8(nvs, kKeyEnabled, &en);
        (void)nvs_get_u16(nvs, kKeyTimeoutMin, &tm);
        if (tm < 1) tm = 1;
        if (tm > kMaxTimeoutMin) tm = kMaxTimeoutMin;
        size_t len = sizeof(g_pin_hash);
        std::lock_guard<std::mutex> lg(g_cfg_mutex);
        if (nvs_get_blob(nvs, kKeyPinHash, g_pin_hash, &len) == ESP_OK &&
            len == sizeof(g_pin_hash)) {
            g_pin_set = true;
        }
        g_enabled.store(en != 0, std::memory_order_relaxed);
        g_timeout_min.store(tm, std::memory_order_relaxed);
        nvs_close(nvs);
    }
    activity_note();
    ESP_LOGI(kTag, "init: enabled=%d timeout=%u min pin_set=%d",
             g_enabled.load() ? 1 : 0,
             static_cast<unsigned>(g_timeout_min.load()),
             pin_is_set() ? 1 : 0);
}

void activity_note() {
    g_last_activity_us.store(esp_timer_get_time(), std::memory_order_relaxed);
}

void set_ui_hooks(UiHook on_lock, UiHook on_unlock) {
    g_on_lock = std::move(on_lock);
    g_on_unlock = std::move(on_unlock);
}

void lock_now() {
    if (g_locked.load(std::memory_order_acquire)) return;
    if (!pin_is_set()) {
        // Locking without a PIN would be unrecoverable from the device.
        ESP_LOGW(kTag, "lock refused: no PIN set");
        return;
    }
    uint8_t b = M5.Display.getBrightness();
    g_saved_brightness = b ? b : 128;
    M5.Display.setBrightness(0);
    g_awake = false;
    g_entry_len = 0;
    std::memset(g_entry, 0, sizeof(g_entry));
    g_error = false;
    g_shown_countdown_s = -1;
    g_locked.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "locked");
    // app_main's hook closes menu / QR / soft keyboard, flips the
    // ui_root overlay flag and invalidates fullscreen — the lock-screen
    // layer paints into the (dark) framebuffer, so the terminal content
    // is already gone before the first wake.
    if (g_on_lock) g_on_lock();
}

bool locked() {
    return g_locked.load(std::memory_order_acquire);
}

void tick() {
    if (locked()) return;
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (!pin_is_set()) return;
    if (tab5::ap_qr_screen::visible()) {
        // Never lock over the provisioning overlay — re-arm instead.
        activity_note();
        return;
    }
    int64_t idle_us = esp_timer_get_time() -
                      g_last_activity_us.load(std::memory_order_relaxed);
    int64_t limit_us = static_cast<int64_t>(
                           g_timeout_min.load(std::memory_order_relaxed)) *
                       60 * 1000000;
    if (idle_us >= limit_us) {
        ESP_LOGI(kTag, "idle timeout — locking");
        lock_now();
    }
}

bool wake_if_dark() {
    if (!locked() || g_awake) return false;
    g_awake = true;
    M5.Display.setBrightness(g_saved_brightness);
    return true;
}

bool handle_touch(const TouchPoint& p) {
    if (!locked()) return false;
    if (p.event == TouchEvent::Down) {
        int key = hit_key(p.x, p.y);
        if (key >= 0) press_key(key);
    }
    return true;  // the lock screen owns the whole touch surface
}

bool feed_key(uint8_t byte) {
    if (!locked()) return false;
    if (wake_if_dark()) return true;  // defensive — caller normally handles
    if (lockout_remaining_s() > 0) return true;
    if (byte >= '0' && byte <= '9') {
        int key = (byte == '0') ? kKeyZero : (byte - '1');
        press_key(key);
    } else if (byte == 0x08 || byte == 0x7F) {
        press_key(kKeyBackspace);
    } else if (byte == '\r' || byte == '\n') {
        press_key(kKeyOk);
    }
    return true;  // swallow everything while locked
}

void blink_tick() {
    if (!locked() || !g_awake) return;
    int remain = lockout_remaining_s();
    if (remain != g_shown_countdown_s &&
        (remain > 0 || g_shown_countdown_s > 0)) {
        // Countdown value changed, or the lockout just expired — refresh
        // the message line (falls back to the hint when it ends).
        render_message();
    }
}

void render() {
    if (!locked()) return;
    auto& d = M5.Display;
    d.startWrite();
    d.fillScreen(kBg);

    d.setFont(&fonts::lgfxJapanGothic_36);
    d.setTextColor(kTitleFg, kBg);
    d.setTextDatum(lgfx::textdatum_t::middle_center);
    d.drawString("画面ロック", kCx, kTitleY);

    render_field();
    render_message();

    // Numeric pad.
    d.setTextDatum(lgfx::textdatum_t::middle_center);
    for (int key = 0; key < 12; ++key) {
        int col = key % 3;
        int row = key / 3;
        int x = kPadX0 + col * (kKeyW + kKeyGapX);
        int y = kPadY0 + row * (kKeyH + kKeyGapY);
        uint16_t bg = (key == kKeyOk) ? kOkBg : kKeyBg;
        d.fillRoundRect(x, y, kKeyW, kKeyH, 10, bg);
        d.drawRoundRect(x, y, kKeyW, kKeyH, 10, kFieldEdge);
        char label[4];
        if (key == kKeyBackspace) {
            std::snprintf(label, sizeof(label), "←");
        } else if (key == kKeyOk) {
            std::snprintf(label, sizeof(label), "OK");
        } else {
            std::snprintf(label, sizeof(label), "%d",
                          (key == kKeyZero) ? 0 : key + 1);
        }
        d.setFont(&fonts::lgfxJapanGothic_28);
        d.setTextColor(kKeyFg, bg);
        d.drawString(label, x + kKeyW / 2, y + kKeyH / 2);
    }

    d.endWrite();

    // Restore the terminal defaults the rest of the app expects.
    d.setTextDatum(lgfx::textdatum_t::top_left);
    d.setFont(&fonts::lgfxJapanGothic_24);
}

}  // namespace tab5::screen_lock
