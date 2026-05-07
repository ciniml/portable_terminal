/*
 * Tailscale client for ESP32 (ESP-IDF 6.0)
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tailscale client configuration.
 *
 * All string pointers must remain valid until tailscale_esp32_stop() returns.
 * Pass NULL to load configuration from NVS (namespace "tailscale") and
 * fall back to Kconfig defaults for any missing keys.
 */
typedef struct {
    const char *auth_key;       /*!< Pre-auth key (tskey-auth-...), or NULL */
    const char *hostname;       /*!< Device hostname on Tailscale network, or NULL */
    const char *control_server; /*!< Control server URL, or NULL for login.tailscale.com */
} tailscale_config_t;

/**
 * @brief Start the Tailscale client.
 *
 * Registers the device with the Tailscale control server, fetches the
 * network map, and configures WireGuard peers.  Must be called after WiFi
 * STA has obtained an IP address.
 *
 * @param config  Client configuration.  Pass NULL to load from NVS + Kconfig.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t tailscale_esp32_start(const tailscale_config_t *config);

/**
 * @brief Stop the Tailscale client and tear down the WireGuard tunnel.
 *
 * Safe to call even if the client was never started.
 *
 * @return ESP_OK on success.
 */
esp_err_t tailscale_esp32_stop(void);

/**
 * @brief Query whether the Tailscale client is connected and has a valid IP.
 *
 * @return true if the client is connected, false otherwise.
 */
bool tailscale_esp32_is_connected(void);

/**
 * @brief Get the Tailscale IP address assigned to this device.
 *
 * @param ip_str  Buffer of at least 16 bytes to receive the IP string.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t tailscale_esp32_get_ip(char *ip_str, size_t ip_str_len);

/**
 * @brief Copy the most recent AuthURL reported by the control server.
 *
 * Populated when registration succeeds without a usable auth_key, in
 * which case the device cannot send/receive Tailscale traffic until the
 * URL is opened in a browser by an authorised tailnet member. The
 * destination buffer is left as an empty string when no approval is
 * pending.
 *
 * @param url_str      Destination buffer.
 * @param url_str_len  Capacity of `url_str` in bytes (must be >= 1).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on a NULL/zero-size buffer.
 */
esp_err_t tailscale_esp32_get_auth_url(char *url_str, size_t url_str_len);

#ifdef __cplusplus
}
#endif
