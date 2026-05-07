/*
 * WireGuard ESP-IDF 6.0 integration layer
 * Ported from WireGuard-ESP32-Arduino by Kenta Ida (fuga@fugafuga.org)
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
 * @brief WireGuard tunnel configuration.
 *
 * All string pointers must remain valid for the lifetime of the tunnel
 * (until wireguard_esp32_stop() returns). When passing NULL to
 * wireguard_esp32_start(), the component loads config from NVS (namespace
 * "wireguard"), falling back to Kconfig defaults for any missing keys.
 */
typedef struct {
    const char    *local_ip;         /*!< Local VPN IPv4 address, e.g. "10.0.0.2" */
    const char    *local_netmask;    /*!< Local VPN netmask, e.g. "255.255.255.0" */
    const char    *local_gateway;    /*!< Local VPN gateway, e.g. "0.0.0.0" */
    const char    *private_key;      /*!< Base64-encoded 32-byte private key */
    const char    *peer_public_key;  /*!< Base64-encoded 32-byte peer public key */
    const char    *peer_endpoint;    /*!< Peer hostname or IP string */
    uint16_t       peer_port;        /*!< Peer UDP port */
    uint16_t       listen_port;      /*!< Local listen port (0 = ephemeral) */
    uint16_t       keepalive;        /*!< Persistent keepalive seconds; 0 = disabled */
    const uint8_t *preshared_key;    /*!< Optional 32-byte preshared key, or NULL */
    bool           set_as_default;   /*!< Set WireGuard netif as default route */
} wireguard_config_t;

/**
 * @brief Start the WireGuard tunnel.
 *
 * Resolves the peer endpoint, creates the WireGuard lwIP netif, adds the
 * peer, and initiates the handshake. Must be called after WiFi STA has
 * obtained an IP address.
 *
 * NTP synchronisation is strongly recommended before calling this function.
 * Without a correct system clock, WireGuard handshakes will be rejected by
 * the peer after a device reset (TAI64N timestamp replay protection).
 *
 * @param config  Tunnel parameters. Pass NULL to load from NVS + Kconfig.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t wireguard_esp32_start(const wireguard_config_t *config);

/**
 * @brief Stop and tear down the WireGuard tunnel.
 *
 * Safe to call even if the tunnel never connected or was never started.
 *
 * @return ESP_OK on success.
 */
esp_err_t wireguard_esp32_stop(void);

/**
 * @brief Query whether the WireGuard peer session is active.
 *
 * A peer is "up" when a handshake has completed successfully and the
 * session key has not yet expired.
 *
 * @return true if the peer is up, false otherwise.
 */
bool wireguard_esp32_is_peer_up(void);

/* ============================================================
 * Managed-mode API (used by Tailscale component)
 * ============================================================ */

#include "lwip/ip_addr.h"

/**
 * @brief Start WireGuard in managed mode (no pre-configured peer).
 *
 * Peers are added later via wireguard_esp32_add_peer().
 * The local IP may be updated after the control plane assigns an address
 * by calling wireguard_esp32_set_address().
 *
 * @param private_key_b64  Base64-encoded 32-byte private key (NULL = generate new).
 * @param local_ip         Initial local VPN IPv4 address, or NULL for "0.0.0.0".
 * @param local_netmask    Netmask string, or NULL for "255.255.255.255".
 * @param listen_port      UDP listen port (0 = ephemeral).
 */
esp_err_t wireguard_esp32_start_managed(const char *private_key_b64,
                                        const char *local_ip,
                                        const char *local_netmask,
                                        uint16_t    listen_port);

/**
 * @brief Update the local IP/netmask of the WireGuard interface.
 *
 * Called after the Tailscale control server assigns a 100.x.y.z address.
 */
esp_err_t wireguard_esp32_set_address(const char *local_ip,
                                      const char *local_netmask);

/**
 * @brief Add a peer to the managed WireGuard interface.
 *
 * @param public_key_b64   Base64-encoded 32-byte peer public key.
 * @param allowed_ip       Peer's allowed source IP (e.g. 100.x.y.z), or IPADDR_ANY.
 * @param allowed_mask     Allowed netmask (e.g. /32 = 255.255.255.255), or IPADDR_ANY.
 * @param endpoint         Initial UDP endpoint IP, or NULL / IPADDR_ANY for DERP-only.
 * @param port             UDP port of the peer.
 * @param keepalive        Persistent keepalive interval in seconds (0 = disabled).
 * @param peer_index_out   Receives the allocated peer index on success.
 */
esp_err_t wireguard_esp32_add_peer(const char      *public_key_b64,
                                   const ip_addr_t *allowed_ip,
                                   const ip_addr_t *allowed_mask,
                                   const ip_addr_t *endpoint,
                                   uint16_t         port,
                                   uint16_t         keepalive,
                                   uint8_t         *peer_index_out);

/**
 * @brief Update the UDP endpoint of an existing managed peer.
 *
 * Used by the DISCO component to upgrade from a DERP relay to a direct path.
 */
esp_err_t wireguard_esp32_update_endpoint(uint8_t          peer_index,
                                          const ip_addr_t *new_endpoint,
                                          uint16_t         new_port);

/**
 * @brief Remove a peer from the managed WireGuard interface.
 */
esp_err_t wireguard_esp32_remove_peer(uint8_t peer_index);

/**
 * @brief Retrieve the local WireGuard public key (32 raw bytes).
 *
 * Only valid after wireguard_esp32_start() or wireguard_esp32_start_managed().
 *
 * @param pubkey_out  Buffer of at least 32 bytes to receive the public key.
 */
esp_err_t wireguard_esp32_get_pubkey(uint8_t pubkey_out[32]);

/**
 * @brief Retrieve the local WireGuard private key (32 raw bytes).
 *
 * Only valid after wireguard_esp32_start() or wireguard_esp32_start_managed().
 * Handle with care — callers must zero the buffer when done.
 *
 * @param privkey_out  Buffer of at least 32 bytes to receive the private key.
 */
esp_err_t wireguard_esp32_get_privkey(uint8_t privkey_out[32]);

/**
 * @brief Callback type for sending WireGuard packets via DERP relay.
 *
 * Called by wireguardif when a peer's endpoint is in the DERP pseudo-address
 * range (127.3.3.0/24). The callback should forward the encrypted WireGuard
 * packet through the DERP TLS connection.
 *
 * @param peer_pub  32-byte peer public key (identifies the DERP destination).
 * @param pkt       Encrypted WireGuard packet payload.
 * @param pkt_len   Length of packet.
 */
typedef void (*wireguard_derp_output_fn)(const uint8_t *peer_pub,
                                         const uint8_t *pkt, size_t pkt_len);

/**
 * @brief Register a DERP output callback for routing packets to DERP-only peers.
 *
 * When a WireGuard peer's endpoint IP is in 127.3.3.0/24 (Tailscale DERP
 * pseudo-address range), encrypted packets are passed to this callback instead
 * of being sent via UDP.
 */
void wireguard_esp32_set_derp_output(wireguard_derp_output_fn fn);

#ifdef __cplusplus
}
#endif
