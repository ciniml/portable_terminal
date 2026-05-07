/*
 * Tailscale DERP relay client.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* DERP frame types (https://pkg.go.dev/tailscale.com/derp) */
#define DERP_FRAME_SERVER_KEY      0x01
#define DERP_FRAME_CLIENT_INFO     0x02
#define DERP_FRAME_SERVER_INFO     0x03
#define DERP_FRAME_SEND_PACKET     0x04
#define DERP_FRAME_RECV_PACKET     0x05
#define DERP_FRAME_KEEP_ALIVE      0x06
#define DERP_FRAME_NOTE_PREFERRED  0x07
#define DERP_FRAME_PEER_GONE       0x08
#define DERP_FRAME_PEER_PRESENT    0x09
#define DERP_FRAME_WATCH_CONNS     0x10
#define DERP_FRAME_CLOSE_PEER      0x11
#define DERP_FRAME_PING            0x12
#define DERP_FRAME_PONG            0x13
#define DERP_FRAME_HEALTH          0x14
#define DERP_FRAME_RESTARTING      0x15

/**
 * @brief DERP server node descriptor (from DERPMap in MapResponse).
 */
typedef struct {
    char hostname[64];
    uint16_t stun_port;
    uint16_t derp_port;  /* typically 443 */
    int region_id;       /* Tailscale DERPMap region ID (>0 when valid) */
} ts_derp_node_t;

/**
 * @brief Connect to the DERP home server.
 *
 * Opens a TLS connection to the DERP server, performs the HTTP/1.1 upgrade
 * to the DERP protocol, and starts a background receive task that injects
 * incoming WireGuard packets into the wireguardif input path.
 *
 * @param node       DERP server to connect to.
 * @param node_priv  Our node private key (32 bytes) for DERP auth.
 * @param node_pub   Our node public key (32 bytes).
 * @return ESP_OK on success.
 */
esp_err_t ts_derp_connect(const ts_derp_node_t *node,
                          const uint8_t node_priv[32],
                          const uint8_t node_pub[32]);

/**
 * @brief Send a WireGuard packet to a peer via DERP.
 *
 * Wraps the packet in a DERP SendPacket frame and writes it to the TLS
 * connection.  Thread-safe (uses an internal mutex).
 *
 * @param dst_pub   Destination peer public key (32 bytes).
 * @param pkt       WireGuard packet payload.
 * @param pkt_len   Payload length in bytes.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t ts_derp_send(const uint8_t dst_pub[32],
                       const uint8_t *pkt, size_t pkt_len);

/**
 * @brief Disconnect from DERP and stop the receive task.
 */
void ts_derp_disconnect(void);

/**
 * @brief Update the home DERP server (called when DERPMap changes).
 *
 * If already connected to a different server, the old connection is closed
 * and a new one is established.
 */
esp_err_t ts_derp_set_home(const ts_derp_node_t *node,
                           const uint8_t node_priv[32],
                           const uint8_t node_pub[32]);

/**
 * @brief WireGuard DERP output hook — sends an encrypted WireGuard packet
 *        via DERP to the peer identified by @p peer_pub.
 *
 * Signature matches wireguard_derp_output_fn in wireguard_esp32.h.
 * Register via wireguard_esp32_set_derp_output(ts_derp_send_packet).
 */
void ts_derp_send_packet(const uint8_t *peer_pub,
                         const uint8_t *pkt, size_t pkt_len);
