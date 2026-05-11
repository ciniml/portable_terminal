#include "status_bar.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "M5Unified.h"

#include "sdkconfig.h"
#include "connection.hpp"
#if CONFIG_TAB5_WIFI_ENABLED
#include "wifi_setup.hpp"
#endif

namespace tab5 {

namespace {

constexpr const char* kTag = "status";

// Right margin starts at x = (1280 - 960) / 2 + 960 = 1120.
constexpr int kPanelX      = 1120;
constexpr int kPanelY      = 0;
constexpr int kPanelW      = 160;
constexpr int kPanelH      = 720;
constexpr int kRefreshMs   = 5000;

constexpr uint16_t kBgColor      = 0x0841;  // very dark grey
constexpr uint16_t kFgColor      = 0xFFFF;
constexpr uint16_t kBatGoodColor = 0x07E0;
constexpr uint16_t kBatLowColor  = 0xFD20;  // amber
constexpr uint16_t kBatCritColor = 0xF800;
constexpr uint16_t kSeparator    = 0x4208;

// Exported synchronous render for the UI compositor. Caller already
// holds the lock.
void status_render_impl();

}  // namespace

void status_render() { status_render_impl(); }

namespace {

void status_render_impl() {
    auto& d = M5.Display;
    d.fillRect(kPanelX, kPanelY, kPanelW, kPanelH, kBgColor);
    d.drawFastVLine(kPanelX, 0, kPanelH, kSeparator);

    d.setFont(&fonts::lgfxJapanGothic_20);
    d.setTextDatum(top_left);
    d.setTextColor(kFgColor, kBgColor);

    int y = 12;
    auto label = [&](const char* s) {
        d.drawString(s, kPanelX + 10, y);
        y += 22;
    };
    auto value = [&](const char* s) {
        d.drawString(s, kPanelX + 14, y);
        y += 26;
    };

    // --- Battery -------------------------------------------------
    int32_t bl = M5.Power.getBatteryLevel();
    auto    ic = M5.Power.isCharging();
    char    bbuf[40];
    if (bl < 0) {
        snprintf(bbuf, sizeof(bbuf), "Battery: --");
    } else {
        const char* tag = (ic == M5.Power.is_charging) ? " CHG" :
                          (ic == M5.Power.is_discharging) ? "" : " ?";
        snprintf(bbuf, sizeof(bbuf), "Battery: %ld%%%s",
                 static_cast<long>(bl), tag);
    }
    label("--- Power ---");
    uint16_t bat_color = (bl < 0)  ? kFgColor
                       : (bl <= 10) ? kBatCritColor
                       : (bl <= 30) ? kBatLowColor
                       : kBatGoodColor;
    d.setTextColor(bat_color, kBgColor);
    value(bbuf);
    d.setTextColor(kFgColor, kBgColor);

    // Battery bar
    {
        int bx = kPanelX + 14;
        int by = y;
        int bw = kPanelW - 28;
        int bh = 10;
        d.drawRect(bx, by, bw, bh, kFgColor);
        if (bl > 0) {
            int fillw = (bw - 2) * bl / 100;
            d.fillRect(bx + 1, by + 1, fillw, bh - 2, bat_color);
        }
        y += bh + 14;
    }

    // --- Wi-Fi ---------------------------------------------------
    label("--- Network ---");
#if CONFIG_TAB5_WIFI_ENABLED
    auto wst = wifi_status();
    if (wst.connected) {
        uint8_t a = (wst.ip4 >>  0) & 0xFF;
        uint8_t b = (wst.ip4 >>  8) & 0xFF;
        uint8_t c = (wst.ip4 >> 16) & 0xFF;
        uint8_t e = (wst.ip4 >> 24) & 0xFF;
        char ipbuf[40];
        snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", a, b, c, e);
        d.setTextColor(kBatGoodColor, kBgColor);
        value("Wi-Fi: up");
        d.setTextColor(kFgColor, kBgColor);
        value(ipbuf);
    } else {
        d.setTextColor(kBatLowColor, kBgColor);
        value("Wi-Fi: down");
        d.setTextColor(kFgColor, kBgColor);
    }
#else
    d.setTextColor(0x8410, kBgColor);
    value("Wi-Fi: off");
    d.setTextColor(kFgColor, kBgColor);
#endif

    if (auto* c = active_connection(); c && c->is_connected()) {
        char ln[40];
        snprintf(ln, sizeof(ln), "%s: up", c->kind());
        d.setTextColor(kBatGoodColor, kBgColor);
        value(ln);
        d.setTextColor(kFgColor, kBgColor);
        // Print host label, truncated to fit the panel.
        char host[40];
        snprintf(host, sizeof(host), "%.30s", c->host_label());
        value(host);
    } else {
#if CONFIG_TAB5_SSH_ENABLED || CONFIG_TAB5_TELNET_ENABLED
        d.setTextColor(kBatLowColor, kBgColor);
        value("Remote: down");
        d.setTextColor(kFgColor, kBgColor);
#endif
    }

    // --- Uptime --------------------------------------------------
    y += 6;
    label("--- Uptime ---");
    uint32_t s = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
    char ubuf[24];
    snprintf(ubuf, sizeof(ubuf), "%lu:%02lu:%02lu",
             static_cast<unsigned long>(s / 3600),
             static_cast<unsigned long>((s / 60) % 60),
             static_cast<unsigned long>(s % 60));
    value(ubuf);

    // Restore terminal font for the rest of the app.
    d.setFont(&fonts::lgfxJapanGothic_24);
}

StatusLock g_lock;

void status_task(void*) {
    for (;;) {
        if (g_lock) {
            g_lock([] { status_render_impl(); });
        } else {
            status_render_impl();
        }
        vTaskDelay(pdMS_TO_TICKS(kRefreshMs));
    }
}

}  // namespace

void start_status_bar(StatusLock lock) {
    g_lock = std::move(lock);
    BaseType_t rc = xTaskCreate(&status_task, "status", 4096, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (rc != pdPASS) {
        ESP_LOGE(kTag, "task create failed");
    }
}

}  // namespace tab5
