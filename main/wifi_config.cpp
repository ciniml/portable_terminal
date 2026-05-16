#include "wifi_config.hpp"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace tab5::wifi_config {

namespace {

constexpr const char* kTag = "wifi_cfg";
constexpr const char* kNs  = "wifi";

Config g_cfg{};

}  // namespace

void init() {
    g_cfg = Config{};
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(%s) failed", kNs);
        return;
    }

    size_t sz = sizeof(g_cfg.ssid);
    esp_err_t err = nvs_get_str(nh, "ssid", g_cfg.ssid, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Seed from Kconfig.
#if CONFIG_TAB5_WIFI_ENABLED
        if (CONFIG_TAB5_WIFI_SSID[0]) {
            strncpy(g_cfg.ssid, CONFIG_TAB5_WIFI_SSID, sizeof(g_cfg.ssid) - 1);
            strncpy(g_cfg.psk,  CONFIG_TAB5_WIFI_PSK,  sizeof(g_cfg.psk)  - 1);
            g_cfg.timeout_s = CONFIG_TAB5_WIFI_CONNECT_TIMEOUT_S;
            nvs_set_str(nh, "ssid", g_cfg.ssid);
            nvs_set_str(nh, "psk",  g_cfg.psk);
            nvs_set_i32(nh, "timeout", g_cfg.timeout_s);
            nvs_commit(nh);
            ESP_LOGI(kTag, "seeded Wi-Fi from Kconfig: %s", g_cfg.ssid);
        }
#endif
    } else if (err == ESP_OK) {
        sz = sizeof(g_cfg.psk);
        nvs_get_str(nh, "psk", g_cfg.psk, &sz);
        int32_t t = 0;
        if (nvs_get_i32(nh, "timeout", &t) == ESP_OK) g_cfg.timeout_s = t;
    }
    nvs_close(nh);
    if (g_cfg.timeout_s <= 0) g_cfg.timeout_s = 20;
    ESP_LOGI(kTag, "Wi-Fi config: ssid='%s' timeout=%ds",
             g_cfg.ssid[0] ? g_cfg.ssid : "(none)", g_cfg.timeout_s);
}

Config get() { return g_cfg; }

bool set(const Config& c) {
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READWRITE, &nh) != ESP_OK) return false;
    g_cfg = c;
    if (g_cfg.timeout_s <= 0) g_cfg.timeout_s = 20;
    nvs_set_str(nh, "ssid", g_cfg.ssid);
    nvs_set_str(nh, "psk",  g_cfg.psk);
    nvs_set_i32(nh, "timeout", g_cfg.timeout_s);
    esp_err_t err = nvs_commit(nh);
    nvs_close(nh);
    return err == ESP_OK;
}

bool has_credentials() {
    return g_cfg.ssid[0] != '\0';
}

}  // namespace tab5::wifi_config
