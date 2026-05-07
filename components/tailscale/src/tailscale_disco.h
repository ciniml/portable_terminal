/*
 * Tailscale DISCO path-discovery protocol.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lwip/ip_addr.h"

/* DISCO message types */
#define DISCO_MSG_PING  0x01
#define DISCO_MSG_PONG  0x02

/**
 * @brief Start the DISCO UDP socket and receiver task.
 *
 * Opens a UDP socket on an ephemeral port and starts a FreeRTOS task that
 * listens for DiscoPong replies.  On receipt of a valid Pong from a peer,
 * calls wireguard_esp32_update_endpoint() to upgrade the peer's path from
 * DERP relay to a direct UDP route.
 *
 * @param disco_priv  Our DISCO private key (32 bytes).
 * @param disco_pub   Our DISCO public key (32 bytes).
 * @return ESP_OK on success.
 */
esp_err_t ts_disco_start(const uint8_t disco_priv[32],
                         const uint8_t disco_pub[32]);

/**
 * @brief Send a DiscoPing to a peer's known endpoint.
 *
 * The ping is encrypted with the sender's DISCO private key and the
 * recipient's DISCO public key (box construction over x25519 + XChaCha20).
 *
 * @param peer_index    WireGuard peer index (for endpoint update on Pong).
 * @param peer_disco_pub  Peer DISCO public key (32 bytes).
 * @param endpoint      Peer endpoint IP address.
 * @param port          Peer endpoint UDP port.
 * @return ESP_OK on success.
 */
esp_err_t ts_disco_ping(uint8_t peer_index,
                        const uint8_t peer_disco_pub[32],
                        const ip_addr_t *endpoint,
                        uint16_t port);

/**
 * @brief Stop the DISCO task and close the UDP socket.
 */
void ts_disco_stop(void);
