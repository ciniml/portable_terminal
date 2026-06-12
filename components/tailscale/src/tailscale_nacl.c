/*
 * NaCl box (XSalsa20-Poly1305) seal + open.
 *
 * Lifted from the inline implementation that used to live in
 * tailscale_derp.c so that DISCO can reuse it. The seal path is the
 * exact same algorithm; open is the inverse with a constant-time
 * Poly1305 tag verification.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_nacl.h"

#include "crypto/refc/poly1305-donna.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Salsa20 / HSalsa20 primitives                                        */
/* ------------------------------------------------------------------ */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define LD32(p) ( (uint32_t)(p)[0]        | ((uint32_t)(p)[1] <<  8) \
                | ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))
#define ST32(p, v) do { (p)[0]=(v)&0xff; (p)[1]=((v)>>8)&0xff; \
                        (p)[2]=((v)>>16)&0xff; (p)[3]=((v)>>24)&0xff; } while(0)

#define SALSA_QR(a,b,c,d) \
    b ^= ROTL32(a+d, 7);  c ^= ROTL32(b+a, 9); \
    d ^= ROTL32(c+b,13);  a ^= ROTL32(d+c,18);

static void salsa20_core(uint32_t out[16], const uint32_t in[16])
{
    uint32_t x[16];
    memcpy(x, in, 64);
    for (int i = 0; i < 10; i++) {
        SALSA_QR(x[ 0], x[ 4], x[ 8], x[12]);
        SALSA_QR(x[ 5], x[ 9], x[13], x[ 1]);
        SALSA_QR(x[10], x[14], x[ 2], x[ 6]);
        SALSA_QR(x[15], x[ 3], x[ 7], x[11]);
        SALSA_QR(x[ 0], x[ 1], x[ 2], x[ 3]);
        SALSA_QR(x[ 5], x[ 6], x[ 7], x[ 4]);
        SALSA_QR(x[10], x[11], x[ 8], x[ 9]);
        SALSA_QR(x[15], x[12], x[13], x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

/* HSalsa20: key(32) + nonce_prefix(16) → subkey(32). No addition of input. */
static void hsalsa20(const uint8_t key[32], const uint8_t n[16], uint8_t out[32])
{
    uint32_t x[16] = {
        0x61707865, LD32(key+ 0), LD32(key+ 4), LD32(key+ 8),
        LD32(key+12), 0x3320646e, LD32(n+ 0), LD32(n+ 4),
        LD32(n+ 8), LD32(n+12), 0x79622d32, LD32(key+16),
        LD32(key+20), LD32(key+24), LD32(key+28), 0x6b206574,
    };
    uint32_t z[16];
    memcpy(z, x, 64);
    for (int i = 0; i < 10; i++) {
        SALSA_QR(z[ 0], z[ 4], z[ 8], z[12]);
        SALSA_QR(z[ 5], z[ 9], z[13], z[ 1]);
        SALSA_QR(z[10], z[14], z[ 2], z[ 6]);
        SALSA_QR(z[15], z[ 3], z[ 7], z[11]);
        SALSA_QR(z[ 0], z[ 1], z[ 2], z[ 3]);
        SALSA_QR(z[ 5], z[ 6], z[ 7], z[ 4]);
        SALSA_QR(z[10], z[11], z[ 8], z[ 9]);
        SALSA_QR(z[15], z[12], z[13], z[14]);
    }
    ST32(out+ 0, z[ 0]); ST32(out+ 4, z[ 5]);
    ST32(out+ 8, z[10]); ST32(out+12, z[15]);
    ST32(out+16, z[ 6]); ST32(out+20, z[ 7]);
    ST32(out+24, z[ 8]); ST32(out+28, z[ 9]);
}

/* ------------------------------------------------------------------ */
/* Key derivation                                                       */
/* ------------------------------------------------------------------ */

/* NaCl box: shared X25519 secret → HSalsa20 → 32-byte symmetric key.
 *           Then XSalsa20 uses (key, nonce[0:16]) to derive a per-message
 *           subkey via a second HSalsa20, and runs Salsa20 with nonce[16:24]. */
static void derive_subkey(const uint8_t shared[32],
                          const uint8_t nonce[24],
                          uint8_t subkey[32])
{
    static const uint8_t zero16[16] = {0};
    uint8_t nacl_key[32];
    hsalsa20(shared, zero16, nacl_key);
    hsalsa20(nacl_key, nonce, subkey);
}

/* Build the initial XSalsa20 state matrix. */
static void xsalsa20_init_state(uint32_t state[16],
                                const uint8_t subkey[32],
                                const uint8_t nonce[24])
{
    state[ 0] = 0x61707865;
    state[ 1] = LD32(subkey+ 0);
    state[ 2] = LD32(subkey+ 4);
    state[ 3] = LD32(subkey+ 8);
    state[ 4] = LD32(subkey+12);
    state[ 5] = 0x3320646e;
    state[ 6] = LD32(nonce+16);
    state[ 7] = LD32(nonce+20);
    state[ 8] = 0;        /* counter low */
    state[ 9] = 0;        /* counter high */
    state[10] = 0x79622d32;
    state[11] = LD32(subkey+16);
    state[12] = LD32(subkey+20);
    state[13] = LD32(subkey+24);
    state[14] = LD32(subkey+28);
    state[15] = 0x6b206574;
}

/* Run keystream block, write 64 bytes of keystream into `out`. */
static void xsalsa20_block(uint32_t state[16], uint8_t out[64])
{
    uint32_t blk[16];
    salsa20_core(blk, state);
    for (int i = 0; i < 16; i++) ST32(out + i*4, blk[i]);
}

/* Constant-time 16-byte compare. */
static bool ct_eq16(const uint8_t a[16], const uint8_t b[16])
{
    uint8_t d = 0;
    for (int i = 0; i < 16; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void ts_nacl_box_seal(const uint8_t shared[32],
                      const uint8_t nonce[24],
                      const uint8_t *pt, size_t ptlen,
                      uint8_t *out)
{
    uint8_t subkey[32];
    derive_subkey(shared, nonce, subkey);

    uint32_t state[16];
    xsalsa20_init_state(state, subkey, nonce);

    /* Block 0: bytes 0..31 = Poly1305 key, bytes 32..63 = encrypt pt[0..32). */
    uint8_t blk0[64];
    xsalsa20_block(state, blk0);

    uint8_t poly_key[32];
    memcpy(poly_key, blk0, 32);

    uint8_t *tag = out;
    uint8_t *ct  = out + 16;

    size_t pos = 0;
    size_t first = ptlen < 32 ? ptlen : 32;
    for (size_t j = 0; j < first; j++) ct[j] = pt[j] ^ blk0[32 + j];
    pos = first;

    state[8] = 1;
    while (pos < ptlen) {
        uint8_t ks[64];
        xsalsa20_block(state, ks);
        state[8]++;
        size_t chunk = (ptlen - pos < 64) ? (ptlen - pos) : 64;
        for (size_t j = 0; j < chunk; j++) ct[pos + j] = pt[pos + j] ^ ks[j];
        pos += chunk;
    }

    /* MAC over ciphertext only. */
    poly1305_context pctx;
    poly1305_init(&pctx, poly_key);
    poly1305_update(&pctx, ct, ptlen);
    poly1305_finish(&pctx, tag);
}

bool ts_nacl_box_open(const uint8_t shared[32],
                      const uint8_t nonce[24],
                      const uint8_t *in, size_t inlen,
                      uint8_t *out_pt)
{
    if (inlen < TS_NACL_BOX_OVERHEAD) return false;

    uint8_t subkey[32];
    derive_subkey(shared, nonce, subkey);

    uint32_t state[16];
    xsalsa20_init_state(state, subkey, nonce);

    uint8_t blk0[64];
    xsalsa20_block(state, blk0);

    uint8_t poly_key[32];
    memcpy(poly_key, blk0, 32);

    const uint8_t *recv_tag = in;
    const uint8_t *ct       = in + 16;
    size_t         ctlen    = inlen - 16;

    /* Verify Poly1305 tag against ciphertext (constant time). */
    poly1305_context pctx;
    poly1305_init(&pctx, poly_key);
    poly1305_update(&pctx, ct, ctlen);
    uint8_t calc_tag[16];
    poly1305_finish(&pctx, calc_tag);
    if (!ct_eq16(calc_tag, recv_tag)) return false;

    /* Decrypt — same XSalsa20 keystream as seal (block 0 second half + blocks 1+). */
    size_t pos = 0;
    size_t first = ctlen < 32 ? ctlen : 32;
    for (size_t j = 0; j < first; j++) out_pt[j] = ct[j] ^ blk0[32 + j];
    pos = first;

    state[8] = 1;
    while (pos < ctlen) {
        uint8_t ks[64];
        xsalsa20_block(state, ks);
        state[8]++;
        size_t chunk = (ctlen - pos < 64) ? (ctlen - pos) : 64;
        for (size_t j = 0; j < chunk; j++) out_pt[pos + j] = ct[pos + j] ^ ks[j];
        pos += chunk;
    }
    return true;
}
