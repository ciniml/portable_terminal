/*
 * ts2021 Noise IK control-channel handshake.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_tls.h"
#include "tailscale_keys.h"

/* ------------------------------------------------------------------ */
/* Session state (used after handshake for transport encryption)        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  send_key[32];
    uint8_t  recv_key[32];
    uint64_t send_nonce;
    uint64_t recv_nonce;
} ts_noise_session_t;

/* ------------------------------------------------------------------ */
/* Handshake split API                                                  */
/* ------------------------------------------------------------------ */

/* Wire sizes
 * Init:  [2B version][1B type=0x01][2B payload_len=96][32B eph_pub][48B enc_static][16B enc_empty]
 * Resp:  [1B type=0x02][2B payload_len=48][32B resp_eph_pub][16B enc_empty]
 */
#define TS_NOISE_INIT_MSG_LEN  101
#define TS_NOISE_RESP_MSG_LEN   51

/* Intermediate state between ts_noise_build_init and ts_noise_finish */
typedef struct {
    uint8_t h[32];
    uint8_t ck[32];
    uint8_t eph_priv[32];
} ts_noise_hs_t;

/**
 * @brief Build the Noise IK initiator message.
 *
 * Requires the server's Noise static public key (fetched from /key).
 * The caller must send the resulting init_msg in the X-Tailscale-Handshake
 * header of POST /ts2021, then call ts_noise_finish() after receiving the
 * server's response.
 *
 * @param machine_priv  Client machine private key (32 bytes, RFC-7748 clamped)
 * @param machine_pub   Client machine public key (32 bytes)
 * @param srv_pub       Server Noise static public key (32 bytes)
 * @param init_msg      Output: TS_NOISE_INIT_MSG_LEN bytes
 * @param hs            Output: intermediate handshake state
 */
esp_err_t ts_noise_build_init(const uint8_t machine_priv[32],
                               const uint8_t machine_pub[32],
                               const uint8_t srv_pub[32],
                               uint8_t       init_msg[TS_NOISE_INIT_MSG_LEN],
                               ts_noise_hs_t *hs);

/**
 * @brief Finish the Noise IK handshake after receiving the server response.
 *
 * Call after reading TS_NOISE_RESP_MSG_LEN bytes from the connection
 * (immediately after the HTTP 101 response headers).
 *
 * @param hs            Intermediate state from ts_noise_build_init
 * @param machine_priv  Client machine private key (32 bytes)
 * @param resp_msg      Server response: TS_NOISE_RESP_MSG_LEN bytes
 * @param session       Output: populated session keys
 */
esp_err_t ts_noise_finish(ts_noise_hs_t       *hs,
                           const uint8_t        machine_priv[32],
                           const uint8_t        resp_msg[TS_NOISE_RESP_MSG_LEN],
                           ts_noise_session_t  *session);

/* ------------------------------------------------------------------ */
/* Transport AEAD (post-handshake) — framing done by caller            */
/* ------------------------------------------------------------------ */

/**
 * @brief AEAD-encrypt plaintext → ciphertext (plen + 16 bytes).
 *
 * Wire framing ([type][2B BE len] header) is handled by the caller.
 */
esp_err_t ts_noise_encrypt(ts_noise_session_t *session,
                           const uint8_t      *plaintext, size_t plen,
                           uint8_t            *ciphertext);

/**
 * @brief AEAD-decrypt enc_len bytes of ciphertext → plaintext.
 *
 * enc_len must be >= 16.  *plen = enc_len - 16 on success.
 * @return ESP_ERR_INVALID_CRC on authentication failure.
 */
esp_err_t ts_noise_decrypt(ts_noise_session_t *session,
                           const uint8_t      *ciphertext, size_t enc_len,
                           uint8_t            *plaintext,  size_t *plen);
