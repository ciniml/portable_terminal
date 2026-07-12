// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HTTP configuration service, phase 1.
//
// SoftAP + esp_http_server. Routes:
//   GET /          — settings landing page (embedded http_config_page.html)
//   GET /api/info  — firmware / network status as JSON
//
// Phase-2 write endpoints (Wi-Fi credentials, connection profiles,
// Tailscale auth key) will land under /api/ as POSTs — keep new reads
// under /api/ too so the page stays a static shell over a JSON API.
#include "http_config.hpp"

#include "sdkconfig.h"

#if CONFIG_TAB5_HTTP_CONFIG_ENABLED

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "wifi_setup.hpp"

#if CONFIG_TAB5_OTA_ENABLED
#include "ota_task.hpp"
#endif
#if CONFIG_TAILSCALE_ENABLE
#include "tailscale_esp32.h"
#endif

namespace tab5::http_config {

namespace {

constexpr const char* kTag = "tab5_httpcfg";

httpd_handle_t g_server = nullptr;
esp_netif_t* g_ap_netif = nullptr;
bool g_running = false;
char g_ssid[33] = {};

// Landing page embedded via EMBED_TXTFILES (NUL-terminated).
extern "C" const char page_html_start[] asm(
    "_binary_http_config_page_html_start");

esp_err_t handle_root(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page_html_start, HTTPD_RESP_USE_STRLEN);
}

#if CONFIG_TAB5_OTA_ENABLED
const char* ota_phase_name(tab5::ota::Status::Phase p) {
    using Phase = tab5::ota::Status::Phase;
    switch (p) {
    case Phase::Idle:           return "Idle";
    case Phase::Polling:        return "Polling";
    case Phase::Downloading:    return "Downloading";
    case Phase::Verifying:      return "Verifying";
    case Phase::DeferredReboot: return "DeferredReboot";
    case Phase::Failed:         return "Failed";
    }
    return "?";
}
#endif

esp_err_t handle_api_info(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);

    const esp_app_desc_t* app = esp_app_get_description();
    cJSON_AddStringToObject(root, "project", app->project_name);
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "idf", app->idf_ver);
    cJSON_AddStringToObject(root, "compile_date", app->date);
    cJSON_AddStringToObject(root, "compile_time", app->time);

    char sha[17];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(&sha[i * 2], 3, "%02x", app->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(root, "sha256", sha);
    cJSON_AddStringToObject(root, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(
        root, "uptime_s",
        static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free",
                            static_cast<double>(esp_get_free_heap_size()));

    {
        cJSON* sta = cJSON_AddObjectToObject(root, "sta");
        auto st = tab5::wifi_status();
        cJSON_AddBoolToObject(sta, "connected", st.connected);
        char ip[16];
        std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                      static_cast<unsigned>((st.ip4 >> 0) & 0xFF),
                      static_cast<unsigned>((st.ip4 >> 8) & 0xFF),
                      static_cast<unsigned>((st.ip4 >> 16) & 0xFF),
                      static_cast<unsigned>((st.ip4 >> 24) & 0xFF));
        cJSON_AddStringToObject(sta, "ip", st.connected ? ip : "");
        cJSON_AddStringToObject(sta, "ssid", st.ssid);
    }

    {
        cJSON* ap = cJSON_AddObjectToObject(root, "ap");
        cJSON_AddStringToObject(ap, "ssid", g_ssid);
    }

#if CONFIG_TAB5_OTA_ENABLED
    {
        cJSON* ota = cJSON_AddObjectToObject(root, "ota");
        auto snap = tab5::ota::snapshot();
        cJSON_AddStringToObject(ota, "phase", ota_phase_name(snap.phase));
        cJSON_AddStringToObject(ota, "current", snap.current_version);
        cJSON_AddStringToObject(ota, "target", snap.target_version);
    }
#endif

#if CONFIG_TAILSCALE_ENABLE
    {
        cJSON* ts = cJSON_AddObjectToObject(root, "tailscale");
        bool up = tailscale_esp32_is_connected();
        cJSON_AddBoolToObject(ts, "up", up);
        char tip[48] = {};
        if (up && tailscale_esp32_get_ip(tip, sizeof(tip)) == ESP_OK) {
            cJSON_AddStringToObject(ts, "ip", tip);
        } else {
            cJSON_AddStringToObject(ts, "ip", "");
        }
    }
#endif

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

esp_err_t start_httpd() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;   // headroom for phase-2 write endpoints
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&g_server, &cfg);
    if (err != ESP_OK) return err;

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = &handle_root,
        .user_ctx = nullptr,
    };
    const httpd_uri_t info_uri = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = &handle_api_info,
        .user_ctx = nullptr,
    };
    if ((err = httpd_register_uri_handler(g_server, &root_uri)) != ESP_OK ||
        (err = httpd_register_uri_handler(g_server, &info_uri)) != ESP_OK) {
        httpd_stop(g_server);
        g_server = nullptr;
        return err;
    }
    return ESP_OK;
}

}  // namespace

esp_err_t start() {
    if (g_running) return ESP_OK;

    // Refuse cleanly when Wi-Fi never came up (esp_wifi_init not run —
    // e.g. no stored STA credentials, so wifi_sta_connect was skipped).
    // Phase 2 may grow an AP-only bring-up path for virgin devices.
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Wi-Fi not initialised (%s) — softAP unavailable",
                 esp_err_to_name(err));
        return err;
    }

    // Default AP netif: 192.168.4.1/24 + DHCP server. Created once and
    // kept across stop()/start() cycles — destroying a default netif is
    // not cleanly supported.
    if (!g_ap_netif) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
        if (!g_ap_netif) {
            ESP_LOGE(kTag, "esp_netif_create_default_wifi_ap failed");
            return ESP_FAIL;
        }
    }

    if ((err = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK) {
        ESP_LOGE(kTag, "set_mode(APSTA) failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t mac[6] = {};
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) != ESP_OK) {
        ESP_LOGW(kTag, "AP MAC unavailable, SSID suffix falls back to 000000");
    }
    std::snprintf(g_ssid, sizeof(g_ssid), "Tab5-%02X%02X%02X",
                  mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg{};
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), g_ssid,
                 sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = std::strlen(g_ssid);
    ap_cfg.ap.channel = CONFIG_TAB5_HTTP_CONFIG_AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;

    constexpr const char* kPsk = CONFIG_TAB5_HTTP_CONFIG_AP_PSK;
    const size_t psk_len = std::strlen(kPsk);
    if (psk_len >= 8) {
        std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), kPsk,
                     sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (psk_len > 0) {
            ESP_LOGE(kTag, "AP PSK shorter than 8 chars — falling back to "
                     "an OPEN AP (fix CONFIG_TAB5_HTTP_CONFIG_AP_PSK)");
        }
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    if ((err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)) != ESP_OK) {
        ESP_LOGE(kTag, "set_config(AP) failed: %s", esp_err_to_name(err));
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }

    if ((err = start_httpd()) != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed: %s", esp_err_to_name(err));
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }

    g_running = true;
    ESP_LOGI(kTag, "settings service up: SSID=%s auth=%s http://192.168.4.1/",
             g_ssid, ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa2");
    return ESP_OK;
}

void stop() {
    if (!g_running) return;
    if (g_server) {
        httpd_stop(g_server);
        g_server = nullptr;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "set_mode(STA) on stop failed: %s",
                 esp_err_to_name(err));
    }
    g_running = false;
    ESP_LOGI(kTag, "settings service stopped");
}

bool is_running() {
    return g_running;
}

const char* ap_ssid() {
    return g_ssid;
}

}  // namespace tab5::http_config

#endif  // CONFIG_TAB5_HTTP_CONFIG_ENABLED
