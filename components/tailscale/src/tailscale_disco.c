/*
 * Tailscale DISCO — minimum-viable implementation for direct UDP.
 *
 * See tailscale_disco.h for the wire format. This module is the data-
 * plane glue: a Ping sender, a Pong responder, and a Pong handler that
 * promotes the verified address to the WireGuard peer's endpoint.
 *
 * Threading: ts_disco_rx() runs on the lwIP tcpip thread (called from
 * wireguardif_network_rx). ts_disco_send_ping() may be called from any
 * thread; wireguard_esp32_udp_send() handles the marshalling.
 *
 * Scope: Ping/Pong over the WG UDP port. CallMeMaybe and explicit
 * trust-window expiry are deliberately left out (see plan file's
 * "Open risks" section).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_disco.h"
#include "tailscale_control.h"
#include "tailscale_derp.h"
#include "tailscale_nacl.h"
#include "tailscale_netmap.h"

#include "wireguard_esp32.h"
#include "crypto.h"  /* wireguard_x25519, wireguard_random_bytes */

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/inet.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "ts_disco";

/* ------------------------------------------------------------------ */
/* Wire constants                                                       */
/* ------------------------------------------------------------------ */

#define DISCO_MAGIC_LEN     6
#define DISCO_SENDER_LEN    32
#define DISCO_NONCE_LEN     24
#define DISCO_TAG_LEN       16
#define DISCO_HEADER_LEN    (DISCO_MAGIC_LEN + DISCO_SENDER_LEN + DISCO_NONCE_LEN)

#define DISCO_INNER_HDR_LEN 2   /* type + version */
#define DISCO_TXID_LEN      12

#define DISCO_PING_BODY_LEN (DISCO_TXID_LEN + 32 /* NodeKey */)
#define DISCO_PONG_BODY_LEN (DISCO_TXID_LEN + 16 /* SrcIP */ + 2 /* SrcPort */)

static const uint8_t DISCO_MAGIC[DISCO_MAGIC_LEN] =
    { 0x54, 0x53, 0xF0, 0x9F, 0x92, 0xAC };

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

#define DISCO_PENDING_MAX 64   /* up to TS_NETMAP_MAX_CANDIDATES * peers */
#define DISCO_VERIFIED_MAX 16  /* one slot per WG peer; idx == wg_peer_index */

typedef struct {
    bool     in_use;
    uint8_t  tx_id[DISCO_TXID_LEN];
    uint8_t  peer_disco_pub[32];   /* used to verify Pong sender identity */
    uint8_t  wg_peer_index;
} pending_ping_t;

static uint8_t         s_disco_priv[32];
static uint8_t         s_disco_pub[32];
static uint8_t         s_node_pub[32];
static bool            s_inited = false;
static pending_ping_t  s_pending[DISCO_PENDING_MAX];
/* Per-WG-peer "already promoted to direct" flag. Set on the first valid
 * Pong; later Pongs for the same WG peer (from races between candidates)
 * are ignored so we don't toggle the endpoint back and forth across
 * unreachable interfaces. Cleared when the peer is removed from the
 * netmap (or, conservatively, never — re-init zeros it). */
static bool            s_verified[DISCO_VERIFIED_MAX];

/* Rate-limit: at most one outbound CallMeMaybe per WG peer per 30 s.
 * Indexed by wg_peer_index. esp_timer_get_time() is microseconds since boot. */
#define DISCO_PEERS_MAX                16
#define DISCO_CALL_ME_MAYBE_INTERVAL_US (30LL * 1000LL * 1000LL)
static int64_t         s_last_call_me_maybe_ts[DISCO_PEERS_MAX];

/* Forward decl — registered with wireguardif as the DISCO input callback. */
static void ts_disco_rx(const uint8_t *data, size_t len,
                        const ip_addr_t *src_ip, uint16_t src_port);

/* ------------------------------------------------------------------ */
/* Pending Ping bookkeeping                                             */
/* ------------------------------------------------------------------ */

static pending_ping_t *pending_alloc(void)
{
    for (int i = 0; i < DISCO_PENDING_MAX; i++) {
        if (!s_pending[i].in_use) return &s_pending[i];
    }
    /* Overflow: recycle slot 0. The lost ping just won't trigger an
     * endpoint update; the next netmap apply will issue another. */
    return &s_pending[0];
}

static pending_ping_t *pending_find(const uint8_t tx_id[DISCO_TXID_LEN])
{
    for (int i = 0; i < DISCO_PENDING_MAX; i++) {
        if (s_pending[i].in_use &&
            memcmp(s_pending[i].tx_id, tx_id, DISCO_TXID_LEN) == 0) {
            return &s_pending[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Wire packing / unpacking                                             */
/* ------------------------------------------------------------------ */

/* Build the full DISCO frame for `inner_pt` (the plaintext post the
 * 2-byte type/version header) destined for `peer_disco_pub`. `out`
 * receives DISCO_HEADER_LEN + DISCO_TAG_LEN + inner_pt_len bytes. */
static esp_err_t build_disco_frame(const uint8_t peer_disco_pub[32],
                                   const uint8_t *inner_pt, size_t inner_pt_len,
                                   uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t need = DISCO_HEADER_LEN + DISCO_TAG_LEN + inner_pt_len;
    if (out_cap < need) return ESP_ERR_NO_MEM;

    uint8_t nonce[DISCO_NONCE_LEN];
    esp_fill_random(nonce, sizeof(nonce));

    uint8_t shared[32];
    wireguard_x25519(shared, s_disco_priv, peer_disco_pub);

    memcpy(out + 0,                                   DISCO_MAGIC, DISCO_MAGIC_LEN);
    memcpy(out + DISCO_MAGIC_LEN,                     s_disco_pub, DISCO_SENDER_LEN);
    memcpy(out + DISCO_MAGIC_LEN + DISCO_SENDER_LEN,  nonce,       DISCO_NONCE_LEN);

    /* Box output appended after the header: tag(16) || ct(inner_pt_len). */
    ts_nacl_box_seal(shared, nonce, inner_pt, inner_pt_len,
                     out + DISCO_HEADER_LEN);

    *out_len = need;
    return ESP_OK;
}

/* Decrypt the box part of a received DISCO frame. `inner_pt_buf` gets
 * the plaintext (including the 2-byte type/version header). Returns
 * the plaintext length, or -1 on tag-verify failure. */
static int open_disco_frame(const uint8_t *frame, size_t frame_len,
                            const uint8_t sender_pub[32],
                            uint8_t *inner_pt_buf, size_t buf_cap)
{
    if (frame_len < DISCO_HEADER_LEN + DISCO_TAG_LEN) return -1;
    const uint8_t *nonce = frame + DISCO_MAGIC_LEN + DISCO_SENDER_LEN;
    const uint8_t *box   = frame + DISCO_HEADER_LEN;
    size_t         boxlen = frame_len - DISCO_HEADER_LEN;
    size_t         pt_len = boxlen - DISCO_TAG_LEN;
    if (buf_cap < pt_len) return -1;

    uint8_t shared[32];
    wireguard_x25519(shared, s_disco_priv, sender_pub);
    if (!ts_nacl_box_open(shared, nonce, box, boxlen, inner_pt_buf)) {
        return -1;
    }
    return (int)pt_len;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t ts_disco_init(const uint8_t disco_priv[32],
                        const uint8_t disco_pub[32],
                        const uint8_t node_pub[32])
{
    if (!disco_priv || !disco_pub || !node_pub) return ESP_ERR_INVALID_ARG;
    memcpy(s_disco_priv, disco_priv, 32);
    memcpy(s_disco_pub,  disco_pub,  32);
    memcpy(s_node_pub,   node_pub,   32);
    memset(s_pending,    0, sizeof(s_pending));
    memset(s_verified,   0, sizeof(s_verified));
    memset(s_last_call_me_maybe_ts, 0, sizeof(s_last_call_me_maybe_ts));
    s_inited = true;

    wireguard_esp32_set_disco_input(ts_disco_rx);
    ESP_LOGI(TAG, "DISCO ready (disco_pub %02x%02x%02x%02x... node_pub %02x%02x%02x%02x...)",
             disco_pub[0], disco_pub[1], disco_pub[2], disco_pub[3],
             node_pub[0],  node_pub[1],  node_pub[2],  node_pub[3]);
    return ESP_OK;
}

esp_err_t ts_disco_send_ping(uint8_t wg_peer_index,
                             const uint8_t peer_node_pub[32],
                             const uint8_t peer_disco_pub[32],
                             const ip_addr_t *cand_ip, uint16_t cand_port)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!peer_disco_pub || !cand_ip || cand_port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Inner plaintext: [type=Ping][ver=0] || TxID[12] || NodeKey[32] */
    uint8_t inner[DISCO_INNER_HDR_LEN + DISCO_PING_BODY_LEN];
    inner[0] = DISCO_MSG_PING;
    inner[1] = 0;  /* version */
    uint8_t *tx_id_field = inner + DISCO_INNER_HDR_LEN;
    esp_fill_random(tx_id_field, DISCO_TXID_LEN);
    memcpy(inner + DISCO_INNER_HDR_LEN + DISCO_TXID_LEN, s_node_pub, 32);
    (void)peer_node_pub;  /* not needed in the Ping body; we send our own */

    uint8_t frame[DISCO_HEADER_LEN + DISCO_TAG_LEN +
                  DISCO_INNER_HDR_LEN + DISCO_PING_BODY_LEN];
    size_t  frame_len = 0;
    esp_err_t err = build_disco_frame(peer_disco_pub, inner, sizeof(inner),
                                      frame, sizeof(frame), &frame_len);
    if (err != ESP_OK) return err;

    err_t lwerr = wireguard_esp32_udp_send(cand_ip, cand_port, frame, frame_len);
    if (lwerr != ERR_OK) {
        ESP_LOGW(TAG, "udp_send -> %d for ping to %08x:%u",
                 (int)lwerr,
                 (unsigned)ip_2_ip4(cand_ip)->addr, (unsigned)cand_port);
        return ESP_FAIL;
    }

    /* Register pending so the Pong handler can route the endpoint update
     * to the right wg peer. */
    pending_ping_t *p = pending_alloc();
    p->in_use = true;
    memcpy(p->tx_id,          tx_id_field,    DISCO_TXID_LEN);
    memcpy(p->peer_disco_pub, peer_disco_pub, 32);
    p->wg_peer_index = wg_peer_index;

    ESP_LOGI(TAG, "Ping sent peer=%u to %u.%u.%u.%u:%u txid=%02x%02x%02x%02x...",
             wg_peer_index,
             (unsigned)((ip_2_ip4(cand_ip)->addr      ) & 0xFF),
             (unsigned)((ip_2_ip4(cand_ip)->addr >>  8) & 0xFF),
             (unsigned)((ip_2_ip4(cand_ip)->addr >> 16) & 0xFF),
             (unsigned)((ip_2_ip4(cand_ip)->addr >> 24) & 0xFF),
             (unsigned)cand_port,
             tx_id_field[0], tx_id_field[1], tx_id_field[2], tx_id_field[3]);
    return ESP_OK;
}

/* Pack one "a.b.c.d:port" endpoint string as an 18-byte AddrPort record:
 * IPv4-mapped IPv6 (10 zero bytes, 0xFF, 0xFF, then 4 bytes IPv4 in network
 * order), followed by 2 bytes of port in big-endian. Returns true on parse. */
static bool pack_addrport_v4(const char *ep_str, uint8_t out[18])
{
    const char *colon = strrchr(ep_str, ':');
    if (!colon) return false;
    char ip_buf[40];
    size_t ip_len = (size_t)(colon - ep_str);
    if (ip_len == 0 || ip_len >= sizeof(ip_buf)) return false;
    memcpy(ip_buf, ep_str, ip_len);
    ip_buf[ip_len] = '\0';

    ip4_addr_t v4;
    if (!ip4addr_aton(ip_buf, &v4)) return false;
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return false;

    memset(out, 0, 10);
    out[10] = 0xFF;
    out[11] = 0xFF;
    memcpy(out + 12, &v4.addr, 4);    /* network byte order */
    out[16] = (uint8_t)((port >> 8) & 0xFF);
    out[17] = (uint8_t)(port & 0xFF);
    return true;
}

esp_err_t ts_disco_send_call_me_maybe(uint8_t wg_peer_index,
                                       const uint8_t peer_node_pub[32],
                                       const uint8_t peer_disco_pub[32],
                                       uint8_t peer_derp_region)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!peer_node_pub || !peer_disco_pub) return ESP_ERR_INVALID_ARG;
    if (peer_derp_region == 0) return ESP_ERR_INVALID_ARG;

    /* Skip peers we've already verified as direct — no point in
     * inviting more reverse Pings to a path that's already up. */
    if (wg_peer_index < DISCO_VERIFIED_MAX && s_verified[wg_peer_index]) {
        return ESP_OK;
    }

    /* Rate limit. */
    int64_t now = esp_timer_get_time();
    if (wg_peer_index < DISCO_PEERS_MAX) {
        int64_t last = s_last_call_me_maybe_ts[wg_peer_index];
        if (last != 0 && (now - last) < DISCO_CALL_ME_MAYBE_INTERVAL_US) {
            return ESP_OK;
        }
    }

    /* Snapshot the endpoints we've advertised to the control server.
     * If we haven't any yet (STUN / pubip discovery still in progress),
     * silently skip — the next netmap_apply tick will retry. */
    char eps[CTRL_MAX_ENDPOINTS][CTRL_MAX_EP_LEN];
    int n_eps = ts_ctrl_get_endpoints(eps, CTRL_MAX_ENDPOINTS);
    if (n_eps <= 0) {
        ESP_LOGI(TAG, "call_me_maybe: skip peer %u — no endpoints to advertise",
                 wg_peer_index);
        return ESP_OK;
    }

    /* Build inner plaintext: [type=CallMeMaybe][ver=0] || AddrPort * N. */
    uint8_t inner[DISCO_INNER_HDR_LEN + 18 * CTRL_MAX_ENDPOINTS];
    inner[0] = DISCO_MSG_CALL_ME_MAYBE;
    inner[1] = 0;
    int packed = 0;
    for (int i = 0; i < n_eps; i++) {
        if (eps[i][0] == '\0') continue;
        if (pack_addrport_v4(eps[i], inner + DISCO_INNER_HDR_LEN + packed * 18)) {
            packed++;
        }
    }
    if (packed == 0) {
        ESP_LOGI(TAG, "call_me_maybe: skip peer %u — no parseable endpoints",
                 wg_peer_index);
        return ESP_OK;
    }
    size_t inner_len = DISCO_INNER_HDR_LEN + (size_t)packed * 18;

    uint8_t frame[DISCO_HEADER_LEN + DISCO_TAG_LEN +
                  DISCO_INNER_HDR_LEN + 18 * CTRL_MAX_ENDPOINTS];
    size_t  frame_len = 0;
    esp_err_t err = build_disco_frame(peer_disco_pub, inner, inner_len,
                                      frame, sizeof(frame), &frame_len);
    if (err != ESP_OK) return err;

    /* Send via DERP — the whole point of CallMeMaybe is that we don't
     * yet have a direct UDP path to the peer. */
    err = ts_derp_send((int)peer_derp_region, peer_node_pub,
                       frame, frame_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CallMeMaybe ts_derp_send peer=%u region=%u -> %d",
                 wg_peer_index, peer_derp_region, (int)err);
        return err;
    }

    if (wg_peer_index < DISCO_PEERS_MAX) {
        s_last_call_me_maybe_ts[wg_peer_index] = now;
    }
    ESP_LOGI(TAG, "CallMeMaybe sent peer=%u to derp=%u with %d eps",
             wg_peer_index, peer_derp_region, packed);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Receive path                                                         */
/* ------------------------------------------------------------------ */

/* Build and send a Pong in response to a received Ping. */
static void send_pong(const uint8_t sender_pub[32],
                      const uint8_t tx_id[DISCO_TXID_LEN],
                      const ip_addr_t *src_ip, uint16_t src_port)
{
    uint8_t inner[DISCO_INNER_HDR_LEN + DISCO_PONG_BODY_LEN];
    inner[0] = DISCO_MSG_PONG;
    inner[1] = 0;
    uint8_t *body = inner + DISCO_INNER_HDR_LEN;
    memcpy(body, tx_id, DISCO_TXID_LEN);
    /* SrcIP[16]: IPv4-mapped IPv6 = ::ffff:a.b.c.d */
    uint8_t *srcip16 = body + DISCO_TXID_LEN;
    memset(srcip16, 0, 10);
    srcip16[10] = 0xFF;
    srcip16[11] = 0xFF;
    uint32_t addr = ip_2_ip4(src_ip)->addr;     /* network byte order */
    memcpy(srcip16 + 12, &addr, 4);
    /* SrcPort[2] big-endian */
    body[DISCO_TXID_LEN + 16 + 0] = (uint8_t)(src_port >> 8);
    body[DISCO_TXID_LEN + 16 + 1] = (uint8_t)(src_port & 0xFF);

    uint8_t frame[DISCO_HEADER_LEN + DISCO_TAG_LEN +
                  DISCO_INNER_HDR_LEN + DISCO_PONG_BODY_LEN];
    size_t  frame_len = 0;
    if (build_disco_frame(sender_pub, inner, sizeof(inner),
                          frame, sizeof(frame), &frame_len) != ESP_OK) {
        return;
    }

    err_t lwerr = wireguard_esp32_udp_send(src_ip, src_port, frame, frame_len);
    if (lwerr != ERR_OK) {
        ESP_LOGW(TAG, "Pong udp_send -> %d", (int)lwerr);
        return;
    }
    ESP_LOGI(TAG, "Pong sent to %u.%u.%u.%u:%u",
             (unsigned)((addr      ) & 0xFF),
             (unsigned)((addr >>  8) & 0xFF),
             (unsigned)((addr >> 16) & 0xFF),
             (unsigned)((addr >> 24) & 0xFF),
             (unsigned)src_port);
}

static void ts_disco_rx(const uint8_t *data, size_t len,
                        const ip_addr_t *src_ip, uint16_t src_port)
{
    if (!s_inited) return;
    if (len < DISCO_HEADER_LEN + DISCO_TAG_LEN + DISCO_INNER_HDR_LEN) return;
    if (memcmp(data, DISCO_MAGIC, DISCO_MAGIC_LEN) != 0) return;

    const uint8_t *sender_pub = data + DISCO_MAGIC_LEN;

    /* Decrypt inner plaintext. */
    uint8_t pt[256];
    int pt_len = open_disco_frame(data, len, sender_pub, pt, sizeof(pt));
    if (pt_len < 0) {
        ESP_LOGD(TAG, "drop: box open failed (sender %02x%02x%02x%02x...)",
                 sender_pub[0], sender_pub[1], sender_pub[2], sender_pub[3]);
        return;
    }
    if (pt_len < DISCO_INNER_HDR_LEN) return;

    uint8_t type = pt[0];
    /* pt[1] = version, ignore */

    switch (type) {
    case DISCO_MSG_PING: {
        if (pt_len < DISCO_INNER_HDR_LEN + DISCO_TXID_LEN) return;
        const uint8_t *tx_id = pt + DISCO_INNER_HDR_LEN;
        ESP_LOGI(TAG, "Ping recv from %08x:%u txid=%02x%02x%02x%02x...",
                 (unsigned)ip_2_ip4(src_ip)->addr, (unsigned)src_port,
                 tx_id[0], tx_id[1], tx_id[2], tx_id[3]);
        send_pong(sender_pub, tx_id, src_ip, src_port);
        break;
    }
    case DISCO_MSG_PONG: {
        if (pt_len < DISCO_INNER_HDR_LEN + DISCO_TXID_LEN) return;
        const uint8_t *tx_id = pt + DISCO_INNER_HDR_LEN;
        pending_ping_t *pp = pending_find(tx_id);
        if (!pp) {
            ESP_LOGD(TAG, "Pong with unknown txid %02x%02x%02x%02x...",
                     tx_id[0], tx_id[1], tx_id[2], tx_id[3]);
            return;
        }
        /* Verify the responder is who we pinged. */
        if (memcmp(pp->peer_disco_pub, sender_pub, 32) != 0) {
            ESP_LOGW(TAG, "Pong txid match but disco_pub mismatch (drop)");
            return;
        }
        uint8_t wg_idx = pp->wg_peer_index;
        pp->in_use = false;

        /* First valid Pong wins. Later pongs for the same WG peer
         * (multi-probe races among candidates) shouldn't toggle the
         * endpoint back to a path that was simply slower-but-still-
         * reachable. */
        if (wg_idx < DISCO_VERIFIED_MAX && s_verified[wg_idx]) {
            ESP_LOGD(TAG, "Pong from %08x:%u for peer %u — already direct",
                     (unsigned)ip_2_ip4(src_ip)->addr, (unsigned)src_port,
                     wg_idx);
            return;
        }
        if (wg_idx < DISCO_VERIFIED_MAX) s_verified[wg_idx] = true;

        ESP_LOGI(TAG, "Pong recv from %u.%u.%u.%u:%u → peer %u direct",
                 (unsigned)((ip_2_ip4(src_ip)->addr      ) & 0xFF),
                 (unsigned)((ip_2_ip4(src_ip)->addr >>  8) & 0xFF),
                 (unsigned)((ip_2_ip4(src_ip)->addr >> 16) & 0xFF),
                 (unsigned)((ip_2_ip4(src_ip)->addr >> 24) & 0xFF),
                 (unsigned)src_port, wg_idx);
        wireguard_esp32_update_endpoint(wg_idx, src_ip, src_port);
        break;
    }
    case DISCO_MSG_CALL_ME_MAYBE: {
        /* Body: zero or more 18-byte AddrPort records (IPv6[16] + Port[2 BE]).
         * Semantics: "I'm sender. Ping me at any of these addresses now." */
        const uint8_t *body     = pt + DISCO_INNER_HDR_LEN;
        size_t         body_len = (size_t)pt_len - DISCO_INNER_HDR_LEN;
        if (body_len % 18 != 0) {
            ESP_LOGW(TAG, "CallMeMaybe: ragged body %u B", (unsigned)body_len);
            return;
        }
        uint8_t wg_idx;
        uint8_t peer_node_pub[32];
        if (!ts_netmap_lookup_by_disco(sender_pub, &wg_idx, peer_node_pub)) {
            ESP_LOGD(TAG, "CallMeMaybe: unknown sender");
            return;
        }
        int n = (int)(body_len / 18);
        ESP_LOGI(TAG, "CallMeMaybe from peer %u with %d addrs", wg_idx, n);
        for (int i = 0; i < n; i++) {
            const uint8_t *rec = body + i * 18;
            /* Tab5 has no IPv6 — accept only v4-mapped (::ffff:a.b.c.d). */
            bool v4mapped = true;
            for (int j = 0; j < 10; j++) {
                if (rec[j]) { v4mapped = false; break; }
            }
            if (v4mapped && (rec[10] != 0xFF || rec[11] != 0xFF)) {
                v4mapped = false;
            }
            if (!v4mapped) continue;
            uint32_t addr_be;
            memcpy(&addr_be, rec + 12, 4);
            uint16_t port = ((uint16_t)rec[16] << 8) | rec[17];
            uint32_t a_h = ntohl(addr_be);
            if (port == 0 || a_h == 0)                continue;
            if ((a_h & 0xFF000000) == 0x7F000000)     continue;  /* 127/8 */
            if ((a_h & 0xF0000000) == 0xE0000000)     continue;  /* 224/4 */
            ip4_addr_t v4;
            ip4_addr_set_u32(&v4, addr_be);
            ip_addr_t ip;
            ip_addr_copy_from_ip4(ip, v4);
            ts_disco_send_ping(wg_idx, peer_node_pub, sender_pub, &ip, port);
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "drop: unsupported DISCO type 0x%02x", type);
        break;
    }
}
