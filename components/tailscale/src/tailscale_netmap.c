/*
 * Tailscale network map — MapResponse stream parser and WireGuard peer manager.
 *
 * Uses the SAX-style ts_js streaming JSON parser instead of cJSON. This keeps
 * memory bounded by the input buffer length plus O(depth) parser state, which
 * is essential on the ESP32 where the ~28 KB MapResponse JSON would exhaust
 * cJSON's tree allocations alongside the WiFi/TLS heap.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_netmap.h"
#include "tailscale_derp.h"
#include "tailscale_disco.h"
#include "tailscale_keys.h"
#include "tailscale_jstream.h"

#include "wireguard_esp32.h"   /* wireguard_esp32_add_peer / remove / update */

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lwip/ip_addr.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static const char *TAG = "ts_netmap";

/* ------------------------------------------------------------------ */
/* Peer table                                                           */
/* ------------------------------------------------------------------ */

#define TS_NETMAP_MAX_PEERS 16

typedef struct {
    bool     active;
    uint8_t  wg_index;         /* WireGuard peer index */
    int64_t  node_id;          /* Tailscale NodeID (for PeersRemoved) */
    uint8_t  node_pub[32];     /* WireGuard public key (raw) */
    uint8_t  disco_pub[32];    /* DISCO public key (raw) */
    char     ts_ip[20];        /* 100.x.y.z */
    char     name[64];         /* e.g. "kenta-laptop.tail-xxxxx.ts.net" */
    int      derp_region;      /* peer's home DERP region (debug) */
} netmap_peer_t;

static EXT_RAM_BSS_ATTR netmap_peer_t s_peers[TS_NETMAP_MAX_PEERS];
static char          s_self_ip[20];
static ts_derp_node_t s_derp_home;
static bool           s_derp_home_valid = false;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* "100.x.y.z/32" → "100.x.y.z" */
static void strip_prefix(const char *cidr, char *ip_out, size_t ip_out_len)
{
    strlcpy(ip_out, cidr, ip_out_len);
    char *slash = strchr(ip_out, '/');
    if (slash) *slash = '\0';
}

/* "1.2.3.4:12345" → ip_addr_t + port */
static bool parse_endpoint(const char *ep_str,
                            ip_addr_t *ip_out, uint16_t *port_out)
{
    const char *colon = strrchr(ep_str, ':');
    if (!colon) return false;

    char ip_buf[40];
    size_t ip_len = (size_t)(colon - ep_str);
    if (ip_len == 0 || ip_len >= sizeof(ip_buf)) return false;
    memcpy(ip_buf, ep_str, ip_len);
    ip_buf[ip_len] = '\0';

    ip4_addr_t ip4;
    if (!ip4addr_aton(ip_buf, &ip4)) return false;
    ip_addr_copy_from_ip4(*ip_out, ip4);

    *port_out = (uint16_t)atoi(colon + 1);
    return (*port_out != 0);
}

static int find_peer_slot(const uint8_t pub[32])
{
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (s_peers[i].active &&
            memcmp(s_peers[i].node_pub, pub, 32) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_peer_slot_by_id(int64_t node_id)
{
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (s_peers[i].active && s_peers[i].node_id == node_id) {
            return i;
        }
    }
    return -1;
}

static int alloc_peer_slot(void)
{
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (!s_peers[i].active) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Streaming parser helpers                                             */
/* ------------------------------------------------------------------ */

/* After reading an OBJ_START, walk member-by-member calling key_handler
 * for each KEY event. The handler is responsible for consuming the value
 * (either via ts_js_skip() to ignore, or by reading it). Returns at the
 * matching OBJ_END. */
typedef bool (*key_handler_fn)(ts_js_t *j, void *ctx);

static bool walk_object(ts_js_t *j, key_handler_fn handler, void *ctx)
{
    while (1) {
        ts_js_evt_t e = ts_js_next(j);
        if (e == TS_JS_OBJ_END) return true;
        if (e != TS_JS_KEY) return false;
        if (!handler(j, ctx)) {
            /* Handler indicated it didn't consume the value — skip it. */
            if (ts_js_skip(j) == TS_JS_ERROR) return false;
        }
    }
}

/* Consume the next value, which must be a TS_JS_STRING, into dst. Returns
 * true if a string was read (possibly truncated). */
static bool read_string_value(ts_js_t *j, char *dst, size_t dst_size)
{
    ts_js_evt_t e = ts_js_next(j);
    if (e != TS_JS_STRING) {
        if (e == TS_JS_OBJ_START || e == TS_JS_ARR_START) {
            /* Not a string — back up by skipping the rest. */
            while (j->depth > 0) {
                ts_js_evt_t e2 = ts_js_next(j);
                if (e2 == TS_JS_END || e2 == TS_JS_ERROR) break;
                if ((e2 == TS_JS_OBJ_END || e2 == TS_JS_ARR_END) &&
                    j->depth == 0) break;
            }
        }
        return false;
    }
    ts_js_str_copy(j, dst, dst_size);
    return true;
}

/* ------------------------------------------------------------------ */
/* "Node" object — extract self Tailscale IP                            */
/* ------------------------------------------------------------------ */

static bool node_key_handler(ts_js_t *j, void *ctx)
{
    (void)ctx;
    if (!ts_js_str_eq(j, "Addresses")) return false;

    /* Expect an array of strings; we only want the first. */
    if (ts_js_next(j) != TS_JS_ARR_START) return true;

    ts_js_evt_t e = ts_js_next(j);
    if (e == TS_JS_STRING) {
        char addr[40];
        ts_js_str_copy(j, addr, sizeof(addr));
        strip_prefix(addr, s_self_ip, sizeof(s_self_ip));
        ESP_LOGI(TAG, "Self Tailscale IP: %s", s_self_ip);
        /* /10 covers Tailscale's 100.64.0.0/10 CGNAT range so lwIP routes
         * all peer traffic through the WG netif. */
        wireguard_esp32_set_address(s_self_ip, "255.192.0.0");
        /* Drain remaining array elements. */
        while ((e = ts_js_next(j)) != TS_JS_ARR_END &&
               e != TS_JS_END && e != TS_JS_ERROR) {
            /* tokens / values until ] */
        }
    }
    return true;
}

static void parse_node_obj(ts_js_t *j)
{
    if (ts_js_next(j) != TS_JS_OBJ_START) return;
    walk_object(j, node_key_handler, NULL);
}

/* ------------------------------------------------------------------ */
/* "DERPMap" — pick smallest region ID with a valid Nodes[0]            */
/* ------------------------------------------------------------------ */

typedef struct {
    int             best_id;
    ts_derp_node_t  best_node;
    bool            found;
} derp_state_t;

/* Inside a region's Nodes[0] object: collect HostName, DERPPort, STUNPort. */
static bool node0_key_handler(ts_js_t *j, void *ctx)
{
    ts_derp_node_t *n = (ts_derp_node_t *)ctx;
    if (ts_js_str_eq(j, "HostName")) {
        char host[64];
        if (read_string_value(j, host, sizeof(host)))
            strlcpy(n->hostname, host, sizeof(n->hostname));
        return true;
    }
    if (ts_js_str_eq(j, "DERPPort")) {
        if (ts_js_next(j) != TS_JS_NUMBER) return true;
        n->derp_port = (uint16_t)ts_js_int(j);
        return true;
    }
    if (ts_js_str_eq(j, "STUNPort")) {
        if (ts_js_next(j) != TS_JS_NUMBER) return true;
        n->stun_port = (uint16_t)ts_js_int(j);
        return true;
    }
    return false;
}

/* Inside a region object: collect RegionID, parse first Nodes[] entry. */
typedef struct {
    int             region_id;
    ts_derp_node_t  node0;
    bool            have_node0;
} region_state_t;

static bool region_key_handler(ts_js_t *j, void *ctx)
{
    region_state_t *r = (region_state_t *)ctx;
    if (ts_js_str_eq(j, "RegionID")) {
        if (ts_js_next(j) != TS_JS_NUMBER) return true;
        r->region_id = ts_js_int(j);
        return true;
    }
    if (ts_js_str_eq(j, "Nodes")) {
        if (ts_js_next(j) != TS_JS_ARR_START) return true;
        /* First element is the node we care about; skip the rest. */
        ts_js_evt_t e = ts_js_next(j);
        if (e == TS_JS_OBJ_START) {
            r->node0.derp_port = 443;
            r->node0.stun_port = 3478;
            walk_object(j, node0_key_handler, &r->node0);
            r->have_node0 = true;
        }
        /* Drain any remaining array elements. */
        while (j->depth > 0) {
            e = ts_js_next(j);
            if (e == TS_JS_ARR_END && j->scope[j->depth] != '[') {
                break;
            }
            if (e == TS_JS_ARR_END || e == TS_JS_END || e == TS_JS_ERROR) break;
        }
        return true;
    }
    return false;
}

/* Inside Regions object: each member key is a region ID string, value is
 * the region object. */
static bool regions_key_handler(ts_js_t *j, void *ctx)
{
    derp_state_t *st = (derp_state_t *)ctx;
    /* Region member key (the numeric ID as a string) — value is an object. */
    if (ts_js_next(j) != TS_JS_OBJ_START) return true;

    region_state_t r = { .region_id = INT32_MAX };
    if (!walk_object(j, region_key_handler, &r)) return true;

    if (r.have_node0 && r.region_id < st->best_id && r.node0.hostname[0]) {
        st->best_id   = r.region_id;
        st->best_node = r.node0;
        st->best_node.region_id = r.region_id;
        st->found     = true;
    }
    return true;
}

static bool derpmap_key_handler(ts_js_t *j, void *ctx)
{
    derp_state_t *st = (derp_state_t *)ctx;
    if (!ts_js_str_eq(j, "Regions")) return false;
    if (ts_js_next(j) != TS_JS_OBJ_START) return true;
    walk_object(j, regions_key_handler, st);
    return true;
}

static void parse_derpmap_stream(ts_js_t *j)
{
    if (ts_js_next(j) != TS_JS_OBJ_START) return;
    derp_state_t st = { .best_id = INT32_MAX };
    walk_object(j, derpmap_key_handler, &st);
    if (st.found) {
        ESP_LOGI(TAG, "DERP home server: %s:%d (region %d)",
                 st.best_node.hostname, st.best_node.derp_port,
                 st.best_node.region_id);
        s_derp_home       = st.best_node;
        s_derp_home_valid = true;
    }
}

/* ------------------------------------------------------------------ */
/* Peer array — full snapshot ("Peers") or incremental ("PeersChanged") */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t  node_id;
    bool     have_node_pub;
    uint8_t  node_pub[32];
    uint8_t  disco_pub[32];
    char     ts_ip[20];
    char     name[64];
    ip_addr_t ep_ip;
    uint16_t  ep_port;
    bool      have_direct_ep;
    int       derp_region;     /* >0 when a DERP pseudo endpoint was advertised */
} peer_parse_t;

static bool peer_key_handler(ts_js_t *j, void *ctx)
{
    peer_parse_t *p = (peer_parse_t *)ctx;

    if (ts_js_str_eq(j, "Key")) {
        char hex[80];
        if (read_string_value(j, hex, sizeof(hex)) &&
            ts_key_from_hex(hex, p->node_pub)) {
            p->have_node_pub = true;
        }
        return true;
    }
    if (ts_js_str_eq(j, "ID")) {
        if (ts_js_next(j) != TS_JS_NUMBER) return true;
        p->node_id = ts_js_int64(j);
        return true;
    }
    if (ts_js_str_eq(j, "DiscoKey")) {
        char hex[80];
        if (read_string_value(j, hex, sizeof(hex)))
            ts_key_from_hex(hex, p->disco_pub);
        return true;
    }
    if (ts_js_str_eq(j, "Addresses")) {
        if (ts_js_next(j) != TS_JS_ARR_START) return true;
        ts_js_evt_t e = ts_js_next(j);
        if (e == TS_JS_STRING) {
            char a[40];
            ts_js_str_copy(j, a, sizeof(a));
            strip_prefix(a, p->ts_ip, sizeof(p->ts_ip));
        }
        while ((e = ts_js_next(j)) != TS_JS_ARR_END &&
               e != TS_JS_END && e != TS_JS_ERROR) {}
        return true;
    }
    if (ts_js_str_eq(j, "Endpoints")) {
        if (ts_js_next(j) != TS_JS_ARR_START) return true;
        ts_js_evt_t e = ts_js_next(j);
        if (e == TS_JS_STRING && !p->have_direct_ep) {
            char ep[64];
            ts_js_str_copy(j, ep, sizeof(ep));
            if (parse_endpoint(ep, &p->ep_ip, &p->ep_port))
                p->have_direct_ep = true;
        }
        while ((e = ts_js_next(j)) != TS_JS_ARR_END &&
               e != TS_JS_END && e != TS_JS_ERROR) {}
        return true;
    }
    if (ts_js_str_eq(j, "DERP")) {
        char d[40];
        if (read_string_value(j, d, sizeof(d))) {
            const char *colon = strrchr(d, ':');
            if (colon) p->derp_region = atoi(colon + 1);
        }
        return true;
    }
    if (ts_js_str_eq(j, "Name")) {
        char n[80];
        if (read_string_value(j, n, sizeof(n))) {
            // Trailing dot is conventional in FQDN form; trim for usability.
            size_t l = strnlen(n, sizeof(n));
            while (l > 0 && n[l - 1] == '.') n[--l] = '\0';
            strlcpy(p->name, n, sizeof(p->name));
        }
        return true;
    }
    return false;
}

static void apply_one_peer(const peer_parse_t *p, bool *keep)
{
    if (!p->have_node_pub) return;

    ip_addr_t ep_ip = p->ep_ip;
    uint16_t  ep_port = p->ep_port;
    bool      have_direct_ep = p->have_direct_ep;

#if CONFIG_TAILSCALE_DERP_ONLY
    /* DERP-only mode: never use the advertised direct endpoint. The
     * direct UDP path frequently fails on simplified clients (NAT
     * hole-punching needs DISCO, hairpin on the same LAN, etc.), and
     * there is no automatic fallback once an endpoint is fixed. Routing
     * everything through the home DERP relay is slower but reliable —
     * the relay reaches the peer by its public key regardless of region. */
    have_direct_ep = false;
#endif

    /* If no direct endpoint, fall back to DERP pseudo-IP 127.3.3.40:<region>. */
    if (!have_direct_ep && p->derp_region > 0) {
        ip4_addr_t derp4;
        ip4addr_aton("127.3.3.40", &derp4);
        ip_addr_copy_from_ip4(ep_ip, derp4);
        ep_port = (uint16_t)p->derp_region;
    }

    char pub_b64[64];
    size_t b64_len = sizeof(pub_b64);
    extern bool wireguard_base64_encode(const uint8_t *, size_t, char *, size_t *);
    wireguard_base64_encode(p->node_pub, 32, pub_b64, &b64_len);

    int slot = find_peer_slot(p->node_pub);
    if (slot >= 0) {
        keep[slot] = true;
        s_peers[slot].node_id = p->node_id;
        if (p->name[0]) strlcpy(s_peers[slot].name, p->name,
                                sizeof(s_peers[slot].name));
        if (!ip_addr_isany(&ep_ip) && ep_port != 0) {
            wireguard_esp32_update_endpoint(s_peers[slot].wg_index,
                                            &ep_ip, ep_port);
        }
        return;
    }

    slot = alloc_peer_slot();
    if (slot < 0) {
        ESP_LOGW(TAG, "Peer table full, dropping peer %s", p->ts_ip);
        return;
    }

    ip4_addr_t allowed4, mask4;
    if (p->ts_ip[0] && ip4addr_aton(p->ts_ip, &allowed4)) {
        ip4addr_aton("255.255.255.255", &mask4);
    } else {
        ip4_addr_set_zero(&allowed4);
        ip4_addr_set_zero(&mask4);
    }
    ip_addr_t allowed_ip, allowed_mask;
    ip_addr_copy_from_ip4(allowed_ip, allowed4);
    ip_addr_copy_from_ip4(allowed_mask, mask4);

    uint8_t wg_idx;
    esp_err_t err = wireguard_esp32_add_peer(
        pub_b64,
        &allowed_ip,
        &allowed_mask,
        ip_addr_isany(&ep_ip) ? NULL : &ep_ip,
        ep_port,
        25,
        &wg_idx);

    if (err != ESP_OK) return;

    memcpy(s_peers[slot].node_pub,  p->node_pub,  32);
    memcpy(s_peers[slot].disco_pub, p->disco_pub, 32);
    strlcpy(s_peers[slot].ts_ip, p->ts_ip, sizeof(s_peers[slot].ts_ip));
    strlcpy(s_peers[slot].name,  p->name,  sizeof(s_peers[slot].name));
    s_peers[slot].derp_region = p->derp_region;
    s_peers[slot].wg_index = wg_idx;
    s_peers[slot].node_id  = p->node_id;
    s_peers[slot].active   = true;
    keep[slot] = true;
    ESP_LOGI(TAG, "Added peer %s id=%lld (wg_idx=%d, direct=%d)",
             p->ts_ip, (long long)p->node_id, wg_idx, have_direct_ep);

    if (have_direct_ep) {
        bool disco_set = false;
        for (int b = 0; b < 32; b++)
            if (p->disco_pub[b]) { disco_set = true; break; }
        if (disco_set) {
            ts_disco_ping(wg_idx, p->disco_pub, &ep_ip, ep_port);
        }
    }
}

static void parse_peer_array_stream(ts_js_t *j, bool full_snapshot)
{
    if (ts_js_next(j) != TS_JS_ARR_START) return;

    bool keep[TS_NETMAP_MAX_PEERS] = {false};
    int  count = 0;

    while (1) {
        ts_js_evt_t e = ts_js_next(j);
        if (e == TS_JS_ARR_END) break;
        if (e != TS_JS_OBJ_START) {
            /* Unexpected — skip remainder. */
            ts_js_skip(j);
            continue;
        }

        peer_parse_t p = {0};
        ip4_addr_t zero;
        ip4_addr_set_zero(&zero);
        ip_addr_copy_from_ip4(p.ep_ip, zero);

        if (!walk_object(j, peer_key_handler, &p)) break;
        apply_one_peer(&p, keep);
        count++;
    }

    ESP_LOGI(TAG, "Peers (%s): %d processed",
             full_snapshot ? "full" : "changed", count);
    // Snapshot the current peer table so users can verify which names
    // resolve to which IPs over Tailscale.
    int active = 0;
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (!s_peers[i].active) continue;
        ESP_LOGI(TAG, "  peer[%d] wg=%d derp=%d ip=%-15s name=%s",
                 i, s_peers[i].wg_index, s_peers[i].derp_region,
                 s_peers[i].ts_ip[0] ? s_peers[i].ts_ip : "(none)",
                 s_peers[i].name[0]  ? s_peers[i].name  : "(no-name)");
        active++;
    }
    ESP_LOGI(TAG, "  total active peers: %d / %d", active, TS_NETMAP_MAX_PEERS);

    if (full_snapshot) {
        for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
            if (s_peers[i].active && !keep[i]) {
                ESP_LOGI(TAG, "Removing peer %s (wg_idx=%d)",
                         s_peers[i].ts_ip, s_peers[i].wg_index);
                wireguard_esp32_remove_peer(s_peers[i].wg_index);
                s_peers[i].active = false;
            }
        }
    }
}

static void parse_peers_removed_stream(ts_js_t *j)
{
    if (ts_js_next(j) != TS_JS_ARR_START) return;
    while (1) {
        ts_js_evt_t e = ts_js_next(j);
        if (e == TS_JS_ARR_END) break;
        if (e != TS_JS_NUMBER) continue;
        int64_t node_id = ts_js_int64(j);
        int slot = find_peer_slot_by_id(node_id);
        if (slot >= 0) {
            ESP_LOGI(TAG, "Removing peer %s (id=%lld, wg_idx=%d)",
                     s_peers[slot].ts_ip, (long long)node_id,
                     s_peers[slot].wg_index);
            wireguard_esp32_remove_peer(s_peers[slot].wg_index);
            s_peers[slot].active = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Top-level dispatcher                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    bool keep_alive;
} root_state_t;

static bool root_key_handler(ts_js_t *j, void *ctx)
{
    root_state_t *st = (root_state_t *)ctx;
    if (ts_js_str_eq(j, "Node")) {
        parse_node_obj(j);
        return true;
    }
    if (ts_js_str_eq(j, "DERPMap")) {
        parse_derpmap_stream(j);
        return true;
    }
    if (ts_js_str_eq(j, "Peers")) {
        parse_peer_array_stream(j, true);
        return true;
    }
    if (ts_js_str_eq(j, "PeersChanged")) {
        parse_peer_array_stream(j, false);
        return true;
    }
    if (ts_js_str_eq(j, "PeersRemoved")) {
        parse_peers_removed_stream(j);
        return true;
    }
    if (ts_js_str_eq(j, "PeersChangedPatch")) {
        ESP_LOGW(TAG, "PeersChangedPatch present — not implemented, skipping");
        return false;   /* fall through to ts_js_skip */
    }
    if (ts_js_str_eq(j, "KeepAlive")) {
        if (ts_js_next(j) == TS_JS_BOOL && j->bool_value) {
            st->keep_alive = true;
        }
        return true;
    }
    return false;   /* unknown key — caller will skip the value */
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool ts_netmap_get_derp_home(ts_derp_node_t *node_out)
{
    if (!s_derp_home_valid) return false;
    *node_out = s_derp_home;
    return true;
}

esp_err_t ts_netmap_apply(const char *json_str, size_t json_len)
{
    ESP_LOGD(TAG, "ts_netmap_apply: %u B (heap_free=%u, largest=%u)",
             (unsigned)json_len,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    ts_js_t j;
    ts_js_init(&j, json_str, json_len);

    if (ts_js_next(&j) != TS_JS_OBJ_START) {
        ESP_LOGE(TAG, "MapResponse: not a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    root_state_t st = {0};
    if (!walk_object(&j, root_key_handler, &st)) {
        ESP_LOGE(TAG, "MapResponse: stream parse error");
        return ESP_ERR_INVALID_ARG;
    }

    if (st.keep_alive) {
        ESP_LOGD(TAG, "MapResponse: KeepAlive");
    }
    return ESP_OK;
}

void ts_netmap_get_self_ip(char *ip_str, size_t ip_str_len)
{
    strlcpy(ip_str, s_self_ip, ip_str_len);
}

// Case-insensitive match of `name` against either the full peer Name
// (e.g. "host.tail-xxxxx.ts.net") or its short form (up to the first
// '.'). Returns true on first hit.
static bool peer_name_matches(const char *peer_name, const char *query)
{
    if (!peer_name || !peer_name[0] || !query || !query[0]) return false;
    // Full match (case-insensitive)
    if (strcasecmp(peer_name, query) == 0) return true;
    // Short match: trim peer_name at first '.'.
    const char *dot = strchr(peer_name, '.');
    if (!dot) return false;
    size_t shortlen = (size_t)(dot - peer_name);
    if (strncasecmp(peer_name, query, shortlen) == 0 &&
        query[shortlen] == '\0') {
        return true;
    }
    return false;
}

bool ts_netmap_resolve(const char *name, char *ip_str, size_t ip_str_len)
{
    if (!name || !ip_str || ip_str_len == 0) return false;
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (!s_peers[i].active) continue;
        if (!s_peers[i].ts_ip[0]) continue;
        if (peer_name_matches(s_peers[i].name, name)) {
            strlcpy(ip_str, s_peers[i].ts_ip, ip_str_len);
            return true;
        }
    }
    // Also recognise "self" / our own hostname → 100.x assigned to us.
    // (Useful for ssh-ing back to the Tab5 itself for testing.)
    return false;
}
