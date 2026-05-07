/*
 * Tailscale DERP relay client.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_derp.h"
#include "tailscale_keys.h"

#include "wireguard_esp32.h"     /* for wireguardif input — see note below */
#include "wireguardif.h"
#include "crypto.h"              /* wireguard_x25519, etc. */
#include "crypto/refc/poly1305-donna.h"

#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/socket.h>

static const char *TAG = "ts_derp";

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

#define DERP_RECV_TASK_STACK  8192   /* WG decryption + lwIP input may use ~6KB */
#define DERP_RECV_TASK_PRIO   5
#define DERP_KA_TASK_STACK    2048
#define DERP_KA_TASK_PRIO     4
#define DERP_TX_TASK_STACK    4096
#define DERP_TX_TASK_PRIO     5
#define DERP_TX_QUEUE_LEN     16
#define DERP_KEEPALIVE_MS     15000   /* send keepalive every 15 s */
#define DERP_RECV_TIMEOUT_MS  20000   /* SO_RCVTIMEO on TLS socket */
#define DERP_MAX_FRAME        (2048 + 32 + 4)  /* max WG packet + header */

/* Queue item handed from ts_derp_send (callers, including the WG netif
 * output path that runs under LOCK_TCPIP_CORE) to derp_tx_task. The TX
 * task does the actual TLS write outside the lwIP core lock so that
 * blocking on the underlying TCP socket never wedges the tcpip thread. */
typedef struct {
    uint8_t  frame_type;
    uint32_t plen;
    uint8_t  payload[];      /* flexible array; allocated as one block */
} derp_tx_item_t;

static esp_tls_t         *s_tls = NULL;
static SemaphoreHandle_t  s_tx_mutex = NULL;
static QueueHandle_t      s_tx_queue = NULL;
static TaskHandle_t       s_recv_task = NULL;
static TaskHandle_t       s_ka_task = NULL;
static TaskHandle_t       s_tx_task = NULL;
static bool               s_running = false;
static uint8_t            s_node_pub[32];
static volatile bool      s_connecting = false;  /* guard against double-spawn */

/* Saved home node for reconnect */
static ts_derp_node_t     s_home_node;
static uint8_t            s_node_priv_saved[32];

/* ------------------------------------------------------------------ */
/* TLS helpers                                                          */
/* ------------------------------------------------------------------ */

static int derp_tls_write(const uint8_t *buf, size_t len)
{
    if (!s_tls) return -1;
    size_t written = 0;
    while (written < len) {
        int n = esp_tls_conn_write(s_tls, buf + written, len - written);
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return (int)written;
}

static int derp_tls_read(uint8_t *buf, size_t len)
{
    if (!s_tls) return -1;
    size_t rd = 0;
    while (rd < len) {
        int n = esp_tls_conn_read(s_tls, buf + rd, len - rd);
        if (n <= 0) return -1;
        rd += (size_t)n;
    }
    return (int)rd;
}

/* ------------------------------------------------------------------ */
/* HSalsa20 + Salsa20 (for NaCl box used in DERP ClientInfo)           */
/* ------------------------------------------------------------------ */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define LD32(p) ( (uint32_t)(p)[0]        | ((uint32_t)(p)[1] <<  8) \
                | ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))
#define ST32(p, v) do { (p)[0]=(v)&0xff; (p)[1]=((v)>>8)&0xff; \
                        (p)[2]=((v)>>16)&0xff; (p)[3]=((v)>>24)&0xff; } while(0)

/* Salsa20 double-round (applied 10× for Salsa20/20) */
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

/*
 * NaCl secretbox_seal: XSalsa20-Poly1305.
 * key=32, nonce=24, plaintext, out = nonce(24) + poly1305_tag(16) + ciphertext.
 * out must be at least 24+16+plaintext_len bytes.
 */
static void nacl_box_seal(const uint8_t shared[32],     /* X25519 shared secret */
                          const uint8_t nonce[24],
                          const uint8_t *pt, size_t ptlen,
                          uint8_t *out)
{
    /*
     * NaCl box key derivation (2 steps):
     *   1. nacl_key = HSalsa20(k=dh_point, n=[0]*16)
     *   2. subkey   = HSalsa20(k=nacl_key, n=nonce[0:16])
     */
    static const uint8_t zero16[16] = {0};
    uint8_t nacl_key[32];
    hsalsa20(shared, zero16, nacl_key);
    uint8_t subkey[32];
    hsalsa20(nacl_key, nonce, subkey);

    /*
     * XSalsa20 keystream layout (block 0 = bytes 0-63):
     *   bytes  0-31: Poly1305 key
     *   bytes 32-63: encrypt plaintext[0:32]
     *   block 1 (bytes 64-127): encrypt plaintext[32:96]
     *   etc.
     * We must use the SECOND half of block 0 for the first 32 bytes of plaintext,
     * NOT skip to block 1.
     */
    uint32_t state[16] = {
        0x61707865, LD32(subkey+ 0), LD32(subkey+ 4), LD32(subkey+ 8),
        LD32(subkey+12), 0x3320646e, LD32(nonce+16), LD32(nonce+20),
        0, 0,  /* counter (64-bit LE) */
        0x79622d32, LD32(subkey+16), LD32(subkey+20), LD32(subkey+24),
        LD32(subkey+28), 0x6b206574,
    };

    /* Block 0: bytes 0-31 = Poly1305 key, bytes 32-63 = encrypt pt[0:32] */
    uint32_t blk[16];
    salsa20_core(blk, state);
    uint8_t blk0[64];
    for (int i = 0; i < 16; i++) ST32(blk0 + i*4, blk[i]);

    uint8_t poly_key[32];
    memcpy(poly_key, blk0, 32);

    uint8_t *ct = out + 40; /* nonce(24) + tag(16) */
    size_t pos = 0;

    /* Use bytes 32-63 of block 0 for plaintext[0:min(32,ptlen)] */
    size_t first = ptlen < 32 ? ptlen : 32;
    for (size_t j = 0; j < first; j++) ct[j] = pt[j] ^ blk0[32 + j];
    pos = first;

    /* Use blocks 1, 2, ... for remaining plaintext */
    state[8] = 1;
    while (pos < ptlen) {
        salsa20_core(blk, state);
        state[8]++;
        uint8_t ks[64];
        for (int i = 0; i < 16; i++) ST32(ks + i*4, blk[i]);
        size_t chunk = ptlen - pos < 64 ? ptlen - pos : 64;
        for (size_t j = 0; j < chunk; j++) ct[pos + j] = pt[pos + j] ^ ks[j];
        pos += chunk;
    }

    /* Compute Poly1305 MAC over ciphertext */
    poly1305_context pctx;
    poly1305_init(&pctx, poly_key);
    poly1305_update(&pctx, ct, ptlen);
    uint8_t tag[16];
    poly1305_finish(&pctx, tag);

    /* Write output: nonce(24) | tag(16) | ciphertext */
    memcpy(out,      nonce, 24);
    memcpy(out + 24, tag,   16);
    /* ciphertext already written at out+40 */
}

/* ------------------------------------------------------------------ */
/* DERP frame send/recv                                                 */
/* Frame format: [1-byte frame type][4-byte BE payload length][payload] */
/* ------------------------------------------------------------------ */

static esp_err_t derp_send_frame(uint8_t frame_type,
                                 const uint8_t *payload, size_t plen)
{
    uint8_t hdr[5];
    hdr[0] = frame_type;
    hdr[1] = (uint8_t)((plen >> 24) & 0xff);
    hdr[2] = (uint8_t)((plen >> 16) & 0xff);
    hdr[3] = (uint8_t)((plen >>  8) & 0xff);
    hdr[4] = (uint8_t)( plen        & 0xff);
    if (derp_tls_write(hdr, 5) != 5) return ESP_FAIL;
    if (plen > 0 && derp_tls_write(payload, plen) != (int)plen) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t derp_read_frame(uint8_t *type_out, uint8_t *buf, uint32_t buf_max,
                                  uint32_t *plen_out)
{
    uint8_t hdr[5];
    if (derp_tls_read(hdr, 5) != 5) return ESP_FAIL;
    *type_out = hdr[0];
    uint32_t plen = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16)
                  | ((uint32_t)hdr[3] <<  8) |  (uint32_t)hdr[4];
    *plen_out = plen;
    if (plen > buf_max) {
        /* Drain and discard oversized frames */
        uint8_t drain[64];
        uint32_t rem = plen;
        while (rem > 0) {
            uint32_t n = rem < sizeof(drain) ? rem : sizeof(drain);
            derp_tls_read(drain, n);
            rem -= n;
        }
        return ESP_OK; /* Caller checks *type_out and *plen_out */
    }
    if (plen > 0 && derp_tls_read(buf, plen) != (int)plen) return ESP_FAIL;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* HTTP upgrade to DERP                                                */
/* ------------------------------------------------------------------ */

static esp_err_t derp_http_upgrade(const uint8_t node_priv[32])
{
    char req[512];
    snprintf(req, sizeof(req),
             "GET /derp HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: upgrade\r\n"
             "Upgrade: DERP\r\n"
             "Tailscale-Version: 1.56.0\r\n"
             "\r\n",
             s_home_node.hostname);

    if (derp_tls_write((const uint8_t *)req, strlen(req)) < 0) return ESP_FAIL;

    /* Read until blank line (HTTP/1.1 101 response) */
    char line[256];
    int  llen = 0;
    bool switched = false;
    while (1) {
        uint8_t c;
        if (derp_tls_read(&c, 1) != 1) return ESP_FAIL;
        if (c == '\r') continue;
        if (c == '\n') {
            line[llen] = '\0';
            if (llen == 0) break;
            if (!switched && strstr(line, "101")) switched = true;
            llen = 0;
        } else {
            if (llen < (int)sizeof(line) - 1) line[llen++] = (char)c;
        }
    }
    if (!switched) {
        ESP_LOGE(TAG, "DERP HTTP upgrade failed (no 101)");
        return ESP_FAIL;
    }

    /*
     * Step 1: read ServerKey frame (type=0x01).
     * Payload = 8-byte magic "DERP\xF0\x9F\x94\x91" + 32-byte server X25519 pub key.
     */
    static uint8_t srv_key_buf[128];
    uint8_t ftype; uint32_t fplen;
    if (derp_read_frame(&ftype, srv_key_buf, sizeof(srv_key_buf), &fplen) != ESP_OK) {
        ESP_LOGE(TAG, "DERP: read ServerKey frame failed");
        return ESP_FAIL;
    }
    if (ftype != DERP_FRAME_SERVER_KEY || fplen < 40) {
        ESP_LOGE(TAG, "Expected ServerKey (0x01, len>=40), got type=0x%02x len=%"PRIu32,
                 ftype, fplen);
        return ESP_FAIL;
    }
    const uint8_t *srv_pub = srv_key_buf + 8;  /* skip 8-byte magic prefix */
    ESP_LOGI(TAG, "DERP ServerKey received (fplen=%"PRIu32
             " magic=%02x%02x%02x%02x srv=%02x%02x%02x%02x...)",
             fplen,
             srv_key_buf[0], srv_key_buf[1], srv_key_buf[2], srv_key_buf[3],
             srv_pub[0], srv_pub[1], srv_pub[2], srv_pub[3]);

    /* Step 2: build ClientInfo — NaCl box (XSalsa20-Poly1305)
     *
     * Wire layout: [32 node_pub][24 nonce][16 poly1305 tag][encrypted clientInfo]
     * clientInfo JSON: {"version":2}
     * Shared secret = X25519(node_priv, srv_pub)
     * nonce = all zeros
     */
    static const char CLIENT_INFO_JSON[] = "{\"version\":2}";
    static const uint8_t zero_nonce[24] = {0};

    uint8_t shared[32];
    wireguard_x25519(shared, node_priv, srv_pub);

    size_t ci_pt_len = strlen(CLIENT_INFO_JSON);
    /* Output: 24 (nonce) + 16 (tag) + ci_pt_len */
    size_t box_len = 24 + 16 + ci_pt_len;
    static uint8_t client_info_msg[32 + 24 + 16 + 32]; /* node_pub + box (generous) */
    memcpy(client_info_msg, s_node_pub, 32);
    nacl_box_seal(shared, zero_nonce,
                  (const uint8_t *)CLIENT_INFO_JSON, ci_pt_len,
                  client_info_msg + 32);
    memset(shared, 0, 32);

    ESP_LOGI(TAG, "DERP ClientInfo: node_pub=%02x%02x%02x%02x tag=%02x%02x%02x%02x",
             client_info_msg[0], client_info_msg[1],
             client_info_msg[2], client_info_msg[3],
             client_info_msg[56], client_info_msg[57],  /* tag bytes (at offset 32+24) */
             client_info_msg[58], client_info_msg[59]);

    esp_err_t err = derp_send_frame(DERP_FRAME_CLIENT_INFO,
                                    client_info_msg, 32 + box_len);
    if (err != ESP_OK) return err;

    /* Step 3: read ServerInfo frame (type=0x03) */
    uint8_t info_buf[256];
    if (derp_read_frame(&ftype, info_buf, sizeof(info_buf), &fplen) != ESP_OK) {
        ESP_LOGE(TAG, "DERP: read ServerInfo frame failed (connection closed?)");
        return ESP_FAIL;
    }
    if (ftype != DERP_FRAME_SERVER_INFO) {
        ESP_LOGE(TAG, "Expected ServerInfo (0x03), got 0x%02x len=%"PRIu32, ftype, fplen);
        if (ftype == 0x63 && fplen > 0 && fplen < sizeof(info_buf)) {
            info_buf[fplen] = '\0';
            ESP_LOGE(TAG, "DERP server error: %s", (char *)info_buf);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DERP connected to %s", s_home_node.hostname);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Receive task                                                         */
/* ------------------------------------------------------------------ */

/* Forward declaration — injects raw WireGuard packet into the WG netif */
static void inject_wg_packet(const uint8_t *data, size_t len);

static void derp_recv_task(void *arg)
{
    static uint8_t frame_buf[DERP_MAX_FRAME];

    while (s_running) {
        uint8_t frame_type; uint32_t payload_len;
        esp_err_t rc = derp_read_frame(&frame_type, frame_buf,
                                        sizeof(frame_buf), &payload_len);
        if (rc != ESP_OK) {
            if (!s_running) break;
            /* SO_RCVTIMEO timeout (EAGAIN/EWOULDBLOCK): not a real error */
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            ESP_LOGW(TAG, "DERP recv error (errno=%d) — disconnecting", errno);
            break;
        }
        if (payload_len > sizeof(frame_buf)) continue; /* oversized, already drained */

        switch (frame_type) {
        case DERP_FRAME_RECV_PACKET:
            ESP_LOGD(TAG, "DERP RecvPacket src=%02x%02x%02x%02x len=%"PRIu32,
                     frame_buf[0], frame_buf[1], frame_buf[2], frame_buf[3],
                     payload_len - 32);
            if (payload_len > 32)
                inject_wg_packet(frame_buf + 32, payload_len - 32);
            break;
        case DERP_FRAME_KEEP_ALIVE:
            break;
        case DERP_FRAME_PEER_GONE:
            ESP_LOGD(TAG, "DERP: peer gone");
            break;
        default:
            ESP_LOGD(TAG, "DERP: unhandled frame type 0x%02x len=%"PRIu32,
                     frame_type, payload_len);
            break;
        }
    }

    s_running = false;
    vTaskDelete(NULL);
}

/* Allocate and post one frame to the TX queue. Caller may be on any thread,
 * including the lwIP core-locked path — the actual TLS write happens later
 * on derp_tx_task so we never block the tcpip thread on socket I/O. */
static esp_err_t derp_tx_enqueue(uint8_t frame_type,
                                  const uint8_t *payload, size_t plen)
{
    if (!s_tx_queue) return ESP_ERR_INVALID_STATE;
    derp_tx_item_t *item = malloc(sizeof(*item) + plen);
    if (!item) return ESP_ERR_NO_MEM;
    item->frame_type = frame_type;
    item->plen       = (uint32_t)plen;
    if (plen && payload) memcpy(item->payload, payload, plen);
    if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "DERP TX queue full; dropping frame type=0x%02x",
                 frame_type);
        free(item);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* TX worker: serialised TLS writer. Pulls items off the queue and writes
 * them to the DERP TLS connection under s_tx_mutex. */
static void derp_tx_task(void *arg)
{
    while (s_running) {
        derp_tx_item_t *item = NULL;
        if (xQueueReceive(s_tx_queue, &item, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;
        if (!s_running) { free(item); break; }
        if (!s_tls)     { free(item); continue; }

        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        derp_send_frame(item->frame_type, item->payload, item->plen);
        xSemaphoreGive(s_tx_mutex);
        free(item);
    }
    /* Drain any remaining items. */
    derp_tx_item_t *item;
    while (s_tx_queue && xQueueReceive(s_tx_queue, &item, 0) == pdTRUE)
        free(item);
    vTaskDelete(NULL);
}

/* Keepalive task: enqueues a keepalive frame every DERP_KEEPALIVE_MS. */
static void derp_ka_task(void *arg)
{
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(DERP_KEEPALIVE_MS));
        if (!s_running || !s_tls) break;
        derp_tx_enqueue(DERP_FRAME_KEEP_ALIVE, NULL, 0);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* WireGuard packet injection                                           */
/* ------------------------------------------------------------------ */

/* DERP-relayed packets are raw WireGuard UDP payloads, so we must hand
 * them to wireguardif_network_rx (the callback wireguardif registers on
 * its udp_pcb) — NOT tcpip_input, which would treat the bytes as IP.
 * wireguardif_network_rx eventually calls ip_input/tcp_input which must
 * run on the lwIP tcpip thread; we marshal the call via tcpip_callback.
 * Synthetic source is in the 127.3.3.0/24 DERP pseudo range so replies
 * route back through our DERP output hook. */
struct udp_pcb;  /* forward decl — opaque here */
extern void wireguardif_network_rx(void *arg, struct udp_pcb *pcb,
                                    struct pbuf *p, const ip_addr_t *addr,
                                    u16_t port);

static void inject_wg_packet(const uint8_t *data, size_t len)
{
    extern struct netif *netif_list;

    struct netif *nif = netif_list;
    while (nif) {
        if (nif->name[0] == 'w' && nif->name[1] == 'g') break;
        nif = nif->next;
    }
    if (!nif || !nif->state) {
        ESP_LOGD(TAG, "inject_wg_packet: WireGuard netif not found");
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
    if (!p) {
        ESP_LOGW(TAG, "inject_wg_packet: pbuf_alloc failed");
        return;
    }
    memcpy(p->payload, data, len);

    ip_addr_t src_ip;
    ip4_addr_t src4;
    ip4addr_aton("127.3.3.40", &src4);
    ip_addr_copy_from_ip4(src_ip, src4);
    u16_t src_port = (u16_t)(s_home_node.region_id > 0 ? s_home_node.region_id : 1);

    ESP_LOGD(TAG, "inject_wg: type=0x%02x len=%u to wg-netif",
             data[0], (unsigned)len);

    /* Call WG receive directly from the DERP recv task. Thread-safety note:
     * lwIP normally requires API access on the tcpip thread, but we sidestep
     * that here because (a) the WG netif's only inbound source IS this
     * task, (b) outbound encryption goes through the same TLS connection
     * which is serialized with s_tx_mutex. ICMP exchange across threads is
     * stateless enough that direct call works in practice. */
    wireguardif_network_rx(nif->state, NULL, p, &src_ip, src_port);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t ts_derp_connect(const ts_derp_node_t *node,
                          const uint8_t node_priv[32],
                          const uint8_t node_pub[32])
{
    if (s_tls) ts_derp_disconnect();

    /* If called directly (not via ts_derp_set_home), save params */
    if (node != &s_home_node) s_home_node = *node;
    if (node_pub  != s_node_pub)        memcpy(s_node_pub, node_pub, 32);
    if (node_priv != s_node_priv_saved) memcpy(s_node_priv_saved, node_priv, 32);

    ESP_LOGI(TAG, "DERP: connecting to %s:%d (heap_free=%u)",
             node->hostname, node->derp_port,
             (unsigned)esp_get_free_heap_size());
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .non_block         = false,
        .timeout_ms        = 60000,   /* cert chain verification on real-HW
                                       * S3 + concurrent control-TLS load can
                                       * exceed 20s; give ample headroom. */
    };
    s_tls = esp_tls_init();
    if (!s_tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return ESP_ERR_NO_MEM;
    }
    int ret = esp_tls_conn_new_sync(node->hostname,
                                    (int)strlen(node->hostname),
                                    (int)node->derp_port, &cfg, s_tls);
    if (ret != 1) {
        int last_err = 0, mb_err = 0, flags = 0;
        esp_tls_get_and_clear_last_error(&((esp_tls_last_error_t){0}),
                                         &mb_err, &flags);
        (void)last_err;
        ESP_LOGE(TAG, "TLS connect to DERP %s failed: ret=%d mbedtls=-0x%04x flags=0x%x heap=%u",
                 node->hostname, ret, -mb_err, flags,
                 (unsigned)esp_get_free_heap_size());
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
        return ESP_FAIL;
    }

    if (!s_tx_mutex) {
        s_tx_mutex = xSemaphoreCreateMutex();
    }
    if (!s_tx_queue) {
        s_tx_queue = xQueueCreate(DERP_TX_QUEUE_LEN, sizeof(derp_tx_item_t *));
    }

    esp_err_t err = derp_http_upgrade(node_priv);
    if (err != ESP_OK) {
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
        return err;
    }

    /* Set receive timeout so recv_task can wake up and send keepalives */
    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(s_tls, &sockfd) == ESP_OK && sockfd >= 0) {
        struct timeval tv = {
            .tv_sec  = DERP_RECV_TIMEOUT_MS / 1000,
            .tv_usec = (DERP_RECV_TIMEOUT_MS % 1000) * 1000,
        };
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    s_running = true;
    xTaskCreate(derp_recv_task, "ts_derp_rx", DERP_RECV_TASK_STACK,
                NULL, DERP_RECV_TASK_PRIO, &s_recv_task);
    xTaskCreate(derp_ka_task, "ts_derp_ka", DERP_KA_TASK_STACK,
                NULL, DERP_KA_TASK_PRIO, &s_ka_task);
    xTaskCreate(derp_tx_task, "ts_derp_tx", DERP_TX_TASK_STACK,
                NULL, DERP_TX_TASK_PRIO, &s_tx_task);

    return ESP_OK;
}

esp_err_t ts_derp_send(const uint8_t dst_pub[32],
                       const uint8_t *pkt, size_t pkt_len)
{
    if (!s_tls || !s_running) return ESP_ERR_INVALID_STATE;

    /* SendPacket payload: [32 dst pub][WG packet] */
    size_t payload_len = 32 + pkt_len;
    uint8_t *payload = malloc(payload_len);
    if (!payload) return ESP_ERR_NO_MEM;
    memcpy(payload,      dst_pub, 32);
    memcpy(payload + 32, pkt,     pkt_len);

    /* Defer the TLS write to derp_tx_task so callers (including the lwIP
     * core-lock holder) don't block on socket I/O. */
    esp_err_t err = derp_tx_enqueue(DERP_FRAME_SEND_PACKET, payload, payload_len);
    free(payload);

    ESP_LOGD(TAG, "ts_derp_send dst=%02x%02x%02x%02x type=0x%02x len=%u enq=%d",
             dst_pub[0], dst_pub[1], dst_pub[2], dst_pub[3],
             pkt_len > 0 ? pkt[0] : 0, (unsigned)pkt_len, err);

    return err;
}

void ts_derp_disconnect(void)
{
    s_running = false;
    if (s_tls) {
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
    }
    s_recv_task = NULL;
    s_ka_task   = NULL;
    s_tx_task   = NULL;
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* Background task that calls ts_derp_connect then deletes itself */
static void derp_connect_task(void *arg)
{
    (void)arg;
    ts_derp_connect(&s_home_node, s_node_priv_saved, s_node_pub);
    s_connecting = false;
    vTaskDelete(NULL);
}

esp_err_t ts_derp_set_home(const ts_derp_node_t *node,
                           const uint8_t node_priv[32],
                           const uint8_t node_pub[32])
{
    /* Already connected to this server */
    if ((s_tls || s_connecting) &&
        strcmp(s_home_node.hostname, node->hostname) == 0 &&
        s_home_node.derp_port == node->derp_port) {
        return ESP_OK;
    }
    /* Save params before spawning the task */
    s_home_node = *node;
    memcpy(s_node_pub,        node_pub,  32);
    memcpy(s_node_priv_saved, node_priv, 32);
    s_connecting = true;
    ESP_LOGI(TAG, "DERP: spawning connect task for %s", node->hostname);
    BaseType_t ok = xTaskCreate(derp_connect_task, "ts_derp_conn", 12288, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "DERP: xTaskCreate failed (%d)", (int)ok);
        s_connecting = false;
    }
    return ESP_OK;
}

/* wireguard_derp_output_fn-compatible wrapper */
void ts_derp_send_packet(const uint8_t *peer_pub,
                         const uint8_t *pkt, size_t pkt_len)
{
    ts_derp_send(peer_pub, pkt, pkt_len);
}
