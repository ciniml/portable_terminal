// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "ap_qr_screen.hpp"

#if CONFIG_TAB5_HTTP_CONFIG_ENABLED

#include <atomic>
#include <cstdio>

#include "M5Unified.h"

#include "http_config.hpp"
#include "vpn.hpp"

namespace tab5::ap_qr_screen {

namespace {

std::atomic<bool> g_visible{false};

// Snapshot of the Tailscale interactive-auth URL taken at show() time.
// Non-empty => render a third QR panel so the user can log the device
// into the tailnet by scanning it with a phone. The URL appears
// asynchronously (registration runs ~15-30 s after boot), so it is
// captured when the overlay opens, not at boot.
char g_auth_url[256] = {};

constexpr const char* kPortalUrl = "http://192.168.4.1/";

// Append `src` into `dst`, escaping the Wi-Fi-QR special characters
// `\ ; , " :` with a backslash (Wi-Fi Alliance payload rules). Our
// SSID/password only use [A-Za-z0-9-], so no escaping is needed in
// practice; keep it for safety should the derivation ever change.
std::size_t append_escaped(char* dst, std::size_t cap, std::size_t pos,
                           const char* src) {
    for (; *src != '\0' && pos + 2 < cap; ++src) {
        const char c = *src;
        if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':') {
            dst[pos++] = '\\';
        }
        dst[pos++] = c;
    }
    dst[pos] = '\0';
    return pos;
}

// Standard Wi-Fi QR payload, supported natively by the iOS camera and
// Android scanners:  WIFI:T:<auth>;S:<SSID>;P:<password>;H:<hidden>;;
void build_wifi_qr_payload(const char* ssid, const char* pw, char* out,
                           std::size_t cap) {
    std::size_t p = 0;
    const bool open_ap = (pw == nullptr || pw[0] == '\0');
    p += std::snprintf(out + p, cap - p, "%s",
                       open_ap ? "WIFI:T:nopass;S:" : "WIFI:T:WPA;S:");
    p = append_escaped(out, cap, p, ssid);
    if (!open_ap) {
        p += std::snprintf(out + p, cap - p, ";P:");
        p = append_escaped(out, cap, p, pw);
    }
    std::snprintf(out + p, cap - p, ";H:false;;");
}

}  // namespace

void show() {
    g_auth_url[0] = '\0';
    (void)tab5::vpn::get_pending_auth_url(g_auth_url, sizeof(g_auth_url));
    g_visible.store(true, std::memory_order_release);
}
void dismiss() { g_visible.store(false, std::memory_order_release); }
bool visible() { return g_visible.load(std::memory_order_acquire); }

void render() {
    if (!visible()) return;

    const char* ssid = http_config::ap_ssid();
    const char* psk  = http_config::ap_psk();

    auto& d = M5.Display;
    const int W = d.width();    // 1280
    const int H = d.height();   // 720

    // Layout: title band on top, two QR panels side by side (three when
    // a Tailscale interactive-auth URL is pending), credential lines
    // under each QR, dismiss hint at the bottom. White background for
    // QR contrast (scanners want a light quiet zone).
    constexpr int kQrW    = 300;
    const bool have_auth = g_auth_url[0] != '\0';
    // Two panels sit at 1/4 and 3/4; with the auth panel the three
    // columns move to 1/6, 1/2 and 5/6 (427 px pitch — a 300 px QR
    // still fits with margin).
    const int col_l = have_auth ? W / 6 : W / 4;        // AP-join column
    const int col_r = have_auth ? W / 2 : (W * 3) / 4;  // portal column
    const int col_a = (W * 5) / 6;                      // Tailscale column
    const int qr_y  = 170;

    d.startWrite();
    d.fillScreen(TFT_WHITE);

    d.setFont(&fonts::lgfxJapanGothic_36);
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setTextDatum(lgfx::textdatum_t::top_center);
    d.drawString("Wi-Fi setup — scan with your phone", W / 2, 32);

    d.setFont(&fonts::lgfxJapanGothic_24);
    d.drawString("1. Join the Tab5 AP", col_l, qr_y - 44);
    d.drawString("2. Open the settings page", col_r, qr_y - 44);
    if (have_auth) {
        d.drawString("3. Tailscale ログイン", col_a, qr_y - 44);
    }

    // Wi-Fi join QR (left). Version 0 = auto-size from payload length.
    char payload[160];
    build_wifi_qr_payload(ssid, psk, payload, sizeof(payload));
    d.qrcode(payload, col_l - kQrW / 2, qr_y, kQrW, 0);

    // Portal URL QR (middle / right).
    d.qrcode(kPortalUrl, col_r - kQrW / 2, qr_y, kQrW, 0);

    // Tailscale interactive-auth QR (rightmost, only while registration
    // is awaiting login). The phone opens the URL over its own internet
    // connection — joining the Tab5 AP is not required for this one; any
    // network that reaches login.tailscale.com will do.
    if (have_auth) {
        d.qrcode(g_auth_url, col_a - kQrW / 2, qr_y, kQrW, 0);
    }

    // Human-readable credentials under each QR.
    int ty = qr_y + kQrW + 24;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "SSID: %s", ssid);
    d.drawString(buf, col_l, ty);
    if (psk && psk[0]) {
        std::snprintf(buf, sizeof(buf), "Password: %s", psk);
    } else {
        std::snprintf(buf, sizeof(buf), "Password: (open network)");
    }
    d.drawString(buf, col_l, ty + 34);
    d.drawString(kPortalUrl, col_r, ty);
    if (have_auth) {
        d.drawString("Tailscale ログイン", col_a, ty);
    }
    d.setTextColor(TFT_DARKGREY, TFT_WHITE);
    d.drawString("(opens automatically on most phones)", col_r, ty + 34);
    if (have_auth) {
        d.drawString("スマホでスキャンしてログイン", col_a, ty + 34);
    }

    d.setTextColor(TFT_DARKGREY, TFT_WHITE);
    d.drawString("Tap the screen or press any key to return to the terminal",
                 W / 2, H - 60);

    d.endWrite();

    // Restore the terminal defaults the rest of the app expects (same
    // convention as status_bar.cpp).
    d.setTextDatum(lgfx::textdatum_t::top_left);
    d.setFont(&fonts::lgfxJapanGothic_24);
}

}  // namespace tab5::ap_qr_screen

#endif  // CONFIG_TAB5_HTTP_CONFIG_ENABLED
