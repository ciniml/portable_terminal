// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HTTP-based configuration interface (phase 1: read-only firmware info).
//
// Brings up a softAP ("Tab5-XXXXXX") alongside the existing STA
// connection (WIFI_MODE_APSTA) and serves a settings landing page at
// http://192.168.4.1/ plus a JSON firmware-info API at /api/info.
//
// Config *writing* (Wi-Fi credentials, SSH profiles, Tailscale auth
// key) is phase 2 — the URL layout is designed to grow (/api/...)
// but no write endpoint exists yet.
#pragma once

#include "esp_err.h"

namespace tab5::http_config {

// Start the softAP (APSTA mode alongside the existing STA) and the HTTP
// server. AP SSID: "Tab5-XXXXXX" (XXXXXX = lower 3 bytes of the AP MAC,
// hex uppercase). WPA2 password: a non-empty
// CONFIG_TAB5_HTTP_CONFIG_AP_PSK wins; otherwise a random per-device
// 10-char password is generated on first boot and persisted in NVS
// ("httpcfg"/"ap_psk") so it stays stable across reboots. The AP netif
// gets the default 192.168.4.1/24 + DHCP server that esp_netif's
// WIFI_AP default provides. A UDP/53 captive-portal DNS responder
// (captive_portal.cpp) plus HTTP probe handlers make phones pop the
// settings page automatically after joining.
//
// Safe to call after wifi_setup's STA init (esp_wifi_init must have
// run); internally switches mode to WIFI_MODE_APSTA. Idempotent —
// returns ESP_OK if already running. Returns the underlying error
// otherwise (e.g. ESP_ERR_WIFI_NOT_INIT when Wi-Fi never came up).
esp_err_t start();

// Stop HTTP server + softAP, drop back to WIFI_MODE_STA.
void stop();

bool is_running();

// AP SSID for status display (valid after start()).
const char* ap_ssid();

// Effective AP WPA2 password (Kconfig override or the NVS-generated
// per-device one). Valid after start(); empty string when the AP had to
// fall back to open mode. Consumed by the on-LCD QR onboarding screen —
// don't expose it over the HTTP API.
const char* ap_psk();

}  // namespace tab5::http_config
