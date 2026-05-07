/*
 * Tailscale DISCO path-discovery protocol.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_disco.h"
#include "tailscale_keys.h"

#include "wireguard_esp32.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/ip_addr.h"

/* crypto primitives from wireguard component */
#include "crypto.h"
#include "wireguard-platform.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "ts_disco";

/* ------------------------------------------------------------------ */
/* DISCO packet layout (simplified — Tailscale spec):                  */
/*                                                                      */
/* All DISCO packets begin with:                                        */
/*   "TS💬" magic (6 bytes: 0x54 0x53 0xf0 0x9f 0x92 0xac)            */
/*   nonce (24 bytes)                                                   */
/*   box = XChaCha20-Poly1305(                                          */
/*            key = x25519(our_disco_priv, peer_disco_pub),             */
/*            plaintext = [1-byte type][payload]                        */
/*         )                                                            */
/* ------------------------------------------------------------------ */

#define DISCO_MAGIC_LEN   6
#define DISCO_NONCE_LEN   24
#define DISCO_OVERHEAD    (DISCO_MAGIC_LEN + DISCO_NONCE_LEN + 16)

static const uint8_t DISCO_MAGIC[DISCO_MAGIC_LEN] =
    { 0x54, 0x53, 0xf0, 0x9f, 0x92, 0xac };

/* ------------------------------------------------------------------ */
/* Ping payload: [8-byte tx ID][4-byte node_key truncated] = 12 bytes  */
/* (Actual Tailscale ping: only the 8-byte tx ID matters for matching) */
/* ------------------------------------------------------------------ */

#define DISCO_TASK_STACK  3072
#define DISCO_TASK_PRIO   4

typedef struct {
    uint8_t  peer_index;
    uint8_t  peer_disco_pub[32];
    ip_addr_t endpoint;
    uint16_t  port;
    uint8_t  tx_id[8];
} disco_peer_ctx_t;

#define DISCO_MAX_PENDING 8
static disco_peer_ctx_t s_pending[DISCO_MAX_PENDING];
static int              s_pending_count = 0;

static int              s_sock = -1;
static TaskHandle_t     s_task = NULL;
static bool             s_running = false;
static uint8_t          s_disco_priv[32];
static uint8_t          s_disco_pub[32];

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void build_disco_box(const uint8_t our_priv[32],
                            const uint8_t peer_pub[32],
                            const uint8_t *payload, size_t plen,
                            uint8_t *out,        /* DISCO_OVERHEAD + plen */
                            const uint8_t nonce[DISCO_NONCE_LEN])
{
    /* Shared secret via x25519 */
    uint8_t shared[32];
    wireguard_x25519(shared, our_priv, peer_pub);

    /* Box: XChaCha20-Poly1305(shared, nonce, payload) */
    wireguard_xaead_encrypt(out, payload, plen, NULL, 0, nonce, shared);
}

static bool open_disco_box(const uint8_t our_priv[32],
                           const uint8_t peer_pub[32],
                           const uint8_t *box, size_t box_len,
                           uint8_t *out,   /* box_len - 16 */
                           const uint8_t nonce[DISCO_NONCE_LEN])
{
    uint8_t shared[32];
    wireguard_x25519(shared, our_priv, peer_pub);
    return wireguard_xaead_decrypt(out, box, box_len, NULL, 0, nonce, shared);
}

/* ------------------------------------------------------------------ */
/* Receive task                                                         */
/* ------------------------------------------------------------------ */

static void disco_recv_task(void *arg)
{
    static uint8_t buf[512];

    while (s_running) {
        struct sockaddr_in src_addr = {0};
        socklen_t addr_len = sizeof(src_addr);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src_addr, &addr_len);
        if (n < 0) {
            if (!s_running) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Validate magic */
        if ((size_t)n < DISCO_MAGIC_LEN + DISCO_NONCE_LEN + 1 + 16) continue;
        if (memcmp(buf, DISCO_MAGIC, DISCO_MAGIC_LEN) != 0) continue;

        const uint8_t *nonce     = buf + DISCO_MAGIC_LEN;
        const uint8_t *box       = nonce + DISCO_NONCE_LEN;
        size_t         box_len   = (size_t)n - DISCO_MAGIC_LEN - DISCO_NONCE_LEN;

        /* We need the sender's DISCO public key to open the box.
         * In a full implementation, we would look up by src_addr in a peer
         * table populated from the netmap.  Here we try all pending peers. */
        for (int i = 0; i < s_pending_count; i++) {
            uint8_t plain[64] = {0};
            if (!open_disco_box(s_disco_priv, s_pending[i].peer_disco_pub,
                                box, box_len, plain, nonce)) {
                continue;
            }

            uint8_t msg_type = plain[0];
            if (msg_type == DISCO_MSG_PONG) {
                /* Pong received — upgrade WireGuard endpoint to direct path */
                ip_addr_t direct_ip;
                ip4_addr_t ip4;
                ip4.addr = src_addr.sin_addr.s_addr;
                ip_addr_copy_from_ip4(direct_ip, ip4);
                uint16_t direct_port = ntohs(src_addr.sin_port);

                ESP_LOGI(TAG, "DISCO Pong from peer %d — direct path established",
                         s_pending[i].peer_index);
                wireguard_esp32_update_endpoint(s_pending[i].peer_index,
                                               &direct_ip, direct_port);

                /* Remove from pending */
                s_pending[i] = s_pending[--s_pending_count];
            }
            break;
        }
    }

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t ts_disco_start(const uint8_t disco_priv[32],
                         const uint8_t disco_pub[32])
{
    memcpy(s_disco_priv, disco_priv, 32);
    memcpy(s_disco_pub,  disco_pub,  32);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "UDP socket creation failed");
        return ESP_FAIL;
    }

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = 0, /* ephemeral */
    };
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(s_sock);
        s_sock = -1;
        ESP_LOGE(TAG, "UDP bind failed");
        return ESP_FAIL;
    }

    s_running = true;
    xTaskCreate(disco_recv_task, "ts_disco_rx", DISCO_TASK_STACK,
                NULL, DISCO_TASK_PRIO, &s_task);

    ESP_LOGI(TAG, "DISCO started");
    return ESP_OK;
}

esp_err_t ts_disco_ping(uint8_t peer_index,
                        const uint8_t peer_disco_pub[32],
                        const ip_addr_t *endpoint,
                        uint16_t port)
{
    if (s_sock < 0 || ip_addr_isany(endpoint) || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pending_count >= DISCO_MAX_PENDING) {
        ESP_LOGW(TAG, "DISCO pending table full");
        return ESP_ERR_NO_MEM;
    }

    /* Generate random 8-byte tx ID */
    uint8_t tx_id[8];
    wireguard_random_bytes(tx_id, sizeof(tx_id));

    /* Build DISCO Ping payload: [type=0x01][8-byte tx_id] */
    uint8_t payload[9];
    payload[0] = DISCO_MSG_PING;
    memcpy(payload + 1, tx_id, 8);

    /* Generate random nonce */
    uint8_t nonce[DISCO_NONCE_LEN];
    wireguard_random_bytes(nonce, DISCO_NONCE_LEN);

    /* Encrypt */
    uint8_t ciphertext[9 + 16];
    build_disco_box(s_disco_priv, peer_disco_pub,
                    payload, sizeof(payload),
                    ciphertext, nonce);

    /* Assemble packet: magic + nonce + ciphertext */
    uint8_t pkt[DISCO_MAGIC_LEN + DISCO_NONCE_LEN + sizeof(ciphertext)];
    memcpy(pkt,                          DISCO_MAGIC, DISCO_MAGIC_LEN);
    memcpy(pkt + DISCO_MAGIC_LEN,        nonce,       DISCO_NONCE_LEN);
    memcpy(pkt + DISCO_MAGIC_LEN + DISCO_NONCE_LEN, ciphertext, sizeof(ciphertext));

    /* Send to peer endpoint */
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = ip_2_ip4(endpoint)->addr,
        .sin_port        = htons(port),
    };
    sendto(s_sock, pkt, sizeof(pkt), 0,
           (struct sockaddr *)&dst, sizeof(dst));

    /* Register in pending table */
    disco_peer_ctx_t *p = &s_pending[s_pending_count++];
    p->peer_index = peer_index;
    memcpy(p->peer_disco_pub, peer_disco_pub, 32);
    p->endpoint = *endpoint;
    p->port     = port;
    memcpy(p->tx_id, tx_id, 8);

    ESP_LOGD(TAG, "DISCO Ping sent to peer %d", peer_index);
    return ESP_OK;
}

void ts_disco_stop(void)
{
    s_running = false;
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}
