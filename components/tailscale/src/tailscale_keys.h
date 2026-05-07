/*
 * Tailscale key management — machine, node, and DISCO curve25519 keypairs.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Raw curve25519 key length */
#define TS_KEY_LEN 32

/**
 * @brief Tailscale key set.
 *
 * Three curve25519 keypairs:
 *   machine  — long-lived device identity, used for ts2021 Noise IK
 *   node     — WireGuard data-plane key (registered as NodeKey)
 *   disco    — ephemeral DISCO path-discovery key
 *
 * All keys are persisted in NVS namespace "tailscale".
 */
typedef struct {
    uint8_t machine_priv[TS_KEY_LEN];
    uint8_t machine_pub[TS_KEY_LEN];
    uint8_t node_priv[TS_KEY_LEN];
    uint8_t node_pub[TS_KEY_LEN];
    uint8_t disco_priv[TS_KEY_LEN];
    uint8_t disco_pub[TS_KEY_LEN];
} ts_keys_t;

/**
 * @brief Load keys from NVS, generating any that are missing.
 *
 * On first boot all three keypairs are generated and saved.
 * Subsequent boots reload them from NVS.
 *
 * @param keys  Output key set.
 * @return ESP_OK on success.
 */
esp_err_t ts_keys_load(ts_keys_t *keys);

/**
 * @brief Encode a raw 32-byte key as a Tailscale hex-prefixed string.
 *
 * Tailscale uses "nodekey:HEX" and "discokey:HEX" formats in JSON.
 *
 * @param prefix   e.g. "nodekey:", "discokey:", "" (no prefix for machine key)
 * @param key      Raw 32-byte key.
 * @param buf      Output buffer.
 * @param buf_len  Buffer size; needs to be at least strlen(prefix) + 65.
 */
void ts_key_to_hex(const char *prefix, const uint8_t key[TS_KEY_LEN],
                   char *buf, size_t buf_len);

/**
 * @brief Decode a hex string (optionally prefixed) to raw 32 bytes.
 *
 * Skips any "prefix:" portion before the hex digits.
 *
 * @return true on success, false if the string is malformed.
 */
bool ts_key_from_hex(const char *hex_str, uint8_t key_out[TS_KEY_LEN]);
