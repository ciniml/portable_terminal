/*
 * Tailscale public-IP discovery via HTTPS — see tailscale_pubip.h.
 *
 * Replaces the UDP STUN client. In MAP-E / IPv4-over-IPv6 environments
 * (v6プラス / BBIX フルマップ at the author's home) STUN BindingResponses
 * over UDP don't return through the upstream NAT — but TCP 443 to a public
 * "what's my IP" service works fine, so we use that to learn the WAN IPv4.
 * Port we publish is the WG listen port (CONFIG_TAILSCALE_LISTEN_PORT),
 * relying on port preservation by the upstream NAT.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_pubip.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"

#include "sdkconfig.h"

#include <string.h>
#include <ctype.h>

static const char *TAG = "ts_pubip";

#define PUBIP_TASK_STACK     6144
#define PUBIP_TASK_PRIO      4
#define PUBIP_PERIOD_MS      (5 * 60 * 1000)   /* 5 minutes */
#define PUBIP_RX_BUF_MAX     256
#define PUBIP_HTTP_TIMEOUT   10000             /* ms */

#define PUBIP_URL_PRIMARY    "https://api.ipify.org/"
#define PUBIP_URL_FALLBACK   "https://checkip.amazonaws.com/"

static ts_pubip_result_fn s_cb           = NULL;
static TaskHandle_t       s_task         = NULL;
static SemaphoreHandle_t  s_sem          = NULL;
static TimerHandle_t      s_timer        = NULL;
static uint32_t           s_last_ip_be   = 0;
static uint16_t           s_last_port    = 0;
static bool               s_have_last    = false;
static volatile bool      s_running      = false;

/* ------------------------------------------------------------------ */
/* HTTP fetch                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;       /* heap-allocated; freed by caller */
    size_t len;
    size_t cap;
} rx_accum_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        rx_accum_t *a = (rx_accum_t *)evt->user_data;
        if (!a || !a->buf) return ESP_OK;
        size_t space = (a->cap > a->len + 1) ? (a->cap - a->len - 1) : 0;
        size_t take  = evt->data_len < (int)space ? (size_t)evt->data_len : space;
        if (take > 0) {
            memcpy(a->buf + a->len, evt->data, take);
            a->len += take;
            a->buf[a->len] = '\0';
        }
    }
    return ESP_OK;
}

/* Try to find a dotted-quad IPv4 in @body and parse it into @out_be.
 * Returns true on success. Whitespace and surrounding HTML are tolerated:
 * we scan for the first run of [0-9.] characters of length 7..15 and feed
 * it to inet_pton. */
static bool parse_ipv4_body(const char *body, uint32_t *out_be)
{
    if (!body || !out_be) return false;

    /* Quick path: skip leading whitespace, then take everything up to the
     * next whitespace. Covers ipify ("a.b.c.d") and checkip ("a.b.c.d\n"). */
    const char *p = body;
    while (*p && isspace((unsigned char)*p)) p++;
    char trimmed[16];
    size_t n = 0;
    while (*p && n + 1 < sizeof(trimmed) && !isspace((unsigned char)*p)) {
        trimmed[n++] = *p++;
    }
    trimmed[n] = '\0';
    if (n > 0) {
        struct in_addr a;
        if (inet_pton(AF_INET, trimmed, &a) == 1) {
            *out_be = a.s_addr;
            return true;
        }
    }

    /* Defensive scan: look for the first run of [0-9.] of length 7..15
     * anywhere in the body and try inet_pton on it. Handles services that
     * wrap the address in HTML or JSON. */
    for (const char *q = body; *q; ) {
        if ((*q >= '0' && *q <= '9') || *q == '.') {
            const char *start = q;
            while (*q && ((*q >= '0' && *q <= '9') || *q == '.')) q++;
            size_t run = (size_t)(q - start);
            if (run >= 7 && run <= 15) {
                char tmp[16];
                memcpy(tmp, start, run);
                tmp[run] = '\0';
                struct in_addr a;
                if (inet_pton(AF_INET, tmp, &a) == 1) {
                    *out_be = a.s_addr;
                    return true;
                }
            }
        } else {
            q++;
        }
    }
    return false;
}

/* HTTP GET @url, accumulate body into @buf, return ESP_OK if HTTP 200. */
static esp_err_t do_get(const char *url, char *buf, size_t cap)
{
    rx_accum_t accum = { .buf = buf, .len = 0, .cap = cap };
    buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = PUBIP_HTTP_TIMEOUT,
        .event_handler     = http_event_cb,
        .user_data         = &accum,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "probing %s", url);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP fetch %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %s: status=%d", url, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Run one probe: try primary, then fallback. On success, dedup against the
 * previous result and invoke the callback when it has changed. */
static void run_probe(void)
{
    char  body[PUBIP_RX_BUF_MAX];
    bool  got    = false;
    uint32_t ip_be = 0;

    if (do_get(PUBIP_URL_PRIMARY, body, sizeof(body)) == ESP_OK) {
        if (parse_ipv4_body(body, &ip_be)) got = true;
    }
    if (!got) {
        if (do_get(PUBIP_URL_FALLBACK, body, sizeof(body)) == ESP_OK) {
            if (parse_ipv4_body(body, &ip_be)) got = true;
        }
    }
    if (!got) {
        ESP_LOGW(TAG, "no IPv4 parsed from any service");
        return;
    }

    uint16_t port = (uint16_t)CONFIG_TAILSCALE_LISTEN_PORT;

    if (s_have_last && ip_be == s_last_ip_be && port == s_last_port) {
        ESP_LOGD(TAG, "unchanged endpoint");
        return;
    }
    s_last_ip_be = ip_be;
    s_last_port  = port;
    s_have_last  = true;

    ESP_LOGI(TAG, "discovered %u.%u.%u.%u:%u",
             (unsigned)((ip_be      ) & 0xFF),
             (unsigned)((ip_be >>  8) & 0xFF),
             (unsigned)((ip_be >> 16) & 0xFF),
             (unsigned)((ip_be >> 24) & 0xFF),
             (unsigned)port);

    if (s_cb) s_cb(ip_be, port);
}

/* ------------------------------------------------------------------ */
/* Task + timer                                                         */
/* ------------------------------------------------------------------ */

static void pubip_task(void *arg)
{
    (void)arg;
    while (s_running) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) continue;
        if (!s_running) break;
        run_probe();
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static void pubip_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_sem) xSemaphoreGive(s_sem);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t ts_pubip_init(ts_pubip_result_fn cb)
{
    if (s_running) return ESP_ERR_INVALID_STATE;

    s_cb         = cb;
    s_have_last  = false;
    s_last_ip_be = 0;
    s_last_port  = 0;

    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) {
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed");
        return ESP_ERR_NO_MEM;
    }

    s_timer = xTimerCreate("ts_pubip", pdMS_TO_TICKS(PUBIP_PERIOD_MS),
                           pdTRUE, NULL, pubip_timer_cb);
    if (!s_timer) {
        ESP_LOGE(TAG, "xTimerCreate failed");
        vSemaphoreDelete(s_sem);
        s_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    BaseType_t ok = xTaskCreate(pubip_task, "ts_pubip", PUBIP_TASK_STACK,
                                NULL, PUBIP_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        xTimerDelete(s_timer, 0);
        s_timer = NULL;
        vSemaphoreDelete(s_sem);
        s_sem = NULL;
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    if (xTimerStart(s_timer, 0) != pdPASS) {
        ESP_LOGW(TAG, "xTimerStart failed");
    }

    /* Fire one immediately rather than waiting for the first 5-min tick. */
    xSemaphoreGive(s_sem);

    ESP_LOGI(TAG, "init OK (period=%ds, port=%d)",
             PUBIP_PERIOD_MS / 1000, (int)CONFIG_TAILSCALE_LISTEN_PORT);
    return ESP_OK;
}

void ts_pubip_deinit(void)
{
    if (!s_running && !s_sem && !s_timer) return;

    /* Stop the timer first so it can't poke s_sem after we delete it. */
    if (s_timer) {
        xTimerStop(s_timer, 0);
        xTimerDelete(s_timer, 0);
        s_timer = NULL;
    }

    /* Wake the task so it observes s_running=false and exits. */
    s_running = false;
    if (s_sem) xSemaphoreGive(s_sem);

    /* The task self-deletes; give it a moment so we don't yank the semaphore
     * out from under it. ~50 ms is enough for any non-blocked iteration; a
     * probe in flight may extend the wait but that's bounded by the HTTP
     * timeout above. */
    for (int i = 0; i < 20 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_sem) {
        vSemaphoreDelete(s_sem);
        s_sem = NULL;
    }
    s_cb        = NULL;
    s_have_last = false;
}

void ts_pubip_kick(void)
{
    if (!s_running || !s_sem) return;
    xSemaphoreGive(s_sem);
}
