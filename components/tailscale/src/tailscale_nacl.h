/*
 * NaCl box (X25519 → HSalsa20 → XSalsa20-Poly1305) helpers shared
 * between Tailscale DERP (ClientInfo box) and DISCO (Ping/Pong) paths.
 *
 * These do NOT include the public-key wrapper — caller is responsible
 * for computing the shared secret first via wireguard_x25519(shared,
 * our_priv, peer_pub). That keeps the helpers cipher-agnostic and lets
 * callers cache the precompute per peer.
 *
 * Wire convention: out = tag(16) || ciphertext(N). The 24-byte nonce
 * is supplied separately by the caller (it's part of the enclosing
 * Tailscale frame layout, not of the box itself). This matches Go's
 * `crypto/nacl/box.SealAfterPrecomputation` which returns
 * tag||ciphertext appended to the caller buffer.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Box overhead (Poly1305 tag bytes added on top of the plaintext). */
#define TS_NACL_BOX_OVERHEAD 16

/**
 * @brief Encrypt + authenticate `pt` with NaCl box (XSalsa20-Poly1305).
 *
 * @param shared  X25519 shared secret (32 bytes).
 * @param nonce   24-byte unique-per-key nonce.
 * @param pt      Plaintext.
 * @param ptlen   Plaintext length.
 * @param out     Output buffer; must be at least ptlen + TS_NACL_BOX_OVERHEAD
 *                bytes. Layout = tag(16) || ciphertext(ptlen).
 */
void ts_nacl_box_seal(const uint8_t shared[32],
                      const uint8_t nonce[24],
                      const uint8_t *pt, size_t ptlen,
                      uint8_t *out);

/**
 * @brief Verify + decrypt a NaCl box.
 *
 * @param shared  X25519 shared secret (32 bytes).
 * @param nonce   24-byte nonce used at seal time.
 * @param in      Input buffer; layout = tag(16) || ciphertext(N).
 * @param inlen   Input length; must be >= TS_NACL_BOX_OVERHEAD.
 * @param out_pt  Output plaintext, must be at least inlen - TS_NACL_BOX_OVERHEAD bytes.
 * @return true if the tag verified, false otherwise. On false, out_pt
 *         contents are undefined.
 */
bool ts_nacl_box_open(const uint8_t shared[32],
                      const uint8_t nonce[24],
                      const uint8_t *in, size_t inlen,
                      uint8_t *out_pt);

#ifdef __cplusplus
}
#endif
