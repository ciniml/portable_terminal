/*
 * WireGuard implementation for ESP32 Arduino by Kenta Ida (fuga@fugafuga.org)
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "wireguard-platform.h"

#include <stdlib.h>
#include <string.h>
#include "crypto.h"
#include "crypto/refc/blake2s.h"   /* in-tree BLAKE2s — no external deps */
#include "lwip/sys.h"
#include "esp_random.h"            /* esp_fill_random — hardware-backed PRNG */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* --------------------------------------------------------------------------
 * BLAKE2s-based DRBG
 *
 * The previous implementation used mbedTLS CTR_DRBG seeded by esp_fill_random.
 * mbedtls/entropy.h is no longer reliably exposed by the standard mbedtls
 * component in newer ESP-IDF builds, breaking compilation in CI. To avoid
 * the dependency we keep an equivalent in-tree DRBG using BLAKE2s, which
 * the WireGuard implementation already bundles for protocol use.
 *
 * Calling esp_fill_random() directly per request is correct but slow (each
 * call hits the hardware RNG register with mandatory inter-sample delays).
 * WireGuard requests random bytes during handshake setup, key generation,
 * cookie rotation, etc. — infrequent on average but bursty. The DRBG
 * smooths that and reseeds every reseed_interval bytes.
 *
 * Construction (per RFC 8439-style hash-based PRG):
 *   key  = 32-byte secret state
 *   ctr  = 64-bit counter
 *   out  = BLAKE2s(key, ctr || "WG_DRBG")  per 32-byte block
 *
 * Reseed: every RESEED_INTERVAL output bytes, mix 32 fresh bytes from
 * esp_fill_random into the key via BLAKE2s.
 * -------------------------------------------------------------------------- */

#define DRBG_KEY_LEN        32
#define DRBG_BLOCK_LEN      32
#define DRBG_RESEED_BYTES   (1u << 20)   /* 1 MiB between reseeds — generous,
                                          * actual WG demand is < 1 KiB / day */

static struct {
    uint8_t           key[DRBG_KEY_LEN];
    uint64_t          counter;
    uint32_t          bytes_since_reseed;
    bool              initialized;
    SemaphoreHandle_t lock;
} s_drbg;

static const uint8_t DRBG_PERSONALIZATION[7] = { 'W','G','_','D','R','B','G' };

/* Mix 32 bytes of hardware entropy into the DRBG key. Called both during
 * init and periodically while the DRBG is in use. */
static void drbg_reseed_locked(void) {
    uint8_t fresh[32];
    esp_fill_random(fresh, sizeof(fresh));
    blake2s_ctx ctx;
    blake2s_init(&ctx, DRBG_KEY_LEN, NULL, 0);
    blake2s_update(&ctx, s_drbg.key, sizeof(s_drbg.key));
    blake2s_update(&ctx, fresh, sizeof(fresh));
    blake2s_final(&ctx, s_drbg.key);
    s_drbg.bytes_since_reseed = 0;
}

void wireguard_platform_init(void) {
    if (s_drbg.initialized) return;
    s_drbg.lock = xSemaphoreCreateMutex();
    /* Seed the key with 32 bytes of hardware entropy and a personalization
     * string so independent instances do not start from the same state. */
    uint8_t fresh[32];
    esp_fill_random(fresh, sizeof(fresh));
    blake2s(s_drbg.key, DRBG_KEY_LEN, fresh, sizeof(fresh),
            DRBG_PERSONALIZATION, sizeof(DRBG_PERSONALIZATION));
    s_drbg.counter = 0;
    s_drbg.bytes_since_reseed = 0;
    s_drbg.initialized = true;
}

void wireguard_random_bytes(void *bytes, size_t size) {
    if (!s_drbg.initialized) wireguard_platform_init();

    xSemaphoreTake(s_drbg.lock, portMAX_DELAY);
    uint8_t *out = (uint8_t *)bytes;
    while (size > 0) {
        if (s_drbg.bytes_since_reseed >= DRBG_RESEED_BYTES) {
            drbg_reseed_locked();
        }

        /* Generate one block: BLAKE2s(key=key, in=counter || personalization). */
        uint8_t block[DRBG_BLOCK_LEN];
        uint8_t in[8 + sizeof(DRBG_PERSONALIZATION)];
        for (int i = 0; i < 8; i++) in[i] = (uint8_t)(s_drbg.counter >> (8 * i));
        memcpy(in + 8, DRBG_PERSONALIZATION, sizeof(DRBG_PERSONALIZATION));
        blake2s(block, DRBG_BLOCK_LEN, s_drbg.key, DRBG_KEY_LEN,
                in, sizeof(in));
        s_drbg.counter++;

        size_t take = size < DRBG_BLOCK_LEN ? size : DRBG_BLOCK_LEN;
        memcpy(out, block, take);
        out  += take;
        size -= take;
        s_drbg.bytes_since_reseed += take;
    }
    xSemaphoreGive(s_drbg.lock);
}

uint32_t wireguard_sys_now() {
	// Default to the LwIP system time
	return sys_now();
}

void wireguard_tai64n_now(uint8_t *output) {
	// See https://cr.yp.to/libtai/tai64.html
	// 64 bit seconds from 1970 = 8 bytes
	// 32 bit nano seconds from current second

	// Get timestamp. Note that the timestamp must be synced by NTP, 
	//  or at least preserved in NVS, not to go back after reset.
	// Otherwise, the WireGuard remote peer rejects handshake.
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint64_t millis = (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));

	// Split into seconds offset + nanos
	uint64_t seconds = 0x400000000000000aULL + (millis / 1000);
	uint32_t nanos = (millis % 1000) * 1000;
	U64TO8_BIG(output + 0, seconds);
	U32TO8_BIG(output + 8, nanos);
}

bool wireguard_is_under_load() {
	return false;
}

