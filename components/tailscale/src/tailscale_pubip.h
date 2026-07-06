/*
 * Tailscale public-IP discovery via HTTPS (STUN alternative).
 *
 * Fetches the device's public IPv4 from a "what's my IP" HTTPS service
 * (api.ipify.org with checkip.amazonaws.com as fallback) and combines it
 * with CONFIG_TAILSCALE_LISTEN_PORT to form the endpoint that we publish
 * via MapRequest.Endpoints. Used as a replacement for UDP STUN in MAP-E /
 * IPv4-over-IPv6 environments where STUN BindingResponses don't return.
 *
 * Assumes "port preservation" by the upstream NAT (typical for フルマップ
 * MAP-E): the WG socket's source port is exposed unchanged on the WAN
 * side. EIM-symmetric ports would break this assumption — same caveat
 * as the STUN-based path.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of recent public IPs the pubip module remembers and
 * publishes on each result callback. In dual-WAN home setups (IPv4
 * PPPoE + IPv6 IPoE with MAP-E) HTTPS traffic often reaches the "what's
 * my IP" service via a different WAN than WG UDP does, and the returned
 * IP can flap between the two on successive probes. Publishing all the
 * recent IPs at once lets peers try each — one will always match the
 * WG UDP path.
 */
#define TS_PUBIP_MAX_RECENT 4

/**
 * @brief Public-IP discovery result callback.
 *
 * Invoked from the pubip worker task whenever a fresh probe adds a
 * previously-unseen IP to the recent set (or on the very first probe).
 * Passes the full recent set in an LRU-ordered array so the caller can
 * publish all currently-plausible endpoints in one MapRequest.
 *
 * @param ip_be_arr     Array of IPv4 addresses in network byte order,
 *                       most-recent first, up to TS_PUBIP_MAX_RECENT
 *                       entries. Storage is owned by the pubip module
 *                       and only valid for the duration of the call.
 * @param n             Number of entries in ip_be_arr (>= 1, <= max).
 * @param public_port   CONFIG_TAILSCALE_LISTEN_PORT (host byte order).
 */
typedef void (*ts_pubip_result_fn)(const uint32_t *ip_be_arr, size_t n,
                                    uint16_t public_port);

/**
 * @brief One-time initialisation.
 *
 * Spawns the pubip worker task, creates a 5-minute periodic timer, and
 * kicks off the first probe immediately. Idempotent on repeated calls
 * (returns ESP_ERR_INVALID_STATE if already initialised).
 */
esp_err_t ts_pubip_init(ts_pubip_result_fn cb);

/**
 * @brief Cancel and clean up — call from tailscale_esp32_stop().
 */
void ts_pubip_deinit(void);

/**
 * @brief Request one immediate probe.
 *
 * Idempotent: simply gives the worker semaphore. If a probe is already
 * in flight, the worker will run another iteration once it returns.
 */
void ts_pubip_kick(void);

#ifdef __cplusplus
}
#endif
