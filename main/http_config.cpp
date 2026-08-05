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
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "captive_portal.hpp"
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
char g_psk[65] = {};

// NVS home for the generated AP passphrase. Namespace is shared with
// future HTTP-config settings.
constexpr const char* kNvsNamespace = "httpcfg";
constexpr const char* kNvsKeyApPsk  = "ap_psk";

// Resolve the effective AP passphrase into g_psk:
//   1. Non-empty CONFIG_TAB5_HTTP_CONFIG_AP_PSK (>= 8 chars) wins — dev
//      convenience for a fixed, known password.
//   2. Otherwise a per-device 10-char password is generated once and
//      persisted in NVS ("httpcfg"/"ap_psk"), so it stays stable across
//      reboots. Charset avoids the ambiguous 0/o/1/l glyphs since users
//      may have to type it manually if they can't scan the QR code.
// Only an unrecoverable NVS failure leaves g_psk empty (open AP).
void resolve_ap_psk() {
    constexpr const char* kCfgPsk = CONFIG_TAB5_HTTP_CONFIG_AP_PSK;
    const size_t cfg_len = std::strlen(kCfgPsk);
    if (cfg_len >= 8) {
        std::snprintf(g_psk, sizeof(g_psk), "%s", kCfgPsk);
        return;
    }
    if (cfg_len > 0) {
        ESP_LOGE(kTag, "CONFIG_TAB5_HTTP_CONFIG_AP_PSK shorter than 8 chars "
                 "— ignoring it and using the generated per-device password");
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(%s) failed: %s — AP will be OPEN",
                 kNvsNamespace, esp_err_to_name(err));
        g_psk[0] = '\0';
        return;
    }
    size_t len = sizeof(g_psk);
    err = nvs_get_str(nvs, kNvsKeyApPsk, g_psk, &len);
    if (err == ESP_OK && std::strlen(g_psk) >= 8) {
        nvs_close(nvs);
        return;
    }

    // First boot (or corrupted entry): generate and persist.
    // Lowercase + digits minus ambiguous 0/o/1/l → 32 symbols, so a
    // 10-char password carries 50 bits of entropy.
    constexpr char kCharset[] = "abcdefghijkmnpqrstuvwxyz23456789";
    constexpr size_t kSetLen = sizeof(kCharset) - 1;
    static_assert(kSetLen == 32);
    constexpr size_t kPskLen = 10;
    for (size_t i = 0; i < kPskLen; ++i) {
        g_psk[i] = kCharset[esp_random() % kSetLen];
    }
    g_psk[kPskLen] = '\0';
    if ((err = nvs_set_str(nvs, kNvsKeyApPsk, g_psk)) != ESP_OK ||
        (err = nvs_commit(nvs)) != ESP_OK) {
        ESP_LOGW(kTag, "persisting AP password failed: %s — password will "
                 "rotate on next boot", esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "generated per-device AP password (NVS %s/%s)",
                 kNvsNamespace, kNvsKeyApPsk);
    }
    nvs_close(nvs);
}

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
        // Never leak the password itself over the API — anyone on the AP
        // already knows it, but keep the JSON clean.
        cJSON_AddBoolToObject(ap, "psk_set", std::strlen(g_psk) >= 8);
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

// Captive-portal probe / catch-all response: plain 302 to the settings
// page on the AP IP. iOS probes (`/hotspot-detect.html`, ...) follow this
// and the OS pops the captive sheet directly on the settings page; Android
// and Windows behave the same via their own probe URLs. Do NOT be tempted
// to answer Android's `/generate_204` with a genuine 204 — Android also
// verifies connectivity over HTTPS, so an HTTP-only 204 never convinces
// it there is real internet; it just drops the captive-portal state and
// auto-switches back to a saved AP (observed on stackchan-idf).
esp_err_t redirect_to_portal(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Tab5 settings — see http://192.168.4.1/");
    return ESP_OK;
}

esp_err_t handle_probe(httpd_req_t* req) {
    return redirect_to_portal(req);
}

// 404 catch-all: any unknown path (typed URLs, other OS probe variants)
// also lands on the portal while the AP is up.
esp_err_t handle_404(httpd_req_t* req, httpd_err_code_t /*err*/) {
    return redirect_to_portal(req);
}

esp_err_t start_httpd() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 16;  // 2 pages + 6 probes + phase-2 headroom
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

    // OS captive-portal detection probes. Android / Apple / Windows each
    // poke a well-known URL after association; the 302 below (together
    // with the DNS hijack in captive_portal.cpp) is what makes the
    // "sign in to network" sheet pop with our settings page.
    static constexpr const char* kProbeUris[] = {
        "/generate_204",              // Android
        "/gen_204",                   // Android (older)
        "/hotspot-detect.html",       // Apple
        "/library/test/success.html", // Apple (older)
        "/connecttest.txt",           // Windows NCSI
        "/ncsi.txt",                  // Windows NCSI (legacy)
    };
    for (const char* uri : kProbeUris) {
        const httpd_uri_t probe_uri = {
            .uri = uri,
            .method = HTTP_GET,
            .handler = &handle_probe,
            .user_ctx = nullptr,
        };
        if ((err = httpd_register_uri_handler(g_server, &probe_uri))
            != ESP_OK) {
            httpd_stop(g_server);
            g_server = nullptr;
            return err;
        }
    }
    // Everything else 302s to the portal too.
    (void)httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND,
                                     &handle_404);
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

    // Kconfig override, or the persisted per-device password (generated
    // on first boot). Empty only if NVS is completely broken.
    resolve_ap_psk();
    if (std::strlen(g_psk) >= 8) {
        std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), g_psk,
                     sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ESP_LOGE(kTag, "no usable AP password — falling back to an OPEN AP");
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

    // DNS hijack so phone captive-portal probes resolve to 192.168.4.1.
    captive_portal::start();

    g_running = true;
    ESP_LOGI(kTag, "settings service up: SSID=%s auth=%s http://192.168.4.1/",
             g_ssid, ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa2");
    return ESP_OK;
}

void stop() {
    if (!g_running) return;
    captive_portal::stop();
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

const char* ap_psk() {
    return g_psk;
}

}  // namespace tab5::http_config

#endif  // CONFIG_TAB5_HTTP_CONFIG_ENABLED
