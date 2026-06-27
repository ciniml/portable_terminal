/*
 * Tailscale client for ESP32 — top-level coordination.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_esp32.h"

#include "tailscale_keys.h"
#include "tailscale_noise.h"
#include "tailscale_control.h"
#include "tailscale_netmap.h"
#include "tailscale_derp.h"
#include "tailscale_disco.h"
#include "tailscale_pubip.h"

#include "wireguard_esp32.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "tailscale";

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

#define NVS_NAMESPACE    "tailscale"
#define NVS_AUTH_KEY     "auth_key"
#define NVS_HOSTNAME     "hostname"
#define NVS_CTRL_SERVER  "ctrl_server"

static bool              s_started = false;
static ts_keys_t         s_keys;
static ts_ctrl_ctx_t     s_ctrl_ctx;
static TaskHandle_t      s_ctrl_task = NULL;

static char s_auth_key[128];
static char s_hostname[64];
static char s_ctrl_server[64];

#define CTRL_TASK_STACK  8192
#define CTRL_TASK_PRIO   5

/* ------------------------------------------------------------------ */
/* NVS config loading                                                   */
/* ------------------------------------------------------------------ */

static void load_config_from_nvs(tailscale_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        /* Use Kconfig defaults */
        strlcpy(s_auth_key,     CONFIG_TAILSCALE_AUTH_KEY,     sizeof(s_auth_key));
        strlcpy(s_hostname,     CONFIG_TAILSCALE_HOSTNAME,     sizeof(s_hostname));
        strlcpy(s_ctrl_server,  CONFIG_TAILSCALE_CONTROL_SERVER, sizeof(s_ctrl_server));
        goto done;
    }

#define LOAD_STR(buf, key, kdefault) do { \
        size_t _sz = sizeof(buf); \
        if (nvs_get_str(h, key, buf, &_sz) != ESP_OK) \
            strlcpy(buf, kdefault, sizeof(buf)); \
    } while (0)

    LOAD_STR(s_auth_key,    NVS_AUTH_KEY,    CONFIG_TAILSCALE_AUTH_KEY);
    LOAD_STR(s_hostname,    NVS_HOSTNAME,    CONFIG_TAILSCALE_HOSTNAME);
    LOAD_STR(s_ctrl_server, NVS_CTRL_SERVER, CONFIG_TAILSCALE_CONTROL_SERVER);

#undef LOAD_STR
    nvs_close(h);

done:
    cfg->auth_key       = s_auth_key;
    cfg->hostname       = s_hostname;
    cfg->control_server = s_ctrl_server;
}

/* ------------------------------------------------------------------ */
/* Pubip result → MapRequest.Endpoints                                  */
/* ------------------------------------------------------------------ */

static void pubip_result_cb(uint32_t ip_be, uint16_t port)
{
    char buf[CTRL_MAX_EP_LEN];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
             (unsigned)((ip_be      ) & 0xFF),
             (unsigned)((ip_be >>  8) & 0xFF),
             (unsigned)((ip_be >> 16) & 0xFF),
             (unsigned)((ip_be >> 24) & 0xFF),
             (unsigned)port);
    const char *eps[1] = { buf };
    ts_ctrl_set_endpoints(eps, 1);
    ts_ctrl_signal_endpoints_dirty();
    ESP_LOGI(TAG, "Public endpoint: %s", buf);
}

/* ------------------------------------------------------------------ */
/* Control-plane task                                                   */
/* ------------------------------------------------------------------ */

static void ctrl_task(void *arg)
{
    ts_ctrl_ctx_t *ctx = (ts_ctrl_ctx_t *)arg;

    while (1) {
        /* Register and get network map */
        esp_err_t err = ts_ctrl_register(ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Registration failed (%s) — retrying in 30s",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        err = ts_ctrl_map_request(ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MapRequest failed (%s) — retrying in 10s",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Start DERP relay if a home server was found in the DERPMap */
        ts_derp_node_t derp_home;
        bool have_derp = ts_netmap_get_derp_home(&derp_home);
        if (have_derp) {
            ts_derp_set_home(&derp_home,
                             ctx->keys->node_priv,
                             ctx->keys->node_pub);
        }

        /* Re-issue MapRequest now that we know our DERP region, so the
         * coordinator can advertise our NetInfo.PreferredDERP to peers. */
        if (have_derp) {
            err = ts_ctrl_map_request(ctx);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Updated MapRequest failed (%s)",
                         esp_err_to_name(err));
            }
        }

        /* Public-IP probe via HTTPS — replaces UDP STUN, which the user's
         * MAP-E upstream (v6プラス / BBIX フルマップ) doesn't return
         * BindingResponses through. Pubip uses TCP 443 which is unaffected.
         * The discovered (ip, port=WG_listen) endpoint is published via
         * MapRequest.Endpoints so peers can CallMeMaybe us, then our DISCO
         * receiver pings them back to open the NAT pinhole. */
        if (have_derp) {
            static bool pubip_inited = false;
            if (!pubip_inited) {
                if (ts_pubip_init(pubip_result_cb) == ESP_OK) {
                    pubip_inited = true;
                }
            } else {
                ts_pubip_kick();
            }
        }

        /* Run MapResponse long-poll loop (blocks until disconnected) */
        ts_ctrl_poll_loop(ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t tailscale_esp32_start(const tailscale_config_t *config)
{
    if (s_started) {
        ESP_LOGW(TAG, "Tailscale already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. Resolve configuration */
    tailscale_config_t local_cfg;
    if (config == NULL) {
        load_config_from_nvs(&local_cfg);
        config = &local_cfg;
    } else {
        strlcpy(s_auth_key,    config->auth_key       ? config->auth_key       : CONFIG_TAILSCALE_AUTH_KEY,     sizeof(s_auth_key));
        strlcpy(s_hostname,    config->hostname        ? config->hostname        : CONFIG_TAILSCALE_HOSTNAME,     sizeof(s_hostname));
        strlcpy(s_ctrl_server, config->control_server  ? config->control_server  : CONFIG_TAILSCALE_CONTROL_SERVER, sizeof(s_ctrl_server));
    }

    ESP_LOGI(TAG, "Starting Tailscale client (host=%s, ctrl=%s)",
             s_hostname, s_ctrl_server);

    /* 2. Load or generate cryptographic keys */
    esp_err_t err = ts_keys_load(&s_keys);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Key load failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Start WireGuard in managed mode using the node key */
    char node_priv_b64[64];
    size_t b64_len = sizeof(node_priv_b64);
    extern bool wireguard_base64_encode(const uint8_t *, size_t, char *, size_t *);
    if (!wireguard_base64_encode(s_keys.node_priv, 32, node_priv_b64, &b64_len)) {
        ESP_LOGE(TAG, "base64 encode of node key failed");
        return ESP_FAIL;
    }

    err = wireguard_esp32_start_managed(node_priv_b64,
                                        NULL,   /* IP assigned later by control server */
                                        NULL,   /* netmask default */
                                        CONFIG_TAILSCALE_LISTEN_PORT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WireGuard managed start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. Register DERP output hook so WireGuard can route packets via DERP */
    wireguard_esp32_set_derp_output(ts_derp_send_packet);

    /* 5. Initialise DISCO. Registers a callback with wireguardif so any
     *    DISCO frame arriving on the WG UDP port (or via DERP injection)
     *    is dispatched to ts_disco_rx. No own socket, no own task. */
    err = ts_disco_init(s_keys.disco_priv, s_keys.disco_pub, s_keys.node_pub);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DISCO init failed (non-fatal): %s", esp_err_to_name(err));
    }

    /* 5. Set up control context */
    memset(&s_ctrl_ctx, 0, sizeof(s_ctrl_ctx));
    s_ctrl_ctx.keys         = &s_keys;
    s_ctrl_ctx.auth_key     = s_auth_key;
    s_ctrl_ctx.hostname     = s_hostname;
    s_ctrl_ctx.control_host = s_ctrl_server;

    /* 6. Launch control-plane task */
    xTaskCreate(ctrl_task, "ts_ctrl", CTRL_TASK_STACK,
                &s_ctrl_ctx, CTRL_TASK_PRIO, &s_ctrl_task);

    s_started = true;
    ESP_LOGI(TAG, "Tailscale client started");
    return ESP_OK;
}

esp_err_t tailscale_esp32_stop(void)
{
    if (!s_started) return ESP_OK;

    ts_ctrl_stop(&s_ctrl_ctx);
    /* Tear down the pubip probe task before everything else; its HTTP
     * client doesn't share state with WG but ordering keeps shutdown
     * predictable. */
    ts_pubip_deinit();
    ts_derp_disconnect();
    /* DISCO has no task/socket of its own; unregister the dispatcher so
     * a future tailscale_esp32_start() can re-init safely. */
    wireguard_esp32_set_disco_input(NULL);
    wireguard_esp32_stop();

    if (s_ctrl_task) {
        vTaskDelete(s_ctrl_task);
        s_ctrl_task = NULL;
    }

    s_started = false;
    ESP_LOGI(TAG, "Tailscale client stopped");
    return ESP_OK;
}

bool tailscale_esp32_is_connected(void)
{
    if (!s_started) return false;
    char ip[20];
    ts_netmap_get_self_ip(ip, sizeof(ip));
    return (ip[0] != '\0');
}

esp_err_t tailscale_esp32_get_ip(char *ip_str, size_t ip_str_len)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    ts_netmap_get_self_ip(ip_str, ip_str_len);
    return (ip_str[0] != '\0') ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t tailscale_esp32_get_auth_url(char *url_str, size_t url_str_len)
{
    if (!url_str || url_str_len == 0) return ESP_ERR_INVALID_ARG;
    const char *src = ts_ctrl_get_auth_url();
    strlcpy(url_str, src ? src : "", url_str_len);
    return ESP_OK;
}

esp_err_t tailscale_esp32_resolve(const char *name,
                                  char       *ip_str,
                                  size_t      ip_str_len)
{
    if (!name || !ip_str || ip_str_len == 0) return ESP_ERR_INVALID_ARG;
    return ts_netmap_resolve(name, ip_str, ip_str_len) ? ESP_OK
                                                       : ESP_ERR_NOT_FOUND;
}
