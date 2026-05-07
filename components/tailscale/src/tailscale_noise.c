/*
 * ts2021 Noise IK control-channel handshake (Tailscale variant).
 *
 * Protocol:  Noise_IK_25519_ChaChaPoly_BLAKE2s
 * HKDF:      standard HKDF (RFC 5869) with HMAC-BLAKE2s as the PRF
 *            (NOT WireGuard's keyed-BLAKE2s HKDF)
 * Prologue:  "Tailscale Control Protocol v1"
 * Init:      5-byte header + 96-byte payload = 101 bytes total
 * Response:  3-byte header + 48-byte payload = 51 bytes total
 * Payload:   empty (nil plaintext → 16-byte AEAD tag only)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_noise.h"

#include "esp_log.h"

/* crypto primitives from wireguard component */
#include "crypto.h"
#include "wireguard-platform.h"

#include <string.h>
#include <stdbool.h>

static const char *TAG = "ts_noise";

/* ------------------------------------------------------------------ */
/* Noise protocol constants                                             */
/* ------------------------------------------------------------------ */

static const char NOISE_CONSTRUCTION[] = "Noise_IK_25519_ChaChaPoly_BLAKE2s";
static const char NOISE_PROLOGUE[]     = "Tailscale Control Protocol v1";

/* ------------------------------------------------------------------ */
/* HMAC-BLAKE2s                                                         */
/*                                                                      */
/* Standard HMAC construction using unkeyed BLAKE2s as the hash.       */
/* BLAKE2s block size = 64 bytes.                                       */
/* data_len must be <= 33 bytes in this codebase (DH result or HKDF    */
/* expand counter).                                                     */
/* ------------------------------------------------------------------ */

static void hmac_blake2s(uint8_t out[32],
                         const uint8_t *key, size_t key_len,
                         const uint8_t *data, size_t data_len)
{
    uint8_t key_padded[64];
    memset(key_padded, 0, sizeof(key_padded));
    if (key_len > 64) {
        wireguard_blake2s(key_padded, 32, NULL, 0, key, key_len);
    } else {
        memcpy(key_padded, key, key_len);
    }

    /* inner = BLAKE2s(ipad_64 || data) */
    uint8_t buf[128]; /* 64-byte pad + up to 64-byte data */
    for (int i = 0; i < 64; i++) buf[i] = key_padded[i] ^ 0x36u;
    if (data && data_len) memcpy(buf + 64, data, data_len);
    uint8_t inner[32];
    wireguard_blake2s(inner, 32, NULL, 0, buf, 64 + data_len);

    /* out = BLAKE2s(opad_64 || inner_32) */
    for (int i = 0; i < 64; i++) buf[i] = key_padded[i] ^ 0x5cu;
    memcpy(buf + 64, inner, 32);
    wireguard_blake2s(out, 32, NULL, 0, buf, 96);
}

/* ------------------------------------------------------------------ */
/* HKDF-Extract + Expand (RFC 5869) with HMAC-BLAKE2s                  */
/*                                                                      */
/* hkdf_blake2s(salt=ck, ikm, ikm_len, out1, out2)                     */
/*   Extract: PRK = HMAC-BLAKE2s(salt, ikm)                            */
/*   Expand:  out1 = T(1) = HMAC-BLAKE2s(PRK, 0x01)                   */
/*            out2 = T(2) = HMAC-BLAKE2s(PRK, T(1) || 0x02)  (if !NULL) */
/* ------------------------------------------------------------------ */

static void hkdf_blake2s(const uint8_t ck[32],
                         const uint8_t *ikm, size_t ikm_len,
                         uint8_t out1[32], uint8_t out2[32])
{
    uint8_t prk[32];
    hmac_blake2s(prk, ck, 32, ikm, ikm_len);

    uint8_t ctr = 0x01;
    hmac_blake2s(out1, prk, 32, &ctr, 1);

    if (out2) {
        uint8_t t2_in[33];
        memcpy(t2_in, out1, 32);
        t2_in[32] = 0x02;
        hmac_blake2s(out2, prk, 32, t2_in, 33);
    }

    memset(prk, 0, sizeof(prk));
}

/* ------------------------------------------------------------------ */
/* Internal Noise symmetric state                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  h[32];   /* handshake hash */
    uint8_t  ck[32];  /* chaining key */
    uint8_t  k[32];   /* current symmetric key (set by MixKey) */
    uint64_t n;       /* nonce for current key */
    bool     has_k;
} noise_state_t;

/* h = BLAKE2s(h || data) */
static void noise_mix_hash(noise_state_t *s, const uint8_t *data, size_t len)
{
    uint8_t buf[32 + 64]; /* h(32) + max_data(48) well within 96 */
    memcpy(buf, s->h, 32);
    memcpy(buf + 32, data, len);
    wireguard_blake2s(s->h, 32, NULL, 0, buf, 32 + len);
}

/* HKDF(ck, dh) → new_ck + new_k; reset nonce to 0 */
static void noise_mix_key(noise_state_t *s, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t new_ck[32], new_k[32];
    hkdf_blake2s(s->ck, ikm, ikm_len, new_ck, new_k);
    memcpy(s->ck, new_ck, 32);
    memcpy(s->k,  new_k,  32);
    s->n = 0;
    s->has_k = true;
}

/* AEAD-Encrypt(k, n, AD=h, plaintext) → ciphertext; then MixHash(ciphertext) */
static void noise_encrypt_and_hash(noise_state_t *s,
                                   const uint8_t *plaintext, size_t plen,
                                   uint8_t *ciphertext)
{
    static const uint8_t s_empty[1] = {0};
    wireguard_aead_encrypt(ciphertext,
                           plaintext ? plaintext : s_empty, plen,
                           s->h, 32, s->n++, s->k);
    noise_mix_hash(s, ciphertext, plen + 16);
}

/* AEAD-Decrypt(k, n, AD=h, ciphertext) → plaintext; then MixHash(ciphertext) */
static bool noise_decrypt_and_hash(noise_state_t *s,
                                   const uint8_t *ciphertext, size_t clen,
                                   uint8_t *plaintext)
{
    bool ok = wireguard_aead_decrypt(plaintext, ciphertext, clen,
                                     s->h, 32, s->n, s->k);
    if (ok) {
        s->n++;
        noise_mix_hash(s, ciphertext, clen);
    }
    return ok;
}

/* HKDF(ck, nil) → send_key + recv_key */
static void noise_split(const noise_state_t *s,
                        uint8_t send_key[32], uint8_t recv_key[32])
{
    hkdf_blake2s(s->ck, NULL, 0, send_key, recv_key);
}

/* h = ck = BLAKE2s("Noise_IK_25519_ChaChaPoly_BLAKE2s") */
static void noise_state_init(noise_state_t *s)
{
    wireguard_blake2s(s->h, 32, NULL, 0,
                      (const uint8_t *)NOISE_CONSTRUCTION,
                      sizeof(NOISE_CONSTRUCTION) - 1);
    memcpy(s->ck, s->h, 32);
    memset(s->k, 0, 32);
    s->n = 0;
    s->has_k = false;
}

/* ------------------------------------------------------------------ */
/* Public split API                                                     */
/* ------------------------------------------------------------------ */

esp_err_t ts_noise_build_init(const uint8_t machine_priv[32],
                               const uint8_t machine_pub[32],
                               const uint8_t srv_pub[32],
                               uint8_t       init_msg[TS_NOISE_INIT_MSG_LEN],
                               ts_noise_hs_t *hs)
{
    static const uint8_t basepoint[32] = { 9 };

    noise_state_t s;
    noise_state_init(&s);

    /* MixHash(prologue) */
    noise_mix_hash(&s, (const uint8_t *)NOISE_PROLOGUE,
                   sizeof(NOISE_PROLOGUE) - 1);

    /* MixHash(srv_pub) */
    noise_mix_hash(&s, srv_pub, 32);

    /* Generate ephemeral keypair */
    uint8_t eph_pub[32];
    wireguard_random_bytes(hs->eph_priv, 32);
    hs->eph_priv[0]  &= 248;
    hs->eph_priv[31] &= 127;
    hs->eph_priv[31] |= 64;
    wireguard_x25519(eph_pub, hs->eph_priv, basepoint);

    /* MixHash(eph_pub) */
    noise_mix_hash(&s, eph_pub, 32);

    /* MixKey(DH(e_priv_i, srv_pub)) */
    uint8_t dh_es[32];
    wireguard_x25519(dh_es, hs->eph_priv, srv_pub);
    noise_mix_key(&s, dh_es, 32);

    /* EncryptAndHash(machine_pub) → enc_static (48 bytes) */
    uint8_t enc_static[48];
    noise_encrypt_and_hash(&s, machine_pub, 32, enc_static);

    /* MixKey(DH(machine_priv, srv_pub)) */
    uint8_t dh_ss[32];
    wireguard_x25519(dh_ss, machine_priv, srv_pub);
    noise_mix_key(&s, dh_ss, 32);

    /* EncryptAndHash(nil) → enc_empty (16-byte AEAD tag) */
    uint8_t enc_empty[16];
    noise_encrypt_and_hash(&s, NULL, 0, enc_empty);

    /* Save state for ts_noise_finish */
    memcpy(hs->h,  s.h,  32);
    memcpy(hs->ck, s.ck, 32);

    /*
     * Assemble 101-byte init message:
     *   [0:2]   version BE = 1
     *   [2]     type = 0x01
     *   [3:5]   payload_len BE = 96  (0x0060)
     *   [5:37]  eph_pub (32)
     *   [37:85] enc_static (48)
     *   [85:101] enc_empty (16)
     */
    init_msg[0] = 0x00; init_msg[1] = 0x01;  /* version = 1 */
    init_msg[2] = 0x01;                        /* type: initiator */
    init_msg[3] = 0x00; init_msg[4] = 0x60;   /* payload_len = 96 */
    memcpy(init_msg + 5,       eph_pub,     32);
    memcpy(init_msg + 5 + 32,  enc_static,  48);
    memcpy(init_msg + 5 + 80,  enc_empty,   16);

    memset(dh_es, 0, sizeof(dh_es));
    memset(dh_ss, 0, sizeof(dh_ss));

    return ESP_OK;
}

esp_err_t ts_noise_finish(ts_noise_hs_t      *hs,
                           const uint8_t       machine_priv[32],
                           const uint8_t       resp_msg[TS_NOISE_RESP_MSG_LEN],
                           ts_noise_session_t *session)
{
    /*
     * Response layout (51 bytes):
     *   [0]     type = 0x02
     *   [1:3]   payload_len BE = 48
     *   [3:35]  resp_eph_pub (32)
     *   [35:51] enc_empty (16-byte AEAD tag)
     */
    if (resp_msg[0] != 0x02) {
        ESP_LOGE(TAG, "Unexpected Noise response type: 0x%02x", resp_msg[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t resp_plen = ((uint16_t)resp_msg[1] << 8) | resp_msg[2];
    if (resp_plen != 48) {
        ESP_LOGE(TAG, "Unexpected response payload_len: %u", resp_plen);
        return ESP_ERR_INVALID_RESPONSE;
    }

    noise_state_t s;
    memcpy(s.h,  hs->h,  32);
    memcpy(s.ck, hs->ck, 32);
    memset(s.k, 0, 32);
    s.n = 0;
    s.has_k = false;

    const uint8_t *resp_eph = resp_msg + 3;       /* [3:35]  */
    const uint8_t *resp_enc = resp_msg + 3 + 32;  /* [35:51] */

    /* MixHash(e_pub_r) */
    noise_mix_hash(&s, resp_eph, 32);

    /* MixKey(DH(e_priv_i, e_pub_r)) */
    uint8_t dh3[32];
    wireguard_x25519(dh3, hs->eph_priv, resp_eph);
    noise_mix_key(&s, dh3, 32);

    /* MixKey(DH(s_priv_i, e_pub_r)) */
    uint8_t dh4[32];
    wireguard_x25519(dh4, machine_priv, resp_eph);
    noise_mix_key(&s, dh4, 32);

    /* DecryptAndHash(enc_empty_r) — 16-byte tag over nil plaintext */
    uint8_t dummy[1];
    if (!noise_decrypt_and_hash(&s, resp_enc, 16, dummy)) {
        ESP_LOGE(TAG, "Noise response decryption failed — wrong server key?");
        memset(dh3, 0, sizeof(dh3));
        memset(dh4, 0, sizeof(dh4));
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Split(): HKDF(ck, nil) → send_key + recv_key */
    noise_split(&s, session->send_key, session->recv_key);
    session->send_nonce = 0;
    session->recv_nonce = 0;

    memset(dh3, 0, sizeof(dh3));
    memset(dh4, 0, sizeof(dh4));
    memset(hs->eph_priv, 0, sizeof(hs->eph_priv));

    ESP_LOGI(TAG, "Noise IK handshake complete");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Transport encryption / decryption (post-handshake)                  */
/* Frame: [4-byte LE ciphertext_len][ciphertext + 16-byte tag]         */
/* ------------------------------------------------------------------ */

/*
 * Tailscale controlbase uses big-endian nonce (bytes 4-11 of 12-byte nonce).
 * WireGuard chacha20poly1305_encrypt uses little-endian for the same field.
 * Byte-swapping the counter maps LE storage to the same byte sequence as BE.
 */
static inline uint64_t ts_be_nonce(uint64_t counter)
{
    return __builtin_bswap64(counter);
}

/*
 * ts_noise_encrypt: AEAD-encrypt plaintext → ciphertext (plen + 16 bytes).
 * The caller is responsible for framing ([type][2B len] header).
 */
esp_err_t ts_noise_encrypt(ts_noise_session_t *session,
                           const uint8_t      *plaintext, size_t plen,
                           uint8_t            *ciphertext)
{
    uint64_t n = session->send_nonce++;
    wireguard_aead_encrypt(ciphertext, plaintext, plen, NULL, 0,
                           ts_be_nonce(n), session->send_key);
    return ESP_OK;
}

/*
 * ts_noise_decrypt: AEAD-decrypt ciphertext (enc_len bytes) → plaintext.
 * enc_len must be >= 16 (the AEAD tag).
 */
esp_err_t ts_noise_decrypt(ts_noise_session_t *session,
                           const uint8_t      *ciphertext, size_t enc_len,
                           uint8_t            *plaintext,  size_t *plen)
{
    if (enc_len < 16) return ESP_ERR_INVALID_ARG;

    uint64_t n = session->recv_nonce++;

    if (!wireguard_aead_decrypt(plaintext, ciphertext, enc_len,
                                NULL, 0, ts_be_nonce(n), session->recv_key)) {
        ESP_LOGE(TAG, "Noise decrypt: auth failed (nonce=%llu)",
                 (unsigned long long)(n));
        session->recv_nonce--;
        return ESP_ERR_INVALID_CRC;
    }
    *plen = enc_len - 16;
    return ESP_OK;
}
