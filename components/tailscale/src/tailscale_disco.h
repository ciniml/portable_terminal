/*
 * Tailscale DISCO peer-discovery / path-confirmation.
 *
 * What it does
 * ------------
 * Tailscale peers run a STUN-like protocol on top of UDP (or DERP) so
 * that each side can confirm the OTHER side's direct UDP endpoint
 * before trusting WireGuard packets to it. Without DISCO, the standard
 * WireGuard "last-source roaming" decision is overridden by
 * Tailscale magicsock's `bestAddr` and our peer keeps falling back to
 * the DERP relay — which is the bug Tab5 was hitting (~4.7 KB/s, far
 * worse than DERP-only ~82 KB/s).
 *
 * Wire format (upstream `tailscale.com/disco`)
 * --------------------------------------------
 *   [ magic[6]             "TS💬" = 54 53 F0 9F 92 AC ]
 *   [ senderDiscoPub[32]   sender's Curve25519 disco public key ]
 *   [ nonce[24]            random per packet ]
 *   [ box                  NaCl box = X25519 → HSalsa20 → XSalsa20-Poly1305
 *                          plaintext layout: [type:1][version=0:1][body...] ]
 *
 *   Ping body: TxID[12] || NodeKey[32]              (we ignore MTU pad)
 *   Pong body: TxID[12] || SrcIP[16, v4-mapped v6] || SrcPort[2, BE]
 *
 * Plumbing
 * --------
 * DISCO frames ride the SAME UDP port as WireGuard (default 41641).
 * wireguardif_network_rx peeks the first 6 bytes; a magic match is
 * handed to ts_disco_rx() via wireguard_esp32_set_disco_input(). For
 * TX we borrow the WG udp_pcb through wireguard_esp32_udp_send(), so
 * the peer sees src = our 41641 and learns the right endpoint to
 * route WireGuard data to.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DISCO message types (subset we implement — Ping/Pong only). */
#define DISCO_MSG_PING  0x01
#define DISCO_MSG_PONG  0x02

/**
 * @brief Initialise DISCO state and register the WG dispatch callback.
 *
 * Stores our DISCO keypair and NodeKey, then calls
 * wireguard_esp32_set_disco_input(internal_rx) so DISCO frames that
 * arrive on the WG UDP port (direct or via DERP injection) flow into
 * us. Idempotent; safe to call once per Tailscale start.
 *
 * @param disco_priv  Our DISCO Curve25519 private key (32 bytes).
 * @param disco_pub   Our DISCO Curve25519 public key  (32 bytes).
 * @param node_pub    Our WireGuard NodeKey (32 bytes); included in Ping bodies
 *                    so peers can disambiguate which WG peer we are.
 * @return ESP_OK on success.
 */
esp_err_t ts_disco_init(const uint8_t disco_priv[32],
                        const uint8_t disco_pub[32],
                        const uint8_t node_pub[32]);

/**
 * @brief Send a DISCO Ping to a peer's candidate endpoint.
 *
 * The ping is sent from the WG UDP socket (our listen port, default
 * 41641) so the peer sees us at the same source it will need for
 * WireGuard return traffic. When the corresponding Pong comes back
 * matching our random TxID, the receiver path calls
 * wireguard_esp32_update_endpoint(wg_peer_index, src_ip, src_port) —
 * that's what flips outbound WG off DERP onto the direct path.
 *
 * @param wg_peer_index   WireGuard peer index to update on Pong receipt.
 * @param peer_node_pub   Peer's NodeKey, optional but mostly always set.
 *                        Pass NULL to skip the NodeKey field in the Ping body.
 * @param peer_disco_pub  Peer's DISCO Curve25519 public key (32 bytes).
 * @param cand_ip         Candidate IPv4 endpoint to probe.
 * @param cand_port       Candidate UDP port.
 * @return ESP_OK on success.
 */
esp_err_t ts_disco_send_ping(uint8_t wg_peer_index,
                             const uint8_t peer_node_pub[32],
                             const uint8_t peer_disco_pub[32],
                             const ip_addr_t *cand_ip, uint16_t cand_port);

#ifdef __cplusplus
}
#endif
