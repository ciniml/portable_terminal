// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// HTTP configuration service, phase 2b.
//
// SoftAP + esp_http_server. Routes:
//   GET  /              — settings landing page (embedded http_config_page.html)
//   GET  /api/info      — firmware / network status + stored config as JSON
//   POST /api/wifi      — store STA credentials     (NVS via wifi_config)
//   POST /api/profile   — store connection profile 0 (NVS via profiles)
//   POST /api/tailscale — store Tailscale auth key / hostname (NVS "tailscale")
//   POST /api/screenlock— screen-lock enable / timeout / PIN (applies live)
//   POST /api/reboot    — deferred esp_restart()
//
// Write-endpoint contract (also mirrored by the settings page JS):
//   * Bodies are JSON, capped at 1 KB; anything else is rejected.
//   * Writes only touch NVS — nothing reconnects in-line. Responses carry
//     "reboot_required": true and the page offers the reboot button.
//   * Secrets are never echoed back; /api/info only reports *_set flags.
//   * Empty-secret semantics differ per endpoint (documented at each
//     handler): /api/wifi requires ssid+psk every time (open networks
//     need an explicit "open": true), while /api/profile ("password")
//     and /api/tailscale ("auth_key" / "hostname") treat empty or absent
//     fields as "keep the stored value".
//
// Security model: the service is only reachable over the WPA2-protected
// softAP or the trusted STA LAN — no additional auth layer in this phase.
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
#include "profiles.hpp"
#include "screen_lock.hpp"
#include "wifi_config.hpp"
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

    // Stored configuration for form prefill. Secrets themselves are
    // NEVER included — only *_set booleans.
    {
        cJSON* w = cJSON_AddObjectToObject(root, "wifi");
        auto wc = tab5::wifi_config::get();
        cJSON_AddStringToObject(w, "ssid", wc.ssid);
        cJSON_AddBoolToObject(w, "psk_set", wc.psk[0] != '\0');
    }

    {
        cJSON* pr = cJSON_AddObjectToObject(root, "profile");
        if (auto p = tab5::profiles.get(0)) {
            cJSON_AddStringToObject(
                pr, "proto",
                p->proto == tab5::ConnProto::SSH      ? "ssh" :
                p->proto == tab5::ConnProto::Telnet   ? "telnet" : "usb");
            cJSON_AddStringToObject(pr, "host", p->host);
            cJSON_AddNumberToObject(pr, "port", p->port);
            cJSON_AddStringToObject(pr, "user", p->user);
            cJSON_AddBoolToObject(pr, "password_set", p->password[0] != '\0');
        }
    }

    {
        cJSON* sl = cJSON_AddObjectToObject(root, "screenlock");
        auto cfg = tab5::screen_lock::get_config();
        cJSON_AddBoolToObject(sl, "enabled", cfg.enabled);
        cJSON_AddNumberToObject(sl, "timeout_min", cfg.timeout_min);
        cJSON_AddBoolToObject(sl, "pin_set", tab5::screen_lock::pin_is_set());
    }

    {
        cJSON* ts = cJSON_AddObjectToObject(root, "ts_cfg");
        char hostname[64] = {};
        bool key_set = false;
        nvs_handle_t nvs = 0;
        if (nvs_open("tailscale", NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(hostname);
            (void)nvs_get_str(nvs, "hostname", hostname, &len);
            len = 0;  // size query only — never load the key itself
            key_set = nvs_get_str(nvs, "auth_key", nullptr, &len) == ESP_OK &&
                      len > 1;
            nvs_close(nvs);
        }
        cJSON_AddStringToObject(ts, "hostname", hostname);
        cJSON_AddBoolToObject(ts, "auth_key_set", key_set);
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
        // Interactive-auth URL, non-empty while registration is waiting
        // for the user to log in at login.tailscale.com. Suppressed once
        // the tunnel is up — the stored URL only resets on the next
        // register round-trip, so it can be momentarily stale.
        char aurl[200] = {};
        if (!up) tailscale_esp32_get_auth_url(aurl, sizeof(aurl));
        cJSON_AddStringToObject(ts, "auth_url", aurl);
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

// ---- Write API ----------------------------------------------------------

// JSON POST plumbing shared by the /api/ write handlers.

constexpr size_t kMaxBodyLen = 1024;  // Content-Length cap for POST bodies

esp_err_t send_json_error(httpd_req_t* req, const char* status,
                          const char* msg) {
    char body[128];
    std::snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;  // handled — don't let httpd close with its own error
}

esp_err_t send_json_ok(httpd_req_t* req, bool reboot_required) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, reboot_required
                                ? "{\"ok\":true,\"reboot_required\":true}"
                                : "{\"ok\":true}");
    return ESP_OK;
}

// Read the request body (<= kMaxBodyLen) and parse it as JSON. On any
// failure an error response has already been sent and nullptr is
// returned. Caller owns the returned cJSON.
cJSON* read_json_body(httpd_req_t* req, esp_err_t* out_err) {
    if (req->content_len > kMaxBodyLen) {
        *out_err = send_json_error(req, "413 Payload Too Large", "body too large");
        return nullptr;
    }
    char buf[kMaxBodyLen + 1];
    size_t total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, buf + total, req->content_len - total);
        if (n <= 0) {
            *out_err = send_json_error(req, "400 Bad Request", "recv failed");
            return nullptr;
        }
        total += static_cast<size_t>(n);
    }
    buf[total] = '\0';
    cJSON* root = cJSON_ParseWithLength(buf, total);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *out_err = send_json_error(req, "400 Bad Request", "invalid JSON");
        return nullptr;
    }
    *out_err = ESP_OK;
    return root;
}

// Fetch a string field. Returns nullptr when absent or not a string;
// use json_str_or() when absent should read as "".
const char* json_str(const cJSON* root, const char* key) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(it) ? it->valuestring : nullptr;
}

const char* json_str_or(const cJSON* root, const char* key,
                        const char* fallback) {
    const char* s = json_str(root, key);
    return s ? s : fallback;
}

// POST /api/wifi  {"ssid":"...","psk":"...","open":bool?}
//
// Contract: ssid AND psk are required on EVERY submit (there is no
// "keep stored psk" shortcut here — a wrong stored psk is exactly the
// case this endpoint must fix, so silently keeping it is a footgun).
// An empty psk is only accepted together with an explicit "open": true
// (open network). ssid must be 1..32 bytes; a non-empty psk 8..63.
// NVS write only — no in-line reconnect; takes effect on reboot.
esp_err_t handle_api_wifi(httpd_req_t* req) {
    esp_err_t err;
    cJSON* root = read_json_body(req, &err);
    if (!root) return err;

    const char* ssid = json_str(root, "ssid");
    const char* psk  = json_str(root, "psk");
    const cJSON* open_it = cJSON_GetObjectItemCaseSensitive(root, "open");
    const bool open_net = cJSON_IsTrue(open_it);

    if (!ssid || !psk) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "ssid and psk required");
    }
    const size_t ssid_len = std::strlen(ssid);
    const size_t psk_len  = std::strlen(psk);
    if (ssid_len < 1 || ssid_len > 32) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "ssid must be 1-32 bytes");
    }
    if (psk_len == 0 && !open_net) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request",
                               "empty psk requires open:true");
    }
    if (psk_len != 0 && (psk_len < 8 || psk_len > 63)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "psk must be 8-63 bytes");
    }

    auto cfg = tab5::wifi_config::get();  // keep the stored timeout_s
    std::memset(cfg.ssid, 0, sizeof(cfg.ssid));
    std::memset(cfg.psk, 0, sizeof(cfg.psk));
    std::memcpy(cfg.ssid, ssid, ssid_len);
    std::memcpy(cfg.psk, psk, psk_len);
    cJSON_Delete(root);

    if (!tab5::wifi_config::set(cfg)) {
        return send_json_error(req, "500 Internal Server Error", "NVS write failed");
    }
    ESP_LOGI(kTag, "stored Wi-Fi credentials for '%s' (reboot to apply)",
             cfg.ssid);
    return send_json_ok(req, /*reboot_required=*/true);
}

// POST /api/tailscale  {"auth_key":"...","hostname":"..."}
//
// Contract: both fields optional; an empty or absent field keeps the
// stored value (the page prefills hostname and leaves auth_key blank
// with an "(unchanged)" placeholder). A non-empty auth_key must start
// with "tskey-". Written straight to NVS namespace "tailscale" — the
// same keys components/tailscale reads at start — so it applies on the
// next reboot.
esp_err_t handle_api_tailscale(httpd_req_t* req) {
    esp_err_t err;
    cJSON* root = read_json_body(req, &err);
    if (!root) return err;

    const char* auth_key = json_str_or(root, "auth_key", "");
    const char* hostname = json_str_or(root, "hostname", "");

    if (auth_key[0] && std::strncmp(auth_key, "tskey-", 6) != 0) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request",
                               "auth_key must start with tskey-");
    }
    // Size limits from components/tailscale (s_auth_key[128], s_hostname[64]).
    if (std::strlen(auth_key) > 127 || std::strlen(hostname) > 63) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "field too long");
    }

    nvs_handle_t nvs = 0;
    esp_err_t nerr = nvs_open("tailscale", NVS_READWRITE, &nvs);
    if (nerr == ESP_OK) {
        if (auth_key[0]) nerr = nvs_set_str(nvs, "auth_key", auth_key);
        if (nerr == ESP_OK && hostname[0]) {
            nerr = nvs_set_str(nvs, "hostname", hostname);
        }
        if (nerr == ESP_OK) nerr = nvs_commit(nvs);
        nvs_close(nvs);
    }
    cJSON_Delete(root);
    if (nerr != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "NVS write failed");
    }
    ESP_LOGI(kTag, "stored Tailscale config (reboot to apply)");
    return send_json_ok(req, /*reboot_required=*/true);
}

// POST /api/profile
//   {"proto":"ssh"|"telnet","host":"...","port":22,"user":"...","password":"..."}
//
// Updates connection profile 0 (the auto-connect slot); creates it when
// the store is empty. Contract: proto and host are required; port
// defaults to 22 (ssh) / 23 (telnet); an empty or absent password keeps
// profile 0's stored password AND its auth mode (so a host-only edit
// does not clobber a pubkey setup — the key itself stays
// firmware-embedded). A non-empty password switches auth to
// SshAuth::Password. The display name is auto-derived ("proto:host",
// truncated), matching the Kconfig seed convention.
esp_err_t handle_api_profile(httpd_req_t* req) {
    esp_err_t err;
    cJSON* root = read_json_body(req, &err);
    if (!root) return err;

    const char* proto = json_str_or(root, "proto", "");
    const char* host  = json_str_or(root, "host", "");
    const char* user  = json_str_or(root, "user", "");
    const char* pass  = json_str_or(root, "password", "");
    const cJSON* port_it = cJSON_GetObjectItemCaseSensitive(root, "port");

    tab5::ConnProto p_proto;
    if (std::strcmp(proto, "ssh") == 0) {
        p_proto = tab5::ConnProto::SSH;
    } else if (std::strcmp(proto, "telnet") == 0) {
        p_proto = tab5::ConnProto::Telnet;
    } else {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request",
                               "proto must be ssh or telnet");
    }
    const size_t host_len = std::strlen(host);
    if (host_len < 1 || host_len > 63) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "host must be 1-63 bytes");
    }
    int port = cJSON_IsNumber(port_it)
                   ? port_it->valueint
                   : (p_proto == tab5::ConnProto::SSH ? 22 : 23);
    if (port < 1 || port > 65535) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "port must be 1-65535");
    }
    if (std::strlen(user) > 31 || std::strlen(pass) > 63) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "field too long");
    }

    tab5::Profile np{};
    np.proto = p_proto;
    np.port  = static_cast<uint16_t>(port);
    std::strncpy(np.host, host, sizeof(np.host) - 1);
    std::strncpy(np.user, user, sizeof(np.user) - 1);
    std::snprintf(np.name, sizeof(np.name), "%s:%.20s",
                  p_proto == tab5::ConnProto::SSH ? "ssh" : "telnet", host);
    np.auth = tab5::SshAuth::Password;
    if (pass[0]) {
        std::strncpy(np.password, pass, sizeof(np.password) - 1);
    } else if (auto prev = tab5::profiles.get(0)) {
        // Empty password = keep the stored secret and auth mode.
        std::memcpy(np.password, prev->password, sizeof(np.password));
        np.auth = prev->auth;
    }
    cJSON_Delete(root);

    bool ok = tab5::profiles.count() > 0 ? tab5::profiles.update(0, np)
                                         : tab5::profiles.add(np) == 0;
    if (!ok) {
        return send_json_error(req, "500 Internal Server Error", "NVS write failed");
    }
    ESP_LOGI(kTag, "stored profile 0: %s (reboot to apply)", np.name);
    return send_json_ok(req, /*reboot_required=*/true);
}

// POST /api/screenlock
//   {"enabled":bool,"timeout_min":int,"pin":"1234"}
//
// Contract: enabled (bool) and timeout_min (1..1440) are required; pin
// is optional — empty or absent keeps the stored hash (the page leaves
// the field blank with an "(unchanged)" placeholder). A non-empty pin
// must be 4-8 ASCII digits and replaces the stored SHA-256 hash.
// enabled:true is refused unless a PIN is already stored or provided
// in the same request. Applies LIVE — no reboot needed; the settings
// go straight into screen_lock (NVS + runtime atomics).
esp_err_t handle_api_screenlock(httpd_req_t* req) {
    esp_err_t err;
    cJSON* root = read_json_body(req, &err);
    if (!root) return err;

    const cJSON* en_it  = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON* tm_it  = cJSON_GetObjectItemCaseSensitive(root, "timeout_min");
    const char*  pin    = json_str_or(root, "pin", "");

    if (!cJSON_IsBool(en_it)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "enabled required");
    }
    const bool enabled = cJSON_IsTrue(en_it);
    if (!cJSON_IsNumber(tm_it) ||
        tm_it->valueint < 1 || tm_it->valueint > 1440) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request",
                               "timeout_min must be 1-1440");
    }
    const uint16_t timeout_min = static_cast<uint16_t>(tm_it->valueint);

    if (pin[0]) {
        const size_t n = std::strlen(pin);
        bool digits = n >= 4 && n <= 8;
        for (size_t i = 0; digits && i < n; ++i) {
            digits = pin[i] >= '0' && pin[i] <= '9';
        }
        if (!digits) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request",
                                   "pin must be 4-8 digits");
        }
    }
    if (enabled && !pin[0] && !tab5::screen_lock::pin_is_set()) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request",
                               "enabling requires a PIN (none stored)");
    }

    if (pin[0] && !tab5::screen_lock::set_pin(pin)) {
        cJSON_Delete(root);
        return send_json_error(req, "500 Internal Server Error",
                               "NVS write failed");
    }
    cJSON_Delete(root);
    if (!tab5::screen_lock::set_config(enabled, timeout_min)) {
        return send_json_error(req, "500 Internal Server Error",
                               "NVS write failed");
    }
    ESP_LOGI(kTag, "screen lock config: enabled=%d timeout=%u (live)",
             enabled ? 1 : 0, static_cast<unsigned>(timeout_min));
    return send_json_ok(req, /*reboot_required=*/false);
}

// POST /api/reboot — reply {"ok":true} first, restart ~500 ms later on
// an esp_timer so the HTTP response (and the TCP FIN) actually reach
// the browser before the radio goes away.
void reboot_timer_cb(void* /*arg*/) {
    ESP_LOGW(kTag, "rebooting (requested via /api/reboot)");
    esp_restart();
}

esp_err_t handle_api_reboot(httpd_req_t* req) {
    // Drain any (ignored) body so the connection state stays clean.
    if (req->content_len > 0 && req->content_len <= kMaxBodyLen) {
        char sink[128];
        size_t left = req->content_len;
        while (left > 0) {
            int n = httpd_req_recv(
                req, sink, left < sizeof(sink) ? left : sizeof(sink));
            if (n <= 0) break;
            left -= static_cast<size_t>(n);
        }
    }
    esp_err_t err = send_json_ok(req, /*reboot_required=*/false);

    static esp_timer_handle_t s_reboot_timer = nullptr;
    if (!s_reboot_timer) {
        const esp_timer_create_args_t args = {
            .callback = &reboot_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "httpcfg_reboot",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&args, &s_reboot_timer) != ESP_OK) {
            ESP_LOGE(kTag, "reboot timer create failed — restarting inline");
            esp_restart();
        }
    }
    (void)esp_timer_start_once(s_reboot_timer, 500 * 1000);
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
    // 8 KB: the write handlers keep a 1 KB body buffer + a Profile on the
    // handler stack, plus cJSON parse depth.
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16;  // 2 pages + 6 probes + 5 write APIs
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

    // Config write endpoints (see the write-endpoint contract at the top
    // of this file).
    static constexpr struct {
        const char* uri;
        esp_err_t (*handler)(httpd_req_t*);
    } kPostRoutes[] = {
        {"/api/wifi",       &handle_api_wifi},
        {"/api/tailscale",  &handle_api_tailscale},
        {"/api/profile",    &handle_api_profile},
        {"/api/screenlock", &handle_api_screenlock},
        {"/api/reboot",     &handle_api_reboot},
    };
    for (const auto& r : kPostRoutes) {
        const httpd_uri_t post_uri = {
            .uri = r.uri,
            .method = HTTP_POST,
            .handler = r.handler,
            .user_ctx = nullptr,
        };
        if ((err = httpd_register_uri_handler(g_server, &post_uri))
            != ESP_OK) {
            httpd_stop(g_server);
            g_server = nullptr;
            return err;
        }
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

    // Refuse cleanly when Wi-Fi never came up (esp_wifi_init not run).
    // Callers that skipped wifi_sta_connect (virgin device without STA
    // credentials) must run tab5::wifi_hw_init() first — app_main does.
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

    // Started already on the STA path (wifi_sta_connect); a no-op
    // ESP_OK then. On the virgin-device AP-only path this is the call
    // that actually raises the radio.
    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(err));
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
