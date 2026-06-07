#include "boot_progress.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "M5Unified.h"

#include "input_touch.hpp"
#include "status_bar.hpp"

namespace tab5::boot_progress {

namespace {

// State guarded by g_mutex.
SemaphoreHandle_t g_mutex = nullptr;
Stage             g_stage = Stage::Idle;
char              g_detail[64] = {0};

// cancel_requested is read from many threads (status_bar repaint,
// boot task wait loops, touch handler) — make it lock-free.
std::atomic<bool> g_cancel{false};

constexpr const char* stage_label(Stage s) {
    switch (s) {
        case Stage::Idle:             return "Idle";
        case Stage::WifiConnecting:   return "Wi-Fi";
        case Stage::VpnSyncingClock:  return "SNTP";
        case Stage::VpnConnecting:    return "VPN";
        case Stage::VpnAwaitAuth:     return "VPN auth";
        case Stage::RemoteConnecting: return "Remote";
        case Stage::Done:             return "Done";
        case Stage::Failed:           return "Failed";
        case Stage::Cancelled:        return "Cancelled";
    }
    return "?";
}

// Cancel button geometry inside the status panel. Status panel is at
// x = kStatusPanelX (1120), width kStatusPanelW (160). The block is
// painted from the y that status_bar passes in; the button is the
// last thing drawn, at button_y captured here so handle_touch can
// hit-test against it without a layout dance.
std::atomic<int16_t> g_btn_x0{0};
std::atomic<int16_t> g_btn_y0{0};
std::atomic<int16_t> g_btn_x1{0};
std::atomic<int16_t> g_btn_y1{0};

void ensure_mutex() {
    if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
}

}  // namespace

void set(Stage s, const char* detail) {
    ensure_mutex();
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_stage != s) g_stage = s;
    char buf[sizeof(g_detail)] = {0};
    if (detail && *detail) std::snprintf(buf, sizeof(buf), "%s", detail);
    if (std::strncmp(g_detail, buf, sizeof(g_detail)) != 0) {
        std::memcpy(g_detail, buf, sizeof(g_detail));
    }
    xSemaphoreGive(g_mutex);
    // No immediate repaint: status_task polls fast (~250 ms) while
    // is_busy() so a stage change surfaces inside the next tick under
    // the proper UI lock. Calling ui_root::invalidate() from here
    // would touch the LCD without holding the UI mutex.
}

Snapshot get() {
    ensure_mutex();
    Snapshot snap{};
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    snap.stage = g_stage;
    std::memcpy(snap.detail, g_detail, sizeof(snap.detail));
    xSemaphoreGive(g_mutex);
    snap.cancel_requested = g_cancel.load(std::memory_order_acquire);
    return snap;
}

bool is_busy() {
    Snapshot s = get();
    switch (s.stage) {
        case Stage::WifiConnecting:
        case Stage::VpnSyncingClock:
        case Stage::VpnConnecting:
        case Stage::VpnAwaitAuth:
        case Stage::RemoteConnecting:
            return true;
        default:
            return false;
    }
}

void request_cancel() { g_cancel.store(true, std::memory_order_release); }
bool cancel_requested() { return g_cancel.load(std::memory_order_acquire); }
void clear_cancel()  { g_cancel.store(false, std::memory_order_release); }

bool handle_touch(const TouchPoint& p) {
    if (p.event != TouchEvent::Down) return false;
    if (!is_busy()) return false;
    int x0 = g_btn_x0.load(std::memory_order_acquire);
    int y0 = g_btn_y0.load(std::memory_order_acquire);
    int x1 = g_btn_x1.load(std::memory_order_acquire);
    int y1 = g_btn_y1.load(std::memory_order_acquire);
    if (x1 <= x0 || y1 <= y0) return false;
    if (p.x < x0 || p.x >= x1 || p.y < y0 || p.y >= y1) return false;
    request_cancel();
    // The next status_task tick (≤250 ms while is_busy()) will flip the
    // button label to "Cancelling..." through the proper lock path.
    return true;
}

int render_block(int x, int y, int panel_w) {
    Snapshot s = get();
    if (s.stage == Stage::Idle) return y;

    auto& d = M5.Display;

    // Colors that match the status_bar palette.
    constexpr uint16_t kBg       = 0x0841;   // panel background
    constexpr uint16_t kFg       = 0xFFFF;
    constexpr uint16_t kGood     = 0x07E0;   // green
    constexpr uint16_t kWarn     = 0xFD20;   // amber
    constexpr uint16_t kFail     = 0xF800;   // red
    constexpr uint16_t kMuted    = 0x8410;   // grey

    d.setTextDatum(top_left);

    auto label = [&](const char* s) {
        d.drawString(s, x + 10, y);
        y += 22;
    };
    auto value = [&](const char* s) {
        d.drawString(s, x + 14, y);
        y += 26;
    };

    // Heading colour depends on terminal state.
    uint16_t head_color = kWarn;
    if      (s.stage == Stage::Done)      head_color = kGood;
    else if (s.stage == Stage::Failed)    head_color = kFail;
    else if (s.stage == Stage::Cancelled) head_color = kMuted;

    d.setTextColor(head_color, kBg);
    if (s.stage == Stage::Done)            label("--- Boot OK ---");
    else if (s.stage == Stage::Failed)     label("--- Boot fail ---");
    else if (s.stage == Stage::Cancelled)  label("--- Cancelled ---");
    else                                    label("--- Connecting ---");
    d.setTextColor(kFg, kBg);

    // Stage line + detail. Detail is a single line; truncate to fit.
    char line[40];
    std::snprintf(line, sizeof(line), "%s", stage_label(s.stage));
    d.setTextColor(head_color, kBg);
    value(line);
    d.setTextColor(kFg, kBg);
    if (s.detail[0]) {
        // Available glyph width on the panel: panel_w-28 px at ~10 px/char.
        const int max_chars = (panel_w - 28) / 10;
        std::snprintf(line, sizeof(line), "%.*s", max_chars, s.detail);
        value(line);
    }

    if (is_busy()) {
        // Cancel button. ~120×36, centred in the panel, with a 2-px
        // amber border. Label changes once the cancel is in flight so
        // the user sees acknowledgement before the boot task observes
        // the flag.
        constexpr int bw = 120;
        constexpr int bh = 36;
        int bx = x + (panel_w - bw) / 2;
        int by = y + 8;

        bool pressed = s.cancel_requested;
        uint16_t border = pressed ? kMuted : kWarn;
        uint16_t txt    = pressed ? kMuted : kWarn;
        d.drawRect(bx,     by,     bw,     bh,     border);
        d.drawRect(bx + 1, by + 1, bw - 2, bh - 2, border);

        d.setTextDatum(middle_center);
        d.setTextColor(txt, kBg);
        d.drawString(pressed ? "Cancelling..." : "Cancel",
                     bx + bw / 2, by + bh / 2);
        d.setTextDatum(top_left);
        d.setTextColor(kFg, kBg);

        // Publish the rect so handle_touch can hit-test without
        // recomputing the layout.
        g_btn_x0.store(static_cast<int16_t>(bx),       std::memory_order_release);
        g_btn_y0.store(static_cast<int16_t>(by),       std::memory_order_release);
        g_btn_x1.store(static_cast<int16_t>(bx + bw),  std::memory_order_release);
        g_btn_y1.store(static_cast<int16_t>(by + bh),  std::memory_order_release);
        y = by + bh + 10;
    } else {
        // No active button — clear the rect so a stale tap can't fire.
        g_btn_x0.store(0, std::memory_order_release);
        g_btn_y0.store(0, std::memory_order_release);
        g_btn_x1.store(0, std::memory_order_release);
        g_btn_y1.store(0, std::memory_order_release);
        y += 6;
    }

    return y;
}

}  // namespace tab5::boot_progress
