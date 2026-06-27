/*
 * Tailscale ts2021 control-plane client — device registration and MapRequest.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "tailscale_keys.h"
#include "tailscale_noise.h"

/**
 * @brief Context for an active control-plane session.
 *
 * Allocated on the stack by tailscale_esp32_start(); passed into the control
 * task by pointer.
 */
typedef struct {
    ts_keys_t              *keys;
    ts_noise_session_t      noise;
    int                     fd;           /* TLS socket fd (mbedTLS) */
    const char             *control_host; /* e.g. "login.tailscale.com" */
    const char             *auth_key;     /* tskey-auth-... (may be NULL after reg) */
    const char             *hostname;     /* device hostname */
    char                    ts_ip[20];    /* assigned 100.x.y.z */
    bool                    registered;
} ts_ctrl_ctx_t;

/**
 * @brief Register this device with the Tailscale control server.
 *
 * Opens a TLS connection to ctx->control_host, performs the ts2021 Noise IK
 * handshake, and sends a RegisterRequest.  On success, ctx->ts_ip is
 * populated with the assigned Tailscale IP.
 *
 * @param ctx  Control session context (keys and auth_key must be set).
 * @return ESP_OK on success.
 */
esp_err_t ts_ctrl_register(ts_ctrl_ctx_t *ctx);

/**
 * @brief Send a MapRequest and process the initial MapResponse.
 *
 * Must be called after ts_ctrl_register().  Sends a MapRequest (full map,
 * streaming), reads the first MapResponse frame, and invokes
 * ts_netmap_apply() to configure WireGuard peers.
 *
 * This function blocks until the first MapResponse is processed.
 *
 * @param ctx  Active control session (fd and noise must be valid).
 * @return ESP_OK on success.
 */
esp_err_t ts_ctrl_map_request(ts_ctrl_ctx_t *ctx);

/**
 * @brief Run the MapRequest long-poll loop (called from a FreeRTOS task).
 *
 * Sends periodic keepalive pings and processes incremental MapResponse
 * updates to add/remove WireGuard peers.  Never returns normally; call
 * ts_ctrl_stop() to tear down.
 *
 * @param ctx  Active control session.
 */
void ts_ctrl_poll_loop(ts_ctrl_ctx_t *ctx);

/**
 * @brief Signal the control poll loop to exit and close the TLS connection.
 */
void ts_ctrl_stop(ts_ctrl_ctx_t *ctx);

/**
 * @brief Return the most recent AuthURL the control server reported.
 *
 * Non-empty when the device's registration is awaiting interactive
 * approval (i.e. when no auth_key was supplied or the key was invalid /
 * already consumed). Empty string when the device is approved or no
 * registration attempt has been made yet.
 *
 * The returned pointer references a static buffer owned by this module
 * and is only valid until the next ts_ctrl_register() call.
 */
const char *ts_ctrl_get_auth_url(void);

/* Maximum length of a single endpoint string ("a.b.c.d:port" plus NUL). */
#define CTRL_MAX_EP_LEN     48
/* Maximum number of endpoints we publish in MapRequest.Endpoints. */
#define CTRL_MAX_ENDPOINTS  4

/**
 * @brief Replace the published Endpoints list.
 *
 * Strings of the form "a.b.c.d:port" (or "[v6]:port"), copied into
 * module-local storage so the caller may free its strings after return.
 * Caller should also call ts_ctrl_signal_endpoints_dirty() to request a
 * MapRequest re-issuance on the next poll-loop iteration.
 *
 * @param eps  Array of NUL-terminated endpoint strings.
 * @param n    Number of entries (clamped to CTRL_MAX_ENDPOINTS).
 */
void ts_ctrl_set_endpoints(const char *const *eps, int n);

/**
 * @brief Notify the poll loop that endpoints changed.
 *
 * Best-effort: the long-poll loop checks the dirty flag after each
 * MapResponse segment. If it's blocked on map_read_one, the update
 * lands on the next segment arrival or reconnect.
 */
void ts_ctrl_signal_endpoints_dirty(void);

/**
 * @brief Snapshot the currently-published Endpoints into the caller buffer.
 *
 * Strings are "a.b.c.d:port" format, NUL-terminated. Returns the count
 * written, which is min(max, the current set's size) and 0..CTRL_MAX_ENDPOINTS.
 *
 * @param out  Destination buffer; entries are CTRL_MAX_EP_LEN bytes each.
 * @param max  Maximum number of entries the caller can accept.
 * @return Number of endpoint strings written.
 */
int ts_ctrl_get_endpoints(char out[][CTRL_MAX_EP_LEN], int max);
