/*
 * Tailscale DERP relay client.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_derp.h"
#include "tailscale_keys.h"
#include "tailscale_nacl.h"      /* shared XSalsa20-Poly1305 box (with DISCO) */

#include "wireguard_esp32.h"     /* for wireguardif input — see note below */
#include "wireguardif.h"
#include "crypto.h"              /* wireguard_x25519, etc. */

#include "esp_attr.h"
#include "esp_heap_caps.h"
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

/* One DERP relay connection. The home region carries inbound traffic
 * (peers reach us via our home region); additional regions are opened
 * on demand so we can *send* to peers homed on other regions, since
 * DERP does not forward packets across regions. */
typedef struct {
    bool              in_use;
    int               region_id;
    esp_tls_t        *tls;
    SemaphoreHandle_t tx_mutex;
    QueueHandle_t     tx_queue;
    TaskHandle_t      recv_task;
    TaskHandle_t      ka_task;
    TaskHandle_t      tx_task;
    volatile bool     running;
    volatile bool     connecting;
    ts_derp_node_t    node;
} derp_conn_t;

#define DERP_MAX_CONNS 4
static derp_conn_t s_conns[DERP_MAX_CONNS];
static SemaphoreHandle_t s_pool_mutex = NULL;

/* Shared identity (same for every region connection). */
static uint8_t            s_node_pub[32];
static uint8_t            s_node_priv_saved[32];
static int                s_home_region = 0;

/* Look up the resolver in netmap for a region's node descriptor. */
extern bool ts_netmap_get_derp_region(int region_id, ts_derp_node_t *out);

/* ------------------------------------------------------------------ */
/* TLS helpers (operate on one connection)                              */
/* ------------------------------------------------------------------ */

static int derp_tls_write(derp_conn_t *c, const uint8_t *buf, size_t len)
{
    if (!c || !c->tls) return -1;
    size_t written = 0;
    while (written < len) {
        int n = esp_tls_conn_write(c->tls, buf + written, len - written);
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return (int)written;
}

static int derp_tls_read(derp_conn_t *c, uint8_t *buf, size_t len)
{
    if (!c || !c->tls) return -1;
    size_t rd = 0;
    while (rd < len) {
        int n = esp_tls_conn_read(c->tls, buf + rd, len - rd);
        if (n <= 0) return -1;
        rd += (size_t)n;
    }
    return (int)rd;
}

/* ------------------------------------------------------------------ */
/* DERP ClientInfo wire layout: nonce(24) || tag(16) || ct(plen).      */
/* The actual NaCl box (XSalsa20-Poly1305) lives in tailscale_nacl.c   */
/* — that helper produces tag(16)||ct, and we just prepend the nonce. */
/* ------------------------------------------------------------------ */
static void nacl_box_seal(const uint8_t shared[32],
                          const uint8_t nonce[24],
                          const uint8_t *pt, size_t ptlen,
                          uint8_t *out)
{
    memcpy(out, nonce, 24);
    ts_nacl_box_seal(shared, nonce, pt, ptlen, out + 24);
    /* out layout now = nonce(24) || tag(16) || ct(ptlen) */
}

/* ------------------------------------------------------------------ */
/* DERP frame send/recv                                                 */
/* Frame format: [1-byte frame type][4-byte BE payload length][payload] */
/* ------------------------------------------------------------------ */

static esp_err_t derp_send_frame(derp_conn_t *c, uint8_t frame_type,
                                 const uint8_t *payload, size_t plen)
{
    uint8_t hdr[5];
    hdr[0] = frame_type;
    hdr[1] = (uint8_t)((plen >> 24) & 0xff);
    hdr[2] = (uint8_t)((plen >> 16) & 0xff);
    hdr[3] = (uint8_t)((plen >>  8) & 0xff);
    hdr[4] = (uint8_t)( plen        & 0xff);
    if (derp_tls_write(c, hdr, 5) != 5) return ESP_FAIL;
    if (plen > 0 && derp_tls_write(c, payload, plen) != (int)plen) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t derp_read_frame(derp_conn_t *c, uint8_t *type_out,
                                 uint8_t *buf, uint32_t buf_max,
                                 uint32_t *plen_out)
{
    uint8_t hdr[5];
    if (derp_tls_read(c, hdr, 5) != 5) return ESP_FAIL;
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
            derp_tls_read(c, drain, n);
            rem -= n;
        }
        return ESP_OK; /* Caller checks *type_out and *plen_out */
    }
    if (plen > 0 && derp_tls_read(c, buf, plen) != (int)plen) return ESP_FAIL;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* HTTP upgrade to DERP                                                */
/* ------------------------------------------------------------------ */

static esp_err_t derp_http_upgrade(derp_conn_t *c, const uint8_t node_priv[32])
{
    char req[512];
    snprintf(req, sizeof(req),
             "GET /derp HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: upgrade\r\n"
             "Upgrade: DERP\r\n"
             "Tailscale-Version: 1.56.0\r\n"
             "\r\n",
             c->node.hostname);

    if (derp_tls_write(c, (const uint8_t *)req, strlen(req)) < 0) return ESP_FAIL;

    /* Read until blank line (HTTP/1.1 101 response) */
    char line[256];
    int  llen = 0;
    bool switched = false;
    while (1) {
        uint8_t ch;
        if (derp_tls_read(c, &ch, 1) != 1) return ESP_FAIL;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[llen] = '\0';
            if (llen == 0) break;
            if (!switched && strstr(line, "101")) switched = true;
            llen = 0;
        } else {
            if (llen < (int)sizeof(line) - 1) line[llen++] = (char)ch;
        }
    }
    if (!switched) {
        ESP_LOGE(TAG, "DERP HTTP upgrade failed (no 101)");
        return ESP_FAIL;
    }

    /*
     * Step 1: read ServerKey frame (type=0x01).
     * Payload = 8-byte magic "DERP\xF0\x9F\x94\x91" + 32-byte server X25519 pub key.
     * Scratch buffers are stack-local (not static) so concurrent connect
     * tasks for different regions don't clobber each other.
     */
    uint8_t srv_key_buf[128];
    uint8_t ftype; uint32_t fplen;
    if (derp_read_frame(c, &ftype, srv_key_buf, sizeof(srv_key_buf), &fplen) != ESP_OK) {
        ESP_LOGE(TAG, "DERP: read ServerKey frame failed");
        return ESP_FAIL;
    }
    if (ftype != DERP_FRAME_SERVER_KEY || fplen < 40) {
        ESP_LOGE(TAG, "Expected ServerKey (0x01, len>=40), got type=0x%02x len=%"PRIu32,
                 ftype, fplen);
        return ESP_FAIL;
    }
    const uint8_t *srv_pub = srv_key_buf + 8;  /* skip 8-byte magic prefix */
    ESP_LOGD(TAG, "DERP ServerKey received (fplen=%"PRIu32
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
    uint8_t client_info_msg[32 + 24 + 16 + 32]; /* node_pub + box (generous) */
    memcpy(client_info_msg, s_node_pub, 32);
    nacl_box_seal(shared, zero_nonce,
                  (const uint8_t *)CLIENT_INFO_JSON, ci_pt_len,
                  client_info_msg + 32);
    memset(shared, 0, 32);

    ESP_LOGD(TAG, "DERP ClientInfo: node_pub=%02x%02x%02x%02x tag=%02x%02x%02x%02x",
             client_info_msg[0], client_info_msg[1],
             client_info_msg[2], client_info_msg[3],
             client_info_msg[56], client_info_msg[57],  /* tag bytes (at offset 32+24) */
             client_info_msg[58], client_info_msg[59]);

    esp_err_t err = derp_send_frame(c, DERP_FRAME_CLIENT_INFO,
                                    client_info_msg, 32 + box_len);
    if (err != ESP_OK) return err;

    /* Step 3: read ServerInfo frame (type=0x03) */
    uint8_t info_buf[256];
    if (derp_read_frame(c, &ftype, info_buf, sizeof(info_buf), &fplen) != ESP_OK) {
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

    ESP_LOGI(TAG, "DERP connected to %s (region %d)",
             c->node.hostname, c->region_id);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Receive task                                                         */
/* ------------------------------------------------------------------ */

/* Forward declaration — injects raw WireGuard packet into the WG netif */
static void inject_wg_packet(int region_id, const uint8_t *data, size_t len);

static void derp_recv_task(void *arg)
{
    derp_conn_t *c = (derp_conn_t *)arg;

    /* Per-task frame buffer in PSRAM. A single shared static buffer would
     * race once more than one region connection runs a recv task. */
    uint8_t *frame_buf = heap_caps_malloc(DERP_MAX_FRAME, MALLOC_CAP_SPIRAM);
    if (!frame_buf) frame_buf = malloc(DERP_MAX_FRAME);
    if (!frame_buf) {
        ESP_LOGE(TAG, "DERP recv: frame buffer alloc failed (region %d)",
                 c->region_id);
        c->running = false;
        c->recv_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (c->running) {
        uint8_t frame_type; uint32_t payload_len;
        esp_err_t rc = derp_read_frame(c, &frame_type, frame_buf,
                                       DERP_MAX_FRAME, &payload_len);
        if (rc != ESP_OK) {
            if (!c->running) break;
            /* SO_RCVTIMEO timeout (EAGAIN/EWOULDBLOCK): not a real error */
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            ESP_LOGW(TAG, "DERP recv error (region %d errno=%d) — disconnecting",
                     c->region_id, errno);
            break;
        }
        if (payload_len > DERP_MAX_FRAME) continue; /* oversized, already drained */

        switch (frame_type) {
        case DERP_FRAME_RECV_PACKET:
            ESP_LOGD(TAG, "DERP RecvPacket src=%02x%02x%02x%02x len=%"PRIu32,
                     frame_buf[0], frame_buf[1], frame_buf[2], frame_buf[3],
                     payload_len - 32);
            if (payload_len > 32)
                inject_wg_packet(c->region_id, frame_buf + 32, payload_len - 32);
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

    free(frame_buf);
    c->running = false;
    c->recv_task = NULL;
    vTaskDelete(NULL);
}

/* Allocate and post one frame to the TX queue. Caller may be on any thread,
 * including the lwIP core-locked path — the actual TLS write happens later
 * on derp_tx_task so we never block the tcpip thread on socket I/O. */
static esp_err_t derp_tx_enqueue(derp_conn_t *c, uint8_t frame_type,
                                 const uint8_t *payload, size_t plen)
{
    if (!c || !c->tx_queue) return ESP_ERR_INVALID_STATE;
    derp_tx_item_t *item = malloc(sizeof(*item) + plen);
    if (!item) return ESP_ERR_NO_MEM;
    item->frame_type = frame_type;
    item->plen       = (uint32_t)plen;
    if (plen && payload) memcpy(item->payload, payload, plen);
    if (xQueueSend(c->tx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "DERP TX queue full (region %d); dropping frame type=0x%02x",
                 c->region_id, frame_type);
        free(item);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* TX worker: serialised TLS writer. Pulls items off the queue and writes
 * them to this connection's TLS link under c->tx_mutex. */
static void derp_tx_task(void *arg)
{
    derp_conn_t *c = (derp_conn_t *)arg;
    while (c->running) {
        derp_tx_item_t *item = NULL;
        if (xQueueReceive(c->tx_queue, &item, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;
        if (!c->running) { free(item); break; }
        if (!c->tls)     { free(item); continue; }

        xSemaphoreTake(c->tx_mutex, portMAX_DELAY);
        derp_send_frame(c, item->frame_type, item->payload, item->plen);
        xSemaphoreGive(c->tx_mutex);
        free(item);
    }
    /* Drain any remaining items. */
    derp_tx_item_t *item;
    while (c->tx_queue && xQueueReceive(c->tx_queue, &item, 0) == pdTRUE)
        free(item);
    c->tx_task = NULL;
    vTaskDelete(NULL);
}

/* Keepalive task: enqueues a keepalive frame every DERP_KEEPALIVE_MS. */
static void derp_ka_task(void *arg)
{
    derp_conn_t *c = (derp_conn_t *)arg;
    while (c->running) {
        vTaskDelay(pdMS_TO_TICKS(DERP_KEEPALIVE_MS));
        if (!c->running || !c->tls) break;
        derp_tx_enqueue(c, DERP_FRAME_KEEP_ALIVE, NULL, 0);
    }
    c->ka_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* WireGuard packet injection                                           */
/* ------------------------------------------------------------------ */

/* DERP-relayed packets are raw WireGuard UDP payloads, so we must hand
 * them to wireguardif_network_rx (the callback wireguardif registers on
 * its udp_pcb) — NOT tcpip_input, which would treat the bytes as IP.
 * wireguardif_network_rx decrypts, then feeds the inner IP packet into
 * ip_input → tcp_input — and that path is NOT thread-safe with the
 * tcpip thread running concurrently on the same TCP PCB. Earlier we got
 * away with calling it directly from this task for ICMP traffic (truly
 * stateless), but the moment real TCP echo (SSH keystrokes) started
 * flowing this raced with the tcpip thread inside tcp_receive and
 * crashed at tcp_in.c:1582 on a NULL inseg.p. So we marshal the call
 * via tcpip_callback now — it runs on the tcpip thread, which is the
 * only one allowed to touch tcp_input. The pbuf is freed by
 * wireguardif_network_rx via the WG netif's input path.
 * Synthetic source is in the 127.3.3.0/24 DERP pseudo range so replies
 * route back through our DERP output hook. */
struct udp_pcb;  /* forward decl — opaque here */
extern void wireguardif_network_rx(void *arg, struct udp_pcb *pcb,
                                    struct pbuf *p, const ip_addr_t *addr,
                                    u16_t port);

typedef struct {
    struct netif *nif;
    struct pbuf  *p;
    ip_addr_t     src_ip;
    u16_t         src_port;
} inject_ctx_t;

static void inject_on_tcpip(void *arg)
{
    inject_ctx_t *ic = (inject_ctx_t *)arg;
    wireguardif_network_rx(ic->nif->state, NULL, ic->p, &ic->src_ip, ic->src_port);
    free(ic);
}

static void inject_wg_packet(int region_id, const uint8_t *data, size_t len)
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

    inject_ctx_t *ic = malloc(sizeof(*ic));
    if (!ic) {
        ESP_LOGW(TAG, "inject_wg_packet: ctx alloc failed");
        pbuf_free(p);
        return;
    }
    ic->nif = nif;
    ic->p   = p;
    /* Synthesise the source endpoint as 127.3.3.40:<region> so the WG
     * output hook routes replies back out via the DERP connection for
     * the region the packet arrived on. */
    ip4_addr_t src4;
    ip4addr_aton("127.3.3.40", &src4);
    ip_addr_copy_from_ip4(ic->src_ip, src4);
    ic->src_port = (u16_t)(region_id > 0 ? region_id : 1);

    ESP_LOGD(TAG, "inject_wg: type=0x%02x len=%u via tcpip_callback",
             data[0], (unsigned)len);

    err_t err = tcpip_callback(inject_on_tcpip, ic);
    if (err != ERR_OK) {
        ESP_LOGW(TAG, "tcpip_callback -> %d (dropping packet)", (int)err);
        pbuf_free(p);
        free(ic);
    }
}

/* ------------------------------------------------------------------ */
/* Connection pool plumbing                                             */
/* ------------------------------------------------------------------ */

static void derp_pool_init_once(void)
{
    if (!s_pool_mutex) s_pool_mutex = xSemaphoreCreateMutex();
}

/* Free the per-connection FreeRTOS objects. Call only after the conn's
 * tasks have exited (running cleared + a settle delay). */
static void derp_conn_free_resources(derp_conn_t *c)
{
    if (c->tx_queue) {
        derp_tx_item_t *item;
        while (xQueueReceive(c->tx_queue, &item, 0) == pdTRUE) free(item);
        vQueueDelete(c->tx_queue);
        c->tx_queue = NULL;
    }
    if (c->tx_mutex) {
        vSemaphoreDelete(c->tx_mutex);
        c->tx_mutex = NULL;
    }
    c->recv_task = NULL;
    c->ka_task   = NULL;
    c->tx_task   = NULL;
}

/* Tear down a connection's TLS link and tasks. Blocks (~100 ms) — must run
 * off the tcpip thread. Leaves c->in_use / c->region_id for the caller. */
static void derp_conn_teardown(derp_conn_t *c)
{
    c->running = false;
    if (c->tls) {
        esp_tls_conn_destroy(c->tls);
        c->tls = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(100));   /* let recv/ka/tx tasks observe !running */
    derp_conn_free_resources(c);
}

/* Open one DERP connection (blocking). Runs on a connect task, never on the
 * tcpip thread. Uses the shared node identity. Returns ESP_OK with tasks
 * running, or an error with the TLS link torn down. */
static esp_err_t derp_conn_open(derp_conn_t *c, const ts_derp_node_t *node)
{
    c->node      = *node;
    c->region_id = node->region_id;

    ESP_LOGI(TAG, "DERP: connecting to %s:%d region %d (heap_free=%u)",
             node->hostname, node->derp_port, node->region_id,
             (unsigned)esp_get_free_heap_size());

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .non_block         = false,
        .timeout_ms        = 60000,   /* cert chain verification on real-HW
                                       * P4 + concurrent control-TLS load can
                                       * exceed 20s; give ample headroom. */
    };
    c->tls = esp_tls_init();
    if (!c->tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return ESP_ERR_NO_MEM;
    }
    int ret = esp_tls_conn_new_sync(node->hostname,
                                    (int)strlen(node->hostname),
                                    (int)node->derp_port, &cfg, c->tls);
    if (ret != 1) {
        int mb_err = 0, flags = 0;
        esp_tls_get_and_clear_last_error(&((esp_tls_last_error_t){0}),
                                         &mb_err, &flags);
        ESP_LOGE(TAG, "TLS connect to DERP %s failed: ret=%d mbedtls=-0x%04x flags=0x%x heap=%u",
                 node->hostname, ret, -mb_err, flags,
                 (unsigned)esp_get_free_heap_size());
        esp_tls_conn_destroy(c->tls);
        c->tls = NULL;
        return ESP_FAIL;
    }

    if (!c->tx_mutex) c->tx_mutex = xSemaphoreCreateMutex();
    if (!c->tx_queue) c->tx_queue = xQueueCreate(DERP_TX_QUEUE_LEN,
                                                 sizeof(derp_tx_item_t *));

    esp_err_t err = derp_http_upgrade(c, s_node_priv_saved);
    if (err != ESP_OK) {
        esp_tls_conn_destroy(c->tls);
        c->tls = NULL;
        return err;
    }

    /* Set receive timeout so recv_task can wake up and send keepalives. */
    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(c->tls, &sockfd) == ESP_OK && sockfd >= 0) {
        struct timeval tv = {
            .tv_sec  = DERP_RECV_TIMEOUT_MS / 1000,
            .tv_usec = (DERP_RECV_TIMEOUT_MS % 1000) * 1000,
        };
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    c->running = true;
    xTaskCreate(derp_recv_task, "ts_derp_rx", DERP_RECV_TASK_STACK,
                c, DERP_RECV_TASK_PRIO, &c->recv_task);
    xTaskCreate(derp_ka_task, "ts_derp_ka", DERP_KA_TASK_STACK,
                c, DERP_KA_TASK_PRIO, &c->ka_task);
    xTaskCreate(derp_tx_task, "ts_derp_tx", DERP_TX_TASK_STACK,
                c, DERP_TX_TASK_PRIO, &c->tx_task);

    return ESP_OK;
}

/* Connect task — opens (or re-opens) the connection in slot @p arg for its
 * region_id, then clears the connecting flag. Spawned by derp_ensure_region. */
static void derp_connect_task(void *arg)
{
    derp_conn_t *c = (derp_conn_t *)arg;
    int region = c->region_id;

    /* Reap any stale link from a previous (now-dead) incarnation. */
    if (c->tls || c->tx_queue || c->tx_mutex) {
        derp_conn_teardown(c);
    }

    ts_derp_node_t node;
    esp_err_t err;
    if (!ts_netmap_get_derp_region(region, &node)) {
        ESP_LOGW(TAG, "DERP: region %d unknown — cannot connect", region);
        err = ESP_ERR_NOT_FOUND;
    } else {
        err = derp_conn_open(c, &node);
    }

    xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
    if (err != ESP_OK) {
        c->in_use = false;   /* release the slot for a future retry */
    }
    c->connecting = false;
    xSemaphoreGive(s_pool_mutex);
    vTaskDelete(NULL);
}

/* Find the slot serving @p region (in_use), or NULL. Caller holds the pool
 * mutex. */
static derp_conn_t *derp_find_region(int region)
{
    for (int i = 0; i < DERP_MAX_CONNS; i++)
        if (s_conns[i].in_use && s_conns[i].region_id == region)
            return &s_conns[i];
    return NULL;
}

/* Reserve a free slot for @p region (marks in_use + connecting). Caller holds
 * the pool mutex. Returns NULL if the pool is full. */
static derp_conn_t *derp_reserve_slot(int region)
{
    for (int i = 0; i < DERP_MAX_CONNS; i++) {
        if (!s_conns[i].in_use) {
            derp_conn_t *c = &s_conns[i];
            c->in_use     = true;
            c->connecting = true;
            c->region_id  = region;
            c->running    = false;
            c->tls        = NULL;
            return c;
        }
    }
    return NULL;
}

/* Return a ready connection for @p region, or NULL while one is being
 * established. Never blocks (safe from the tcpip thread) — actual connecting
 * is offloaded to derp_connect_task. A dead slot is re-claimed and reconnected. */
static derp_conn_t *derp_ensure_region(int region)
{
    if (region <= 0 || !s_pool_mutex) return NULL;

    derp_conn_t *ready = NULL;
    derp_conn_t *slot  = NULL;

    xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
    derp_conn_t *c = derp_find_region(region);
    if (c) {
        if (c->connecting) {
            /* connect/reap already in progress */
        } else if (c->running && c->tls) {
            ready = c;
        } else {
            /* dead slot — re-claim it and reconnect on this region */
            c->connecting = true;
            slot = c;
        }
    } else {
        slot = derp_reserve_slot(region);
        if (!slot)
            ESP_LOGW(TAG, "DERP pool full (%d conns) — cannot open region %d",
                     DERP_MAX_CONNS, region);
    }
    xSemaphoreGive(s_pool_mutex);

    if (ready) return ready;

    if (slot) {
        BaseType_t ok = xTaskCreate(derp_connect_task, "ts_derp_conn",
                                    12288, slot, 4, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "DERP: connect task spawn failed for region %d", region);
            xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
            slot->in_use     = false;
            slot->connecting = false;
            xSemaphoreGive(s_pool_mutex);
        }
    }
    return NULL;   /* not ready yet; WG retransmits its handshake */
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t ts_derp_connect(const ts_derp_node_t *node,
                          const uint8_t node_priv[32],
                          const uint8_t node_pub[32])
{
    /* Treat as "connect to this node's region as home". */
    return ts_derp_set_home(node, node_priv, node_pub);
}

esp_err_t ts_derp_send(int region, const uint8_t dst_pub[32],
                       const uint8_t *pkt, size_t pkt_len)
{
    if (region <= 0) region = s_home_region;

    derp_conn_t *c = derp_ensure_region(region);
    if (!c) return ESP_ERR_INVALID_STATE;   /* connecting / unavailable */

    /* SendPacket payload: [32 dst pub][WG packet] */
    size_t payload_len = 32 + pkt_len;
    uint8_t *payload = malloc(payload_len);
    if (!payload) return ESP_ERR_NO_MEM;
    memcpy(payload,      dst_pub, 32);
    memcpy(payload + 32, pkt,     pkt_len);

    /* Defer the TLS write to derp_tx_task so callers (including the lwIP
     * core-lock holder) don't block on socket I/O. */
    esp_err_t err = derp_tx_enqueue(c, DERP_FRAME_SEND_PACKET,
                                    payload, payload_len);
    free(payload);

    ESP_LOGD(TAG, "ts_derp_send region=%d dst=%02x%02x%02x%02x type=0x%02x len=%u enq=%d",
             region, dst_pub[0], dst_pub[1], dst_pub[2], dst_pub[3],
             pkt_len > 0 ? pkt[0] : 0, (unsigned)pkt_len, err);

    return err;
}

void ts_derp_disconnect(void)
{
    if (!s_pool_mutex) return;
    for (int i = 0; i < DERP_MAX_CONNS; i++) {
        xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
        bool active = s_conns[i].in_use;
        xSemaphoreGive(s_pool_mutex);
        if (!active) continue;
        derp_conn_teardown(&s_conns[i]);
        xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
        s_conns[i].in_use     = false;
        s_conns[i].connecting = false;
        xSemaphoreGive(s_pool_mutex);
    }
}

esp_err_t ts_derp_set_home(const ts_derp_node_t *node,
                           const uint8_t node_priv[32],
                           const uint8_t node_pub[32])
{
    derp_pool_init_once();

    /* Save the shared identity used for every region connection. */
    memcpy(s_node_pub,        node_pub,  32);
    memcpy(s_node_priv_saved, node_priv, 32);
    s_home_region = node->region_id;

    /* Make sure the netmap can resolve the home region for the connect task
     * even before the next DERPMap is parsed: if it can't, fall through and
     * derp_connect_task will log "region unknown". */
    ESP_LOGI(TAG, "DERP: home region %d (%s)", node->region_id, node->hostname);
    derp_ensure_region(node->region_id);
    return ESP_OK;
}

/* wireguard_derp_output_fn-compatible wrapper. @p region is the peer's home
 * DERP region (carried in the WG peer's pseudo-endpoint port). */
void ts_derp_send_packet(const uint8_t *peer_pub,
                         const uint8_t *pkt, size_t pkt_len, int region)
{
    ts_derp_send(region, peer_pub, pkt, pkt_len);
}
