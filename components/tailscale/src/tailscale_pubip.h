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

/**
 * @brief Public-IP discovery result callback.
 *
 * Invoked from the pubip worker task each time a probe successfully
 * decodes an IPv4 dotted-quad AND the (ip, port) tuple has changed
 * since the last successful probe.
 *
 * @param public_ip_be  Discovered public IPv4 in network byte order.
 * @param public_port   CONFIG_TAILSCALE_LISTEN_PORT (host byte order).
 */
typedef void (*ts_pubip_result_fn)(uint32_t public_ip_be, uint16_t public_port);

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
