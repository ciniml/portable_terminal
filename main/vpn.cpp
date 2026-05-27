#include "vpn.hpp"

#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_WIREGUARD_ENABLE
#include "wireguard_esp32.h"
#endif
#if CONFIG_TAILSCALE_ENABLE
#include "tailscale_esp32.h"
#endif

namespace tab5::vpn {

namespace {

constexpr const char* kTag = "vpn";

bool sync_clock(int timeout_s) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = nullptr;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "sntp_init -> %s", esp_err_to_name(err));
        return false;
    }
    for (int i = 0; i < timeout_s; ++i) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) == ESP_OK) {
            time_t now = time(nullptr);
            ESP_LOGI(kTag, "system time synced: %s", ctime(&now));
            return true;
        }
    }
    ESP_LOGW(kTag, "SNTP did not converge in %ds", timeout_s);
    return false;
}

}  // namespace

Kind kind() {
#if CONFIG_TAILSCALE_ENABLE
    return Kind::Tailscale;
#elif CONFIG_WIREGUARD_ENABLE
    return Kind::WireGuard;
#else
    return Kind::None;
#endif
}

bool start(int timeout_s) {
    if (kind() == Kind::None) return false;

    if (!sync_clock(timeout_s / 2 + 1)) {
        ESP_LOGW(kTag, "proceeding without clock sync — handshake may fail");
    }

#if CONFIG_TAILSCALE_ENABLE
    esp_err_t err = tailscale_esp32_start(nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "tailscale_esp32_start -> %s", esp_err_to_name(err));
        return false;
    }
    for (int i = 0; i < timeout_s * 2; ++i) {
        if (tailscale_esp32_is_connected()) {
            char ip[32] = {0};
            tailscale_esp32_get_ip(ip, sizeof(ip));
            ESP_LOGI(kTag, "Tailscale up: %s", ip);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    char url[256] = {0};
    if (tailscale_esp32_get_auth_url(url, sizeof(url)) == ESP_OK && url[0]) {
        ESP_LOGW(kTag, "Tailscale awaiting approval: %s", url);
    } else {
        ESP_LOGW(kTag, "Tailscale not connected in %ds", timeout_s);
    }
    return false;
#elif CONFIG_WIREGUARD_ENABLE
    esp_err_t err = wireguard_esp32_start(nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "wireguard_esp32_start -> %s", esp_err_to_name(err));
        return false;
    }
    for (int i = 0; i < timeout_s * 2; ++i) {
        if (wireguard_esp32_is_peer_up()) {
            ESP_LOGI(kTag, "WireGuard peer up");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(kTag, "WireGuard handshake did not complete in %ds", timeout_s);
    return false;
#else
    (void)timeout_s;
    return false;
#endif
}

bool is_up() {
#if CONFIG_TAILSCALE_ENABLE
    return tailscale_esp32_is_connected();
#elif CONFIG_WIREGUARD_ENABLE
    return wireguard_esp32_is_peer_up();
#else
    return false;
#endif
}

bool get_tailscale_ip(char* out, size_t out_len) {
#if CONFIG_TAILSCALE_ENABLE
    if (!out || !out_len) return false;
    out[0] = '\0';
    return tailscale_esp32_get_ip(out, out_len) == ESP_OK && out[0];
#else
    (void)out; (void)out_len;
    return false;
#endif
}

bool get_pending_auth_url(char* out, size_t out_len) {
#if CONFIG_TAILSCALE_ENABLE
    if (!out || !out_len) return false;
    out[0] = '\0';
    if (tailscale_esp32_get_auth_url(out, out_len) != ESP_OK) return false;
    return out[0] != '\0';
#else
    (void)out; (void)out_len;
    return false;
#endif
}

}  // namespace tab5::vpn
