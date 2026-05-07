/*
 * Tailscale key management — machine, node, and DISCO curve25519 keypairs.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_keys.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

/* crypto primitives from the wireguard component */
#include "crypto.h"          /* wireguard_x25519 macro */
#include "wireguard-platform.h" /* wireguard_random_bytes */

#include <string.h>
#include <stdio.h>

static const char *TAG = "ts_keys";

#define NVS_NAMESPACE   "tailscale"
#define NVS_MACHINE_PRV "machine_priv"
#define NVS_NODE_PRV    "node_priv"
#define NVS_DISCO_PRV   "disco_priv"

/* curve25519 base point: u = 9 */
static const uint8_t s_basepoint[32] = { 9 };

/* ------------------------------------------------------------------ */

static void gen_keypair(uint8_t priv[TS_KEY_LEN], uint8_t pub[TS_KEY_LEN])
{
    wireguard_random_bytes(priv, TS_KEY_LEN);
    /* RFC 7748 clamping */
    priv[0]  &= 248;
    priv[31] &= 127;
    priv[31] |= 64;
    wireguard_x25519(pub, priv, s_basepoint);
}

static esp_err_t load_or_gen_key(nvs_handle_t h, const char *nvs_key,
                                 uint8_t priv[TS_KEY_LEN], uint8_t pub[TS_KEY_LEN])
{
    size_t key_len = TS_KEY_LEN;
    esp_err_t err = nvs_get_blob(h, nvs_key, priv, &key_len);
    if (err == ESP_OK && key_len == TS_KEY_LEN) {
        wireguard_x25519(pub, priv, s_basepoint);
        ESP_LOGI(TAG, "Loaded key '%s' from NVS", nvs_key);
        return ESP_OK;
    }

    /* Not found or wrong size — generate a new key */
    gen_keypair(priv, pub);
    err = nvs_set_blob(h, nvs_key, priv, TS_KEY_LEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save key '%s': %s", nvs_key, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Generated and saved new key '%s'", nvs_key);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t ts_keys_load(ts_keys_t *keys)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t ret = ESP_OK;
    ret = ret ?: load_or_gen_key(h, NVS_MACHINE_PRV,
                                 keys->machine_priv, keys->machine_pub);
    ret = ret ?: load_or_gen_key(h, NVS_NODE_PRV,
                                 keys->node_priv, keys->node_pub);
    ret = ret ?: load_or_gen_key(h, NVS_DISCO_PRV,
                                 keys->disco_priv, keys->disco_pub);

    if (ret == ESP_OK) {
        err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        }
    }

    nvs_close(h);
    return ret;
}

/* ------------------------------------------------------------------ */

void ts_key_to_hex(const char *prefix, const uint8_t key[TS_KEY_LEN],
                   char *buf, size_t buf_len)
{
    size_t prefix_len = strlen(prefix);
    if (prefix_len >= buf_len) {
        buf[0] = '\0';
        return;
    }
    memcpy(buf, prefix, prefix_len);
    char *p = buf + prefix_len;
    size_t remaining = buf_len - prefix_len;
    for (int i = 0; i < TS_KEY_LEN && remaining > 2; i++, remaining -= 2) {
        snprintf(p, remaining, "%02x", key[i]);
        p += 2;
    }
    *p = '\0';
}

bool ts_key_from_hex(const char *hex_str, uint8_t key_out[TS_KEY_LEN])
{
    if (!hex_str) return false;

    /* Skip any "prefix:" portion */
    const char *colon = strchr(hex_str, ':');
    const char *hex = colon ? colon + 1 : hex_str;

    if (strlen(hex) < TS_KEY_LEN * 2) return false;

    for (int i = 0; i < TS_KEY_LEN; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%02x", &byte) != 1) return false;
        key_out[i] = (uint8_t)byte;
    }
    return true;
}
