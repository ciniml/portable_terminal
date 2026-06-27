/*
 * Tailscale ts2021 control-plane client — device registration and MapRequest.
 *
 * Protocol stack (bottom to top):
 *   TLS (mbedTLS) → HTTP/1.1 upgrade → Noise IK handshake → HTTP/2 → JSON API
 *
 * After the Noise handshake the control channel speaks HTTP/2 over the Noise
 * transport.  Each send wraps one HTTP/2 frame in a Noise record; receives
 * buffer one Noise record at a time and feed an HTTP/2 frame parser.
 *
 * Map response uses streaming [4-byte LE length][data] body segments.
 * Register response is plain JSON in the HTTP/2 DATA frame body.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_control.h"
#include "tailscale_noise.h"
#include "tailscale_netmap.h"
#include "tailscale_derp.h"
#include "tailscale_keys.h"

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wireguard.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "ts_ctrl";

#define TS_CAP_VERSION     "47"
#define CTRL_FRAME_BUF     4096          /* max map-segment / register body */
#define H2_MAX_PAYLOAD     4096          /* max HTTP/2 frame payload we handle */
#define TS_MSG_TYPE_DATA   0x04          /* Noise record type for data */

/* ------------------------------------------------------------------ */
/* TLS helpers                                                          */
/* ------------------------------------------------------------------ */

static esp_tls_t *s_tls  = NULL;
static bool       s_stop = false;

/* When the control server responds with a non-empty `AuthURL` the device
 * must be approved interactively at that URL before traffic can flow.
 * Saved here so the Web UI / public API can surface it. Empty string when
 * the device is approved (or no registration has been performed yet). */
#define TS_AUTH_URL_MAX 192
static char s_auth_url[TS_AUTH_URL_MAX];

const char *ts_ctrl_get_auth_url(void)
{
    return s_auth_url;
}

/* Endpoints published in MapRequest.Endpoints. Populated by the pubip
 * probe via ts_ctrl_set_endpoints(); re-sent on the next poll-loop
 * iteration when s_endpoints_dirty is set. The dirty flag is a plain
 * volatile bool — single-byte loads/stores need no mutex. */
static char          s_endpoints[CTRL_MAX_ENDPOINTS][CTRL_MAX_EP_LEN];
static int           s_endpoint_count = 0;
static volatile bool s_endpoints_dirty = false;

void ts_ctrl_set_endpoints(const char *const *eps, int n)
{
    if (n < 0) n = 0;
    if (n > CTRL_MAX_ENDPOINTS) n = CTRL_MAX_ENDPOINTS;
    for (int i = 0; i < n; i++) {
        if (eps && eps[i]) {
            strlcpy(s_endpoints[i], eps[i], CTRL_MAX_EP_LEN);
        } else {
            s_endpoints[i][0] = '\0';
        }
    }
    s_endpoint_count = n;
}

void ts_ctrl_signal_endpoints_dirty(void)
{
    s_endpoints_dirty = true;
}

int ts_ctrl_get_endpoints(char out[][CTRL_MAX_EP_LEN], int max)
{
    if (!out || max <= 0) return 0;
    int n = s_endpoint_count;
    if (n > max) n = max;
    if (n > CTRL_MAX_ENDPOINTS) n = CTRL_MAX_ENDPOINTS;
    for (int i = 0; i < n; i++) {
        strlcpy(out[i], s_endpoints[i], CTRL_MAX_EP_LEN);
    }
    return n;
}

static int tls_write(const uint8_t *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        if (s_stop) return -1;
        int n = esp_tls_conn_write(s_tls, buf + written, len - written);
        if (n == -0x6900 || n == -0x6880) continue;   /* WANT_READ/WANT_WRITE */
        if (n < 0) { ESP_LOGE(TAG, "TLS write error: %d", n); return -1; }
        written += (size_t)n;
    }
    return (int)written;
}

/* mbedTLS transient codes — caller should retry, not abort */
#define MB_WANT_READ   -0x6900
#define MB_WANT_WRITE  -0x6880

static int tls_read(uint8_t *buf, size_t len)
{
    size_t rd = 0;
    while (rd < len) {
        if (s_stop) return -1;
        int n = esp_tls_conn_read(s_tls, buf + rd, len - rd);
        if (n == MB_WANT_READ || n == MB_WANT_WRITE) continue;
        if (n < 0)  { ESP_LOGE(TAG, "TLS read error: %d", n); return -1; }
        if (n == 0) { ESP_LOGW(TAG, "TLS peer closed"); return (int)rd; }
        rd += (size_t)n;
    }
    return (int)rd;
}

/* ------------------------------------------------------------------ */
/* HTTP/2 / Noise byte-stream reader                                    */
/*                                                                      */
/* s_h2rx buffers one decrypted Noise frame.  h2_read() feeds byte    */
/* requests from it, refilling by reading the next Noise frame when   */
/* the buffer is exhausted.                                            */
/* ------------------------------------------------------------------ */

static EXT_RAM_BSS_ATTR struct {
    uint8_t  enc_buf[H2_MAX_PAYLOAD + 16]; /* ciphertext from TLS   */
    uint8_t  pt_buf[H2_MAX_PAYLOAD];       /* decrypted plaintext   */
    size_t   pt_len;
    size_t   pt_pos;
} s_h2rx;

static esp_err_t h2_fill(ts_ctrl_ctx_t *ctx)
{
    uint8_t  nhdr[3];
    if (tls_read(nhdr, 3) != 3) return ESP_FAIL;

    uint8_t  ftype  = nhdr[0];
    uint32_t enc_len = ((uint32_t)nhdr[1] << 8) | nhdr[2];
    if (ftype != TS_MSG_TYPE_DATA)
        ESP_LOGW(TAG, "H2: unexpected Noise type 0x%02x", ftype);
    if (enc_len < 16 || enc_len > sizeof(s_h2rx.enc_buf)) {
        ESP_LOGE(TAG, "H2: enc_len %"PRIu32" out of range", enc_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (tls_read(s_h2rx.enc_buf, enc_len) != (int)enc_len) return ESP_FAIL;

    size_t plen = 0;
    esp_err_t err = ts_noise_decrypt(&ctx->noise, s_h2rx.enc_buf, enc_len,
                                      s_h2rx.pt_buf, &plen);
    if (err != ESP_OK) return err;
    s_h2rx.pt_len = plen;
    s_h2rx.pt_pos = 0;
    return ESP_OK;
}

/* Read exactly len bytes from the H2 byte stream (buf may be NULL to drain) */
static int h2_read(ts_ctrl_ctx_t *ctx, uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        if (s_h2rx.pt_pos >= s_h2rx.pt_len) {
            if (h2_fill(ctx) != ESP_OK) return -1;
        }
        size_t avail = s_h2rx.pt_len - s_h2rx.pt_pos;
        size_t n     = avail < (len - total) ? avail : (len - total);
        if (buf) memcpy(buf + total, s_h2rx.pt_buf + s_h2rx.pt_pos, n);
        s_h2rx.pt_pos += n;
        total         += n;
    }
    return (int)total;
}

/* ------------------------------------------------------------------ */
/* HTTP/2 frame I/O                                                     */
/* ------------------------------------------------------------------ */

#define H2_FRAME_DATA         0x0
#define H2_FRAME_HEADERS      0x1
#define H2_FRAME_SETTINGS     0x4
#define H2_FRAME_PING         0x6
#define H2_FRAME_GOAWAY       0x7
#define H2_FRAME_WINDOW_UPD   0x8
#define H2_FRAME_CONTINUATION 0x9

#define H2_FLAG_END_STREAM  0x01
#define H2_FLAG_END_HEADERS 0x04
#define H2_FLAG_ACK         0x01
#define H2_FLAG_PADDED      0x08
#define H2_FLAG_PRIORITY    0x20

/* Read one HTTP/2 frame.  payload_buf must be >= H2_MAX_PAYLOAD bytes. */
static esp_err_t h2_read_frame(ts_ctrl_ctx_t *ctx,
                                uint8_t  *type_out,  uint8_t  *flags_out,
                                uint32_t *sid_out,
                                uint8_t  *payload_buf, uint32_t *plen_out)
{
    uint8_t fhdr[9];
    if (h2_read(ctx, fhdr, 9) != 9) return ESP_FAIL;

    uint32_t flen  = ((uint32_t)fhdr[0] << 16) | ((uint32_t)fhdr[1] << 8) | fhdr[2];
    *type_out  = fhdr[3];
    *flags_out = fhdr[4];
    *sid_out   = ((uint32_t)(fhdr[5] & 0x7F) << 24) | ((uint32_t)fhdr[6] << 16) |
                 ((uint32_t)fhdr[7] << 8) | fhdr[8];

    ESP_LOGD(TAG, "H2 rx: type=0x%02x flags=0x%02x sid=%"PRIu32" len=%"PRIu32,
             *type_out, *flags_out, *sid_out, flen);

    if (flen > H2_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "H2: frame too large %"PRIu32, flen);
        h2_read(ctx, NULL, flen);   /* drain to keep stream in sync */
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (flen > 0 && h2_read(ctx, payload_buf, flen) != (int)flen) return ESP_FAIL;
    *plen_out = flen;
    return ESP_OK;
}

/* Send one HTTP/2 frame as a single Noise record */
static esp_err_t h2_send_frame(ts_ctrl_ctx_t *ctx,
                                uint8_t type, uint8_t flags, uint32_t sid,
                                const uint8_t *payload, uint32_t plen)
{
    static EXT_RAM_BSS_ATTR uint8_t frame_buf[9 + H2_MAX_PAYLOAD];
    frame_buf[0] = (plen >> 16) & 0xFF;
    frame_buf[1] = (plen >>  8) & 0xFF;
    frame_buf[2] =  plen        & 0xFF;
    frame_buf[3] = type;
    frame_buf[4] = flags;
    frame_buf[5] = (sid >> 24) & 0x7F;
    frame_buf[6] = (sid >> 16) & 0xFF;
    frame_buf[7] = (sid >>  8) & 0xFF;
    frame_buf[8] =  sid        & 0xFF;
    if (payload && plen) memcpy(frame_buf + 9, payload, plen);

    uint32_t total   = 9 + plen;
    uint32_t enc_len = total + 16;

    static EXT_RAM_BSS_ATTR uint8_t enc_buf[9 + H2_MAX_PAYLOAD + 16];
    esp_err_t err = ts_noise_encrypt(&ctx->noise, frame_buf, total, enc_buf);
    if (err != ESP_OK) return err;

    uint8_t nhdr[3] = {
        TS_MSG_TYPE_DATA,
        (uint8_t)((enc_len >> 8) & 0xFF),
        (uint8_t)( enc_len       & 0xFF)
    };
    if (tls_write(nhdr, 3) != 3) return ESP_FAIL;
    return (tls_write(enc_buf, enc_len) == (int)enc_len) ? ESP_OK : ESP_FAIL;
}

/* Convenience: handle a received SETTINGS, PING, WINDOW_UPDATE frame.
 * Returns true if the frame was handled (caller should continue the loop). */
static bool h2_handle_ctrl_frame(ts_ctrl_ctx_t *ctx,
                                  uint8_t ftype, uint8_t fflags,
                                  const uint8_t *payload, uint32_t plen)
{
    if (ftype == H2_FRAME_SETTINGS) {
        if (!(fflags & H2_FLAG_ACK))
            h2_send_frame(ctx, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
        return true;
    }
    if (ftype == H2_FRAME_WINDOW_UPD) return true;
    if (ftype == H2_FRAME_PING) {
        h2_send_frame(ctx, H2_FRAME_PING, H2_FLAG_ACK, 0, payload, (plen >= 8 ? 8 : plen));
        return true;
    }
    if (ftype == H2_FRAME_GOAWAY) {
        ESP_LOGE(TAG, "H2: GOAWAY received");
        return true;  /* caller checks for errors differently */
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Minimal HPACK encoder (literal, no Huffman)                          */
/*                                                                      */
/* HPACK static table indices used:                                     */
/*   1  :authority        3  :method POST   4  :path /                 */
/*   7  :scheme https     8  :status 200   28  content-length          */
/*  31  content-type                                                    */
/* ------------------------------------------------------------------ */

static int hpack_str(uint8_t *buf, int pos, int max, const char *s, int slen)
{
    if (slen < 0) slen = (int)strlen(s);
    if (pos + 1 + slen > max) return -1;
    buf[pos++] = (uint8_t)slen;  /* H=0, length */
    memcpy(buf + pos, s, (size_t)slen);
    return pos + slen;
}

static int h2_encode_req_headers(const char *path, const char *host,
                                  uint32_t body_len,
                                  uint8_t *buf, int buf_max)
{
    int pos = 0;

    if (pos + 1 > buf_max) return -1;
    buf[pos++] = 0x83;  /* indexed: :method POST (static[3]) */

    if (pos + 1 > buf_max) return -1;
    buf[pos++] = 0x87;  /* indexed: :scheme https (static[7]) */

    if (pos + 1 > buf_max) return -1;
    buf[pos++] = 0x44;  /* literal incr, name=static[4](:path) */
    if ((pos = hpack_str(buf, pos, buf_max, path, -1)) < 0) return -1;

    if (pos + 1 > buf_max) return -1;
    buf[pos++] = 0x41;  /* literal incr, name=static[1](:authority) */
    if ((pos = hpack_str(buf, pos, buf_max, host, -1)) < 0) return -1;

    if (pos + 1 > buf_max) return -1;
    buf[pos++] = 0x5f;  /* literal incr, name=static[31](content-type) */
    if ((pos = hpack_str(buf, pos, buf_max, "application/json", 16)) < 0) return -1;

    if (pos + 1 > buf_max) return -1;
    buf[pos++] = 0x5c;  /* literal incr, name=static[28](content-length) */
    char cl[12];
    int cl_len = snprintf(cl, sizeof(cl), "%"PRIu32, body_len);
    if ((pos = hpack_str(buf, pos, buf_max, cl, cl_len)) < 0) return -1;

    return pos;
}

/* ------------------------------------------------------------------ */
/* Minimal HPACK decoder — extract :status                              */
/* ------------------------------------------------------------------ */

static int hpack_skip_str(const uint8_t *h, int pos, int hlen)
{
    if (pos >= hlen) return -1;
    int n = h[pos++] & 0x7F;  /* ignore Huffman flag */
    if (pos + n > hlen) return -1;
    return pos + n;
}

static int h2_decode_status(const uint8_t *h, int hlen)
{
    int pos = 0;
    while (pos < hlen) {
        uint8_t b = h[pos];

        if (b & 0x80) {
            /* Indexed header field */
            uint32_t idx = b & 0x7F;
            pos++;
            /* :status 200=8, 204=9, 206=10, 304=11, 400=12, 404=13, 500=14 */
            if (idx == 8)  return 200;
            if (idx == 9)  return 204;
            if (idx == 10) return 206;
            if (idx == 11) return 304;
            if (idx == 12) return 400;
            if (idx == 13) return 404;
            if (idx == 14) return 500;
            continue;
        }
        if ((b & 0xC0) == 0x40) {
            /* Literal with incremental indexing: 01xxxxxx */
            uint32_t idx = b & 0x3F;
            pos++;
            if (idx == 0) { pos = hpack_skip_str(h, pos, hlen); if (pos < 0) return -1; }
            /* Value */
            if (pos >= hlen) return -1;
            int huff = h[pos] & 0x80;
            int vlen = h[pos++] & 0x7F;
            if (pos + vlen > hlen) return -1;
            /* If name is :status (indices 8–14 mean name is :status with a new value) */
            if (!huff && idx >= 8 && idx <= 14 && vlen >= 3) {
                char val[4] = {0};
                memcpy(val, h + pos, vlen < 3 ? (size_t)vlen : 3);
                int st = atoi(val);
                if (st > 0) return st;
            }
            pos += vlen;
            continue;
        }
        /* Literal without indexing (0000) / never indexed (0001) / dyntbl update (001x) */
        if ((b & 0xF0) == 0x00 || (b & 0xF0) == 0x10) {
            pos++;
            if ((b & 0x0F) == 0) { pos = hpack_skip_str(h, pos, hlen); if (pos < 0) return -1; }
            pos = hpack_skip_str(h, pos, hlen);
            if (pos < 0) return -1;
            continue;
        }
        pos++;  /* dynamic table size update or unknown */
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* HTTP/2 connection setup                                              */
/* ------------------------------------------------------------------ */

static uint32_t s_h2_next_sid = 1;

static esp_err_t h2_init(ts_ctrl_ctx_t *ctx)
{
    s_h2rx.pt_len  = 0;
    s_h2rx.pt_pos  = 0;
    s_h2_next_sid  = 1;

    /* Client connection preface: PRI magic (24 B) + empty SETTINGS (9 B) */
    static const uint8_t PREFACE[33] = {
        /* PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n */
        'P','R','I',' ','*',' ','H','T','T','P','/','2','.','0','\r','\n',
        '\r','\n','S','M','\r','\n','\r','\n',
        /* empty SETTINGS frame */
        0x00,0x00,0x00,  /* length = 0 */
        0x04,            /* type = SETTINGS */
        0x00,            /* flags = 0 */
        0x00,0x00,0x00,0x00  /* stream ID = 0 */
    };

    uint32_t enc_len = sizeof(PREFACE) + 16;
    static uint8_t enc_buf[sizeof(PREFACE) + 16];
    esp_err_t err = ts_noise_encrypt(&ctx->noise, PREFACE, sizeof(PREFACE), enc_buf);
    if (err != ESP_OK) return err;

    uint8_t nhdr[3] = {
        TS_MSG_TYPE_DATA,
        (uint8_t)((enc_len >> 8) & 0xFF),
        (uint8_t)( enc_len       & 0xFF)
    };
    if (tls_write(nhdr, 3) != 3) return ESP_FAIL;
    if (tls_write(enc_buf, enc_len) != (int)enc_len) return ESP_FAIL;
    ESP_LOGI(TAG, "H2: sent client preface + empty SETTINGS");

    /* First 9 bytes from server: EarlyPayload magic or HTTP/2 SETTINGS header */
    uint8_t first9[9];
    if (h2_read(ctx, first9, 9) != 9) return ESP_FAIL;

    static const uint8_t EP_MAGIC[5] = {0xff, 0xff, 0xff, 'T', 'S'};
    if (memcmp(first9, EP_MAGIC, 5) == 0) {
        uint32_t ep_len = ((uint32_t)first9[5] << 24) | ((uint32_t)first9[6] << 16) |
                          ((uint32_t)first9[7] <<  8) |  (uint32_t)first9[8];
        ESP_LOGI(TAG, "H2: early payload %"PRIu32" bytes — discarding", ep_len);
        if (h2_read(ctx, NULL, ep_len) != (int)ep_len) return ESP_FAIL;
        /* Read next 9 bytes = actual SETTINGS header */
        if (h2_read(ctx, first9, 9) != 9) return ESP_FAIL;
    }

    /* Parse as HTTP/2 frame header */
    uint32_t flen  = ((uint32_t)first9[0] << 16) | ((uint32_t)first9[1] << 8) | first9[2];
    uint8_t  ftype = first9[3];
    uint8_t  fflags= first9[4];
    uint32_t fsid  = ((uint32_t)(first9[5] & 0x7F) << 24) | ((uint32_t)first9[6] << 16) |
                     ((uint32_t)first9[7] <<  8) | first9[8];

    ESP_LOGI(TAG, "H2: server first frame type=0x%02x flags=0x%02x sid=%"PRIu32" len=%"PRIu32,
             ftype, fflags, fsid, flen);

    if (ftype != H2_FRAME_SETTINGS || (fflags & H2_FLAG_ACK) || fsid != 0) {
        ESP_LOGE(TAG, "H2: expected server SETTINGS, got type=0x%02x", ftype);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Drain SETTINGS payload */
    if (flen > 0 && h2_read(ctx, NULL, flen) != (int)flen) return ESP_FAIL;

    /* Send SETTINGS ACK */
    err = h2_send_frame(ctx, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "H2: SETTINGS ACK sent");

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* HTTP/2 POST — used for RegisterRequest                               */
/*                                                                      */
/* Sends HEADERS + DATA on a new stream; reads until END_STREAM.      */
/* Returns the decoded :status and fills resp_buf with the body.      */
/* ------------------------------------------------------------------ */

static esp_err_t h2_post(ts_ctrl_ctx_t *ctx,
                          const char *path,
                          const uint8_t *body, size_t body_len,
                          uint8_t *resp_buf, size_t resp_buf_size, size_t *resp_len)
{
    uint32_t sid = s_h2_next_sid;
    s_h2_next_sid += 2;

    static uint8_t hpack_buf[512];
    int hlen = h2_encode_req_headers(path, ctx->control_host,
                                      (uint32_t)body_len, hpack_buf, sizeof(hpack_buf));
    if (hlen < 0) { ESP_LOGE(TAG, "H2: HPACK encode failed"); return ESP_ERR_NO_MEM; }

    uint8_t hflags = H2_FLAG_END_HEADERS | (body_len == 0 ? H2_FLAG_END_STREAM : 0);
    esp_err_t err = h2_send_frame(ctx, H2_FRAME_HEADERS, hflags, sid,
                                   hpack_buf, (uint32_t)hlen);
    if (err != ESP_OK) return err;

    if (body_len > 0) {
        const uint8_t *p = body;
        size_t remain = body_len;
        while (remain > 0) {
            uint32_t chunk = (uint32_t)(remain < H2_MAX_PAYLOAD ? remain : H2_MAX_PAYLOAD);
            uint8_t  df    = (chunk == remain) ? H2_FLAG_END_STREAM : 0;
            err = h2_send_frame(ctx, H2_FRAME_DATA, df, sid, p, chunk);
            if (err != ESP_OK) return err;
            p      += chunk;
            remain -= chunk;
        }
    }
    ESP_LOGI(TAG, "H2: POST %s sid=%"PRIu32" body=%u B", path, sid, (unsigned)body_len);

    /* Read response frames until END_STREAM on our stream */
    static EXT_RAM_BSS_ATTR uint8_t fp[H2_MAX_PAYLOAD];
    static EXT_RAM_BSS_ATTR uint8_t hdr_block[H2_MAX_PAYLOAD];
    int    hdr_block_len  = 0;
    int    http_status    = -1;
    size_t resp_acc       = 0;
    bool   end_stream     = false;

    while (!end_stream) {
        uint8_t  ft, ff;
        uint32_t fs, fplen;
        err = h2_read_frame(ctx, &ft, &ff, &fs, fp, &fplen);
        if (err != ESP_OK) return err;

        if (h2_handle_ctrl_frame(ctx, ft, ff, fp, fplen)) continue;
        if (fs != sid) continue;

        if (ft == H2_FRAME_HEADERS || ft == H2_FRAME_CONTINUATION) {
            uint8_t *pl = fp;
            uint32_t pl_len = fplen;
            if (ft == H2_FRAME_HEADERS) {
                if (ff & H2_FLAG_PRIORITY) {
                    if (pl_len < 5) return ESP_ERR_INVALID_RESPONSE;
                    pl += 5; pl_len -= 5;
                }
                if (ff & H2_FLAG_PADDED) {
                    if (pl_len < 1) return ESP_ERR_INVALID_RESPONSE;
                    uint8_t pad = pl[0]; pl++; pl_len--;
                    if (pl_len < pad) return ESP_ERR_INVALID_RESPONSE;
                    pl_len -= pad;
                }
            }
            size_t copy = pl_len < (uint32_t)(H2_MAX_PAYLOAD - hdr_block_len)
                        ? pl_len : (uint32_t)(H2_MAX_PAYLOAD - hdr_block_len);
            memcpy(hdr_block + hdr_block_len, pl, copy);
            hdr_block_len += (int)copy;
            if (ff & H2_FLAG_END_HEADERS) {
                http_status   = h2_decode_status(hdr_block, hdr_block_len);
                hdr_block_len = 0;
                ESP_LOGI(TAG, "H2: :status %d (sid=%"PRIu32")", http_status, sid);
            }
            if (ff & H2_FLAG_END_STREAM) end_stream = true;

        } else if (ft == H2_FRAME_DATA) {
            size_t avail = resp_buf_size - resp_acc;
            size_t n     = fplen < avail ? fplen : avail;
            if (n > 0) { memcpy(resp_buf + resp_acc, fp, n); resp_acc += n; }
            if (ff & H2_FLAG_END_STREAM) end_stream = true;
        }
    }

    *resp_len = resp_acc;
    if (http_status < 200 || http_status >= 300) {
        if (http_status < 0) ESP_LOGE(TAG, "H2: :status not found");
        else                 ESP_LOGE(TAG, "H2: HTTP error %d", http_status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Map DATA stream reader                                               */
/*                                                                      */
/* After sending MapRequest HEADERS+DATA we read the response HEADERS  */
/* frame, then pull streaming body out of DATA frames using this       */
/* helper buffer.                                                      */
/* ------------------------------------------------------------------ */

static struct {
    uint8_t  buf[H2_MAX_PAYLOAD];
    size_t   len;
    size_t   pos;
    uint32_t sid;   /* stream we're reading from */
    bool     done;  /* END_STREAM received         */
} s_map_rx;

static int map_data_read(ts_ctrl_ctx_t *ctx, uint8_t *out, size_t need)
{
    size_t total = 0;
    while (total < need) {
        if (s_map_rx.pos < s_map_rx.len) {
            size_t avail = s_map_rx.len - s_map_rx.pos;
            size_t n     = avail < (need - total) ? avail : (need - total);
            if (out) memcpy(out + total, s_map_rx.buf + s_map_rx.pos, n);
            s_map_rx.pos += n;
            total        += n;
            continue;
        }
        if (s_map_rx.done) return (int)total;   /* stream ended */

        static EXT_RAM_BSS_ATTR uint8_t fp[H2_MAX_PAYLOAD];
        uint8_t  ft, ff;
        uint32_t fs, fplen;
        if (h2_read_frame(ctx, &ft, &ff, &fs, fp, &fplen) != ESP_OK) return -1;

        if (h2_handle_ctrl_frame(ctx, ft, ff, fp, fplen)) continue;
        if (fs != s_map_rx.sid) continue;
        if (ft != H2_FRAME_DATA) continue;

        size_t n = fplen < sizeof(s_map_rx.buf) ? fplen : sizeof(s_map_rx.buf);
        memcpy(s_map_rx.buf, fp, n);
        s_map_rx.len = n;
        s_map_rx.pos = 0;
        if (ff & H2_FLAG_END_STREAM) s_map_rx.done = true;
    }
    return (int)total;
}

/* Persistent reusable buffer for MapResponse segments. Allocated lazily on
 * the first call, then grown as needed, and never freed: this avoids heap
 * fragmentation that becomes fatal once the large initial DERPMap-bearing
 * segment must coexist with WiFi/TLS/WG/DERP allocations. */
static char  *s_map_persistent_buf  = NULL;
static size_t s_map_persistent_size = 0;

/* Read one [4B LE length][body] segment from the map DATA stream into the
 * persistent buffer. The returned pointer is owned by this module — do NOT
 * free() it; it stays valid until the next call to map_read_one. */
static esp_err_t map_read_one(ts_ctrl_ctx_t *ctx,
                               char **buf_out, size_t *len_out)
{
    uint8_t siz[4];
    if (map_data_read(ctx, siz, 4) != 4) return ESP_FAIL;
    uint32_t msg_len = (uint32_t)siz[0] | ((uint32_t)siz[1] << 8) |
                       ((uint32_t)siz[2] << 16) | ((uint32_t)siz[3] << 24);

    ESP_LOGD(TAG, "Map segment len=%"PRIu32, msg_len);
    if (msg_len == 0 || msg_len > 256 * 1024) {
        ESP_LOGE(TAG, "Map segment length invalid: %"PRIu32, msg_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Grow persistent buffer if needed. We only ever grow, never shrink, so
     * once we've successfully held the largest segment we never re-fragment. */
    if (msg_len + 1 > s_map_persistent_size) {
        char *grown = realloc(s_map_persistent_buf, msg_len + 1);
        if (!grown) {
            ESP_LOGE(TAG, "Map buffer realloc OOM (need %"PRIu32" B); draining segment",
                     msg_len + 1);
            /* Drain msg_len bytes from the stream so subsequent reads stay
             * aligned on the next 4-byte length prefix. */
            uint8_t dump[256];
            uint32_t left = msg_len;
            while (left > 0) {
                uint32_t n = left > sizeof(dump) ? sizeof(dump) : left;
                if (map_data_read(ctx, dump, n) != (int)n) return ESP_FAIL;
                left -= n;
            }
            return ESP_ERR_NO_MEM;
        }
        s_map_persistent_buf  = grown;
        s_map_persistent_size = msg_len + 1;
    }

    if (map_data_read(ctx, (uint8_t *)s_map_persistent_buf, msg_len) != (int)msg_len)
        return ESP_FAIL;
    s_map_persistent_buf[msg_len] = '\0';
    *buf_out = s_map_persistent_buf;
    *len_out = msg_len;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Step 1: Fetch server Noise public key via GET /key                  */
/* ------------------------------------------------------------------ */

static esp_err_t ctrl_fetch_server_key(ts_ctrl_ctx_t *ctx, uint8_t srv_pub_out[32])
{
    char req[256];
    snprintf(req, sizeof(req),
             "GET /key?v=" TS_CAP_VERSION " HTTP/1.1\r\n"
             "Host: %s\r\nConnection: close\r\n\r\n", ctx->control_host);
    if (tls_write((const uint8_t *)req, strlen(req)) < 0) return ESP_FAIL;

    char line[256];
    int  llen = 0, content_length = -1;
    bool status_ok = false, chunked = false;
    while (1) {
        uint8_t c;
        if (tls_read(&c, 1) != 1) return ESP_FAIL;
        if (c == '\r') continue;
        if (c == '\n') {
            line[llen] = '\0';
            if (llen == 0) break;
            if (!status_ok && strstr(line, "200")) status_ok = true;
            if (strncasecmp(line, "Content-Length:", 15) == 0)
                content_length = atoi(line + 15);
            if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
                strstr(line + 18, "chunked")) chunked = true;
            llen = 0;
        } else { if (llen < (int)sizeof(line) - 1) line[llen++] = (char)c; }
    }
    if (!status_ok) { ESP_LOGE(TAG, "GET /key: non-200"); return ESP_FAIL; }

    static char body[512];
    int body_len = 0;
    if (content_length > 0 && content_length < (int)sizeof(body) - 1) {
        if (tls_read((uint8_t *)body, (size_t)content_length) != content_length) return ESP_FAIL;
        body_len = content_length;
    } else if (chunked) {
        while (body_len < (int)sizeof(body) - 1) {
            char szline[16]; int szlen = 0;
            while (szlen < (int)sizeof(szline) - 1) {
                uint8_t c;
                if (esp_tls_conn_read(s_tls, &c, 1) <= 0) goto done_chunked;
                if (c == '\r') continue;
                if (c == '\n') break;
                szline[szlen++] = (char)c;
            }
            szline[szlen] = '\0';
            int chunk_sz = (int)strtol(szline, NULL, 16);
            if (chunk_sz <= 0) break;
            int space = (int)sizeof(body) - 1 - body_len;
            int toread = chunk_sz < space ? chunk_sz : space;
            if (tls_read((uint8_t *)body + body_len, (size_t)toread) != toread) return ESP_FAIL;
            body_len += toread;
            for (int i = toread; i < chunk_sz; i++) { uint8_t d; esp_tls_conn_read(s_tls, &d, 1); }
            uint8_t crlf[2]; tls_read(crlf, 2);
        }
        done_chunked:;
    } else {
        while (body_len < (int)sizeof(body) - 1) {
            int n = esp_tls_conn_read(s_tls, (uint8_t *)body + body_len, 1);
            if (n <= 0) break;
            body_len++;
        }
    }
    body[body_len] = '\0';
    ESP_LOGI(TAG, "/key body (%d B): %s", body_len, body);

    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *pk = cJSON_GetObjectItem(root, "publicKey");
        if (!pk) pk = cJSON_GetObjectItem(root, "PublicKey");
        if (cJSON_IsString(pk) && pk->valuestring) {
            bool ok = ts_key_from_hex(pk->valuestring, srv_pub_out);
            cJSON_Delete(root);
            if (ok) { ESP_LOGI(TAG, "Server key fetched (JSON)"); return ESP_OK; }
        }
        cJSON_Delete(root);
    }

    char *trimmed = body;
    while (*trimmed == ' ' || *trimmed == '\n' || *trimmed == '\r') trimmed++;
    if (ts_key_from_hex(trimmed, srv_pub_out)) {
        ESP_LOGI(TAG, "Server key fetched (plain)");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to parse server key");
    return ESP_ERR_INVALID_RESPONSE;
}

/* ------------------------------------------------------------------ */
/* Step 2: HTTP/1.1 upgrade to ts2021 carrying Noise init              */
/* ------------------------------------------------------------------ */

static esp_err_t do_http_upgrade_noise(ts_ctrl_ctx_t *ctx, const uint8_t *init_msg)
{
    char init_b64[200];
    size_t b64_len = sizeof(init_b64) - 1;
    if (!wireguard_base64_encode(init_msg, TS_NOISE_INIT_MSG_LEN, init_b64, &b64_len)) {
        ESP_LOGE(TAG, "base64 encode failed"); return ESP_FAIL;
    }
    init_b64[b64_len] = '\0';

    char hdr[768];
    snprintf(hdr, sizeof(hdr),
             "POST /ts2021 HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Upgrade: tailscale-control-protocol\r\n"
             "Connection: upgrade\r\n"
             "Tailscale-Cap-Version: " TS_CAP_VERSION "\r\n"
             "Tailscale-Version: 1.56.0\r\n"
             "X-Tailscale-Handshake: %s\r\n"
             "Content-Length: 0\r\n\r\n",
             ctx->control_host, init_b64);
    if (tls_write((const uint8_t *)hdr, strlen(hdr)) < 0) return ESP_FAIL;

    char line[256]; int llen = 0; bool switched = false;
    while (1) {
        uint8_t c;
        if (tls_read(&c, 1) != 1) return ESP_FAIL;
        if (c == '\r') continue;
        if (c == '\n') {
            line[llen] = '\0';
            if (llen == 0) break;
            ESP_LOGI(TAG, "/ts2021: %s", line);
            if (!switched && strstr(line, "101")) switched = true;
            llen = 0;
        } else { if (llen < (int)sizeof(line) - 1) line[llen++] = (char)c; }
    }
    if (!switched) {
        char errbody[128] = {0}; int eblen = 0;
        while (eblen < (int)sizeof(errbody) - 1) {
            int n = esp_tls_conn_read(s_tls, (uint8_t *)errbody + eblen, 1);
            if (n <= 0) break;
            eblen++;
        }
        errbody[eblen] = '\0';
        ESP_LOGE(TAG, "Upgrade failed: %s", errbody);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "HTTP upgrade to ts2021 OK");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Connect: TLS → fetch key → Noise IK → HTTP/2 init                  */
/* ------------------------------------------------------------------ */

static esp_tls_t *tls_open(const char *host)
{
    ESP_LOGI(TAG, "TLS connecting to %s:443...", host);
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .non_block         = false,
        .timeout_ms        = 30000,
    };
    esp_tls_t *tls = esp_tls_init();
    if (!tls) { ESP_LOGE(TAG, "esp_tls_init failed"); return NULL; }
    if (esp_tls_conn_new_sync(host, (int)strlen(host), 443, &cfg, tls) != 1) {
        ESP_LOGE(TAG, "TLS connect %s:443 failed", host);
        esp_tls_conn_destroy(tls); return NULL;
    }
    ESP_LOGI(TAG, "TLS connected to %s", host);
    return tls;
}

static esp_err_t ctrl_connect(ts_ctrl_ctx_t *ctx)
{
    /* Close any leftover connection from a previous attempt */
    if (s_tls) { esp_tls_conn_destroy(s_tls); s_tls = NULL; }

    /* Connection 1: fetch Noise public key (Connection: close) */
    s_tls = tls_open(ctx->control_host);
    if (!s_tls) return ESP_FAIL;

    uint8_t srv_pub[32];
    esp_err_t err = ctrl_fetch_server_key(ctx, srv_pub);
    esp_tls_conn_destroy(s_tls); s_tls = NULL;
    if (err != ESP_OK) return err;

    /* Connection 2: upgrade, Noise handshake, HTTP/2 */
    s_tls = tls_open(ctx->control_host);
    if (!s_tls) return ESP_FAIL;

    uint8_t       init_msg[TS_NOISE_INIT_MSG_LEN];
    ts_noise_hs_t hs;
    err = ts_noise_build_init(ctx->keys->machine_priv, ctx->keys->machine_pub,
                               srv_pub, init_msg, &hs);
    if (err != ESP_OK) goto fail;

    err = do_http_upgrade_noise(ctx, init_msg);
    if (err != ESP_OK) goto fail;

    /* Read 51-byte Noise IK response */
    uint8_t resp_msg[TS_NOISE_RESP_MSG_LEN];
    if (tls_read(resp_msg, TS_NOISE_RESP_MSG_LEN) != TS_NOISE_RESP_MSG_LEN) {
        ESP_LOGE(TAG, "Failed to read Noise response"); err = ESP_FAIL; goto fail;
    }
    err = ts_noise_finish(&hs, ctx->keys->machine_priv, resp_msg, &ctx->noise);
    if (err != ESP_OK) goto fail;

    err = h2_init(ctx);
    if (err != ESP_OK) goto fail;

    esp_tls_get_conn_sockfd(s_tls, &ctx->fd);
    return ESP_OK;

fail:
    esp_tls_conn_destroy(s_tls); s_tls = NULL;
    return err;
}

/* ------------------------------------------------------------------ */
/* RegisterRequest / RegisterResponse                                  */
/* ------------------------------------------------------------------ */

esp_err_t ts_ctrl_register(ts_ctrl_ctx_t *ctx)
{
    esp_err_t err = ctrl_connect(ctx);
    if (err != ESP_OK) return err;

    char node_key_hex[80];
    ts_key_to_hex("nodekey:", ctx->keys->node_pub, node_key_hex, sizeof(node_key_hex));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "Version",    65);
    cJSON_AddStringToObject(root, "NodeKey",    node_key_hex);
    cJSON_AddStringToObject(root, "OldNodeKey", node_key_hex);

    if (ctx->auth_key && ctx->auth_key[0] != '\0') {
        cJSON *auth = cJSON_CreateObject();
        cJSON_AddStringToObject(auth, "AuthKey", ctx->auth_key);
        cJSON_AddItemToObject(root, "Auth", auth);
    }

    cJSON *hi = cJSON_CreateObject();
    cJSON_AddStringToObject(hi, "OS",       "linux");
    cJSON_AddStringToObject(hi, "Hostname", ctx->hostname);
    cJSON_AddStringToObject(hi, "GoArch",   "arm");
    cJSON_AddStringToObject(hi, "GoVersion","go1.21");
    cJSON_AddItemToObject(root, "Hostinfo", hi);
    cJSON_AddItemToObject(root, "Endpoints", cJSON_CreateArray());

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;
    /* Body contains the auth_key in plaintext — log only at DEBUG level. */
    ESP_LOGD(TAG, "RegisterRequest: %s", json_str);

    static EXT_RAM_BSS_ATTR uint8_t resp_buf[CTRL_FRAME_BUF];
    size_t resp_len = 0;
    err = h2_post(ctx, "/machine/register",
                  (const uint8_t *)json_str, strlen(json_str),
                  resp_buf, sizeof(resp_buf) - 1, &resp_len);
    free(json_str);
    if (err != ESP_OK) return err;

    resp_buf[resp_len] = '\0';
    /* Response contains user/login profile data (PII) — DEBUG level only. */
    ESP_LOGD(TAG, "RegisterResponse (%u B): %.*s",
             (unsigned)resp_len, (int)resp_len, (char *)resp_buf);

    cJSON *resp = cJSON_Parse((const char *)resp_buf);
    if (!resp) {
        char hex[100] = {0};
        for (size_t i = 0; i < resp_len && i < 32; i++)
            snprintf(hex + i * 3, 4, "%02x ", resp_buf[i]);
        ESP_LOGE(TAG, "RegisterResponse parse failed; hex: %s", hex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *auth_url = cJSON_GetObjectItemCaseSensitive(resp, "AuthURL");
    if (cJSON_IsString(auth_url) && auth_url->valuestring[0] != '\0') {
        ESP_LOGW(TAG, "Needs approval: %s", auth_url->valuestring);
        strlcpy(s_auth_url, auth_url->valuestring, sizeof(s_auth_url));
    } else {
        s_auth_url[0] = '\0';
    }

    ctx->registered = true;
    cJSON_Delete(resp);
    ESP_LOGI(TAG, "Registration complete");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* MapRequest — POST /machine/map with streaming response              */
/* ------------------------------------------------------------------ */

esp_err_t ts_ctrl_map_request(ts_ctrl_ctx_t *ctx)
{
    if (!ctx->registered) return ESP_ERR_INVALID_STATE;

    char node_key_hex[80];
    ts_key_to_hex("nodekey:", ctx->keys->node_pub, node_key_hex, sizeof(node_key_hex));
    char disco_key_hex[80];
    ts_key_to_hex("discokey:", ctx->keys->disco_pub, disco_key_hex, sizeof(disco_key_hex));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "Version",     65);
    cJSON_AddStringToObject(root, "NodeKey",     node_key_hex);
    cJSON_AddStringToObject(root, "DiscoKey",    disco_key_hex);
    cJSON_AddBoolToObject(root,   "Stream",      true);
    cJSON_AddBoolToObject(root,   "IncludeIPv6", false);
    cJSON_AddBoolToObject(root,   "OmitPeers",   false);

    cJSON *hi = cJSON_CreateObject();
    cJSON_AddStringToObject(hi, "OS",       "linux");
    cJSON_AddStringToObject(hi, "Hostname", ctx->hostname);

    /* Tell the coordinator which DERP region we're connected to so other
     * peers know where to relay packets for us. On the very first MapRequest
     * we don't know yet (no DERPMap parsed); the ctrl_task re-issues this
     * request after the first MapResponse arrives. */
    ts_derp_node_t derp_hi;
    if (ts_netmap_get_derp_home(&derp_hi) && derp_hi.region_id > 0) {
        cJSON *netinfo = cJSON_CreateObject();
        cJSON_AddNumberToObject(netinfo, "PreferredDERP", derp_hi.region_id);
        cJSON_AddStringToObject(netinfo, "LinkType", "wired");
        cJSON_AddItemToObject(hi, "NetInfo", netinfo);
    }

    cJSON_AddItemToObject(root, "Hostinfo", hi);

    /* Pubip-discovered (or otherwise published) UDP endpoints we advertise
     * to peers. Empty when the probe hasn't completed yet — coordinator will
     * still emit MapResponse and we re-issue this MapRequest once a result
     * lands. */
    cJSON *eps = cJSON_CreateArray();
    for (int i = 0; i < s_endpoint_count; i++) {
        if (s_endpoints[i][0]) {
            cJSON_AddItemToArray(eps, cJSON_CreateString(s_endpoints[i]));
        }
    }
    cJSON_AddItemToObject(root, "Endpoints", eps);
    s_endpoints_dirty = false;

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    /* Assign a stream ID and send HEADERS + DATA */
    uint32_t sid = s_h2_next_sid;
    s_h2_next_sid += 2;

    static uint8_t hpack_buf[512];
    int hlen = h2_encode_req_headers("/machine/map", ctx->control_host,
                                      (uint32_t)strlen(json_str), hpack_buf, sizeof(hpack_buf));
    if (hlen < 0) { free(json_str); return ESP_ERR_NO_MEM; }

    esp_err_t err = h2_send_frame(ctx, H2_FRAME_HEADERS, H2_FLAG_END_HEADERS,
                                   sid, hpack_buf, (uint32_t)hlen);
    if (err != ESP_OK) { free(json_str); return err; }

    err = h2_send_frame(ctx, H2_FRAME_DATA, H2_FLAG_END_STREAM, sid,
                         (const uint8_t *)json_str, (uint32_t)strlen(json_str));
    free(json_str);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "H2: MapRequest sent on sid=%"PRIu32, sid);

    /* Consume response HEADERS to validate status */
    static EXT_RAM_BSS_ATTR uint8_t fp[H2_MAX_PAYLOAD];
    static EXT_RAM_BSS_ATTR uint8_t hdr_block[H2_MAX_PAYLOAD];
    int  hdr_block_len = 0;
    bool got_headers   = false;

    while (!got_headers) {
        uint8_t  ft, ff; uint32_t fs, fplen;
        err = h2_read_frame(ctx, &ft, &ff, &fs, fp, &fplen);
        if (err != ESP_OK) return err;

        if (h2_handle_ctrl_frame(ctx, ft, ff, fp, fplen)) continue;
        if (fs != sid) continue;

        if (ft == H2_FRAME_HEADERS || ft == H2_FRAME_CONTINUATION) {
            size_t copy = fplen < (uint32_t)(H2_MAX_PAYLOAD - hdr_block_len)
                        ? fplen : (uint32_t)(H2_MAX_PAYLOAD - hdr_block_len);
            memcpy(hdr_block + hdr_block_len, fp, copy);
            hdr_block_len += (int)copy;
            if (ff & H2_FLAG_END_HEADERS) {
                int st = h2_decode_status(hdr_block, hdr_block_len);
                ESP_LOGI(TAG, "H2: map :status %d", st);
                if (st < 200 || st >= 300) {
                    /* Log error body for debugging */
                    static EXT_RAM_BSS_ATTR uint8_t err_fp[H2_MAX_PAYLOAD];
                    uint32_t err_fplen;
                    uint8_t eft, eff; uint32_t efs;
                    if (h2_read_frame(ctx, &eft, &eff, &efs, err_fp, &err_fplen) == ESP_OK
                            && eft == H2_FRAME_DATA && err_fplen > 0 && err_fplen < sizeof(err_fp)) {
                        err_fp[err_fplen] = '\0';
                        ESP_LOGE(TAG, "H2: map error body: %s", err_fp);
                    }
                    return ESP_ERR_INVALID_RESPONSE;
                }
                got_headers = true;
            }
        }
    }

    /* Set up the map DATA stream reader */
    s_map_rx.len  = 0;
    s_map_rx.pos  = 0;
    s_map_rx.sid  = sid;
    s_map_rx.done = false;

    char *map_buf = NULL;
    size_t map_len = 0;
    err = map_read_one(ctx, &map_buf, &map_len);
    if (err != ESP_OK) return err;

    ESP_LOGD(TAG, "MapResponse segment (%u B)", (unsigned)map_len);
    esp_err_t apply_err = ts_netmap_apply(map_buf, map_len);
    /* map_buf is owned by map_read_one's persistent buffer — do NOT free. */

    /* Connect to DERP home server identified in the map response */
    ts_derp_node_t derp_home;
    if (ts_netmap_get_derp_home(&derp_home)) {
        ts_derp_set_home(&derp_home,
                         ctx->keys->node_priv, ctx->keys->node_pub);
    }

    return apply_err;
}

/* ------------------------------------------------------------------ */
/* Long-poll loop                                                       */
/* ------------------------------------------------------------------ */

void ts_ctrl_poll_loop(ts_ctrl_ctx_t *ctx)
{
    while (!s_stop) {
        char *map_buf = NULL;
        size_t map_len = 0;
        esp_err_t err = map_read_one(ctx, &map_buf, &map_len);
        if (err != ESP_OK) {
            /* OOM is recoverable — map_read_one drained the segment from the
             * stream so the next length-prefix is still aligned. Other errors
             * are fatal for this connection. */
            if (err == ESP_ERR_NO_MEM) continue;
            if (s_stop) break;
            ESP_LOGW(TAG, "Map recv error — reconnect in 10 s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            if (s_tls) { esp_tls_conn_destroy(s_tls); s_tls = NULL; }
            ctx->registered = false;
            if (ts_ctrl_register(ctx) == ESP_OK)
                ts_ctrl_map_request(ctx);
            continue;
        }
        ESP_LOGD(TAG, "Incremental MapResponse (%u B)", (unsigned)map_len);
        ts_netmap_apply(map_buf, map_len);
        /* map_buf owned by map_read_one — do NOT free. */

        ts_derp_node_t derp_home;
        if (ts_netmap_get_derp_home(&derp_home)) {
            ts_derp_set_home(&derp_home,
                             ctx->keys->node_priv, ctx->keys->node_pub);
        }

        /* Pubip (or other endpoint source) signalled a change while we
         * were blocked on the long-poll. Re-issue MapRequest so peers
         * see the new Endpoints. */
        if (s_endpoints_dirty) {
            esp_err_t merr = ts_ctrl_map_request(ctx);
            if (merr != ESP_OK) {
                ESP_LOGW(TAG, "Endpoint MapRequest re-issue failed (%s)",
                         esp_err_to_name(merr));
            }
        }
    }
}

void ts_ctrl_stop(ts_ctrl_ctx_t *ctx)
{
    s_stop = true;
    if (s_tls) { esp_tls_conn_destroy(s_tls); s_tls = NULL; }
}
