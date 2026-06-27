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
#include "lwip/netif.h"

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

/* Full DERP region table (region_id -> node). Needed so the DERP client
 * can open a connection to *any* peer's home region, not just ours. */
#define TS_DERP_MAX_REGIONS 32
static ts_derp_node_t s_derp_regions[TS_DERP_MAX_REGIONS];
static int            s_derp_region_count = 0;

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
    // Record every region into the full table so we can connect to a
    // peer's home region on demand (multi-region DERP).
    if (r.have_node0 && r.node0.hostname[0] &&
        s_derp_region_count < TS_DERP_MAX_REGIONS) {
        ts_derp_node_t *slot = NULL;
        for (int i = 0; i < s_derp_region_count; i++) {
            if (s_derp_regions[i].region_id == r.region_id) {
                slot = &s_derp_regions[i];
                break;
            }
        }
        if (!slot) slot = &s_derp_regions[s_derp_region_count++];
        *slot = r.node0;
        slot->region_id = r.region_id;
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
    s_derp_region_count = 0;   // rebuild the region table from this DERPMap
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

/* Top-N direct-UDP candidates we'll DISCO-probe for each peer. Sized
 * for the typical Tailscale advertise (LAN + Docker-bridge IPs + WAN);
 * 4 is enough to cover one LAN candidate + one or two CGNAT/RFC1918
 * + one public, which is what mobile-network / external-LAN cases need
 * to find any reachable path. */
/* Big enough to retain a peer's typical advertisement: one same-LAN
 * RFC1918 + a couple of Docker-bridge RFC1918 + a CGNAT + a public WAN
 * (the last one is the only thing that works from external networks
 * like phone tethering and is the lowest-scored — without keeping it
 * around we'd silently drop the only reachable path). */
#define TS_NETMAP_MAX_CANDIDATES 8

typedef struct {
    ip_addr_t ip;
    uint16_t  port;
    int       score;     /* same encoding as `score` below in the parser */
} ep_candidate_t;

typedef struct {
    int64_t  node_id;
    bool     have_node_pub;
    uint8_t  node_pub[32];
    uint8_t  disco_pub[32];
    char     ts_ip[20];
    char     name[64];
    /* Best single direct endpoint (kept for backward compatibility with
     * the existing WG add/update path that takes one address). */
    ip_addr_t ep_ip;
    uint16_t  ep_port;
    bool      have_direct_ep;
    /* Top-N candidates sorted by descending score for multi-probe DISCO.
     * Populated together with ep_ip / ep_port. */
    ep_candidate_t cands[TS_NETMAP_MAX_CANDIDATES];
    int       cand_count;
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
        // Score each candidate; keep the top N (descending) for multi-
        // probe DISCO. Without multi-probe, a peer on an external
        // network would have to guess the single right endpoint from
        // the peer's full enumeration (LAN IPs, Docker bridges, CGNAT,
        // VPN tunnels, public WAN), which is unreliable. Probing the
        // top few in parallel via cheap DISCO Pings lets the first
        // Pong-back win.
        //
        // Scoring:
        //   +30 same /24 as our Wi-Fi STA IP   (very likely directly reachable)
        //   +20 same /16
        //   +10 same /8
        //   +5  RFC1918 / link-local           (LAN-ish, could be reachable)
        //   +1  CGNAT 100.64.0.0/10
        //   +1  anything else (public WAN, etc — needs router port-forward
        //       or hairpin but is the only choice from external networks
        //       like phone tethering)
        uint32_t self_ip_h = 0;
        for (struct netif *nif = netif_list; nif; nif = nif->next) {
            if (nif->name[0] == 'w' && nif->name[1] == 'g') continue;
            if (!ip4_addr_isany_val(*netif_ip4_addr(nif))) {
                self_ip_h = ntohl(netif_ip4_addr(nif)->addr);
                break;
            }
        }
        ts_js_evt_t e;
        while ((e = ts_js_next(j)) != TS_JS_ARR_END &&
               e != TS_JS_END && e != TS_JS_ERROR) {
            if (e != TS_JS_STRING) continue;
            char ep[64];
            ts_js_str_copy(j, ep, sizeof(ep));
            ip_addr_t  tmp_ip;
            uint16_t   tmp_port;
            if (!parse_endpoint(ep, &tmp_ip, &tmp_port)) {
                ESP_LOGI(TAG, "  endpoint candidate (unparseable) %s", ep);
                continue;
            }
            const ip4_addr_t *ip4 = ip_2_ip4(&tmp_ip);
            if (!ip4) continue;
            uint32_t a = ntohl(ip4->addr);
            int score = 1;   /* base: public WAN still gets probed */
            const bool is_rfc1918 =
                (a & 0xFF000000) == 0x0A000000 ||
                (a & 0xFFF00000) == 0xAC100000 ||
                (a & 0xFFFF0000) == 0xC0A80000 ||
                (a & 0xFFFF0000) == 0xA9FE0000;
            if (is_rfc1918)                       score += 4;  /* total 5 */
            else if ((a & 0xFFC00000) == 0x64400000) score += 0; /* CGNAT, 1 */
            if (self_ip_h) {
                if ((a & 0xFFFFFF00) == (self_ip_h & 0xFFFFFF00)) score += 30;
                else if ((a & 0xFFFF0000) == (self_ip_h & 0xFFFF0000)) score += 20;
                else if ((a & 0xFF000000) == (self_ip_h & 0xFF000000)) score += 10;
            }
            ESP_LOGI(TAG, "  endpoint candidate s%d %s", score, ep);

            /* Insertion-sort into cands[] (descending score). Cap at
             * TS_NETMAP_MAX_CANDIDATES; drop the worst when full. */
            int slot = p->cand_count;
            if (slot >= TS_NETMAP_MAX_CANDIDATES) {
                if (score <= p->cands[TS_NETMAP_MAX_CANDIDATES - 1].score) continue;
                slot = TS_NETMAP_MAX_CANDIDATES - 1;
            } else {
                p->cand_count++;
            }
            while (slot > 0 && p->cands[slot - 1].score < score) {
                p->cands[slot] = p->cands[slot - 1];
                slot--;
            }
            p->cands[slot].ip    = tmp_ip;
            p->cands[slot].port  = tmp_port;
            p->cands[slot].score = score;
        }

        if (p->cand_count > 0) {
            p->ep_ip          = p->cands[0].ip;
            p->ep_port        = p->cands[0].port;
            p->have_direct_ep = true;
            ESP_LOGI(TAG, "  endpoint TOP%d (best s%d)",
                     p->cand_count, p->cands[0].score);
        }
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
    /* DERP-only mode: never use the advertised direct endpoint. */
    have_direct_ep = false;
#else
    /* Always initialise the WG peer's endpoint to its DERP pseudo
     * address when a region is known, even if direct candidates are
     * available. The direct path is then PROMOTED by ts_disco_rx() the
     * moment a DISCO Pong verifies a reachable candidate (see
     * wireguard_esp32_update_endpoint() call there). This mirrors
     * Tailscale magicsock: traffic starts on DERP so connectivity is
     * never gated on direct UDP succeeding, and switches to direct on
     * first verified Pong. Without this, a peer whose advertised
     * direct candidates are all unreachable from our subnet (typical
     * for phone-tethering / external-network cases) leaves WireGuard
     * stuck trying to handshake against a dead address — `EHOSTUNREACH`
     * on the upper-layer connect. */
    have_direct_ep = false;
#endif

    /* DERP fallback for the WG endpoint. With have_direct_ep forced
     * false above this is now the only path that sets ep_ip/ep_port,
     * and it covers both the no-candidate case and the
     * "wait-for-DISCO-promotion" case. */
    if (!have_direct_ep && p->derp_region > 0) {
        ip4_addr_t derp4;
        ip4addr_aton("127.3.3.40", &derp4);
        ip_addr_copy_from_ip4(ep_ip, derp4);
        ep_port = (uint16_t)p->derp_region;
        have_direct_ep = true;  /* feed the add/update path below */
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
        /* Retry CallMeMaybe — endpoints may have become available since
         * we first added this peer (STUN / pubip completes after netmap).
         * Rate-limit inside the sender prevents flooding. */
        bool disco_set = false;
        for (int b = 0; b < 32; b++)
            if (p->disco_pub[b]) { disco_set = true; break; }
        if (disco_set && p->derp_region > 0) {
            ts_disco_send_call_me_maybe(s_peers[slot].wg_index,
                                         p->node_pub, p->disco_pub,
                                         (uint8_t)p->derp_region);
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
    ESP_LOGI(TAG, "Added peer %s id=%lld (wg_idx=%d, cands=%d, derp=%d)",
             p->ts_ip, (long long)p->node_id, wg_idx,
             p->cand_count, p->derp_region);

    /* DISCO probe — independent of which endpoint WG currently uses.
     * WG starts on DERP (set above); DISCO promotes to a direct address
     * via ts_disco_rx() -> wireguard_esp32_update_endpoint() as soon as
     * any candidate's Pong arrives. ts_disco_rx() ignores subsequent
     * Pongs for the same peer, so multi-probe races don't toggle the
     * endpoint back to a slower-but-also-reachable path. */
    bool disco_set = false;
    for (int b = 0; b < 32; b++)
        if (p->disco_pub[b]) { disco_set = true; break; }
    if (disco_set && p->cand_count > 0) {
        for (int i = 0; i < p->cand_count; i++) {
            ts_disco_send_ping(wg_idx, p->node_pub, p->disco_pub,
                               &p->cands[i].ip, p->cands[i].port);
        }
    }

    /* Also send CallMeMaybe via DERP, advertising OUR endpoints, so the
     * peer can ping back at us. This is what unsticks peers behind hostile
     * NATs whose advertised endpoints we can never reach directly: once
     * they DISCO-Ping us at our published endpoint, our Pong responder
     * promotes the path on their side, NAT pinhole opens, and our own
     * outbound DISCO Pings (above) finally get a Pong back.
     * Rate-limited inside ts_disco_send_call_me_maybe; verified peers
     * skipped; silently skipped when we have no endpoints yet. */
    if (disco_set && p->derp_region > 0) {
        ts_disco_send_call_me_maybe(wg_idx, p->node_pub, p->disco_pub,
                                     (uint8_t)p->derp_region);
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

bool ts_netmap_get_derp_region(int region_id, ts_derp_node_t *out)
{
    if (!out || region_id <= 0) return false;
    for (int i = 0; i < s_derp_region_count; i++) {
        if (s_derp_regions[i].region_id == region_id) {
            *out = s_derp_regions[i];
            return true;
        }
    }
    return false;
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

bool ts_netmap_lookup_by_disco(const uint8_t disco_pub[32],
                                uint8_t *wg_idx_out,
                                uint8_t  node_pub_out[32])
{
    if (!disco_pub) return false;
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (!s_peers[i].active) continue;
        if (memcmp(s_peers[i].disco_pub, disco_pub, 32) != 0) continue;
        if (wg_idx_out)   *wg_idx_out = s_peers[i].wg_index;
        if (node_pub_out) memcpy(node_pub_out, s_peers[i].node_pub, 32);
        return true;
    }
    return false;
}
