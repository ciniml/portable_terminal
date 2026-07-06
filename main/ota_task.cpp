// OTA update client — see ota_task.hpp and docs/OTA_UPDATE_DESIGN.md.
//
// Pipeline: poll latest.json → SemVer compare → esp_https_ota download
// (partial HTTP + 256 KB chunks) → deferred reboot (wait for idle) →
// bootloader rollback if post-boot health check fails.

#include "ota_task.hpp"

#include "sdkconfig.h"

#if CONFIG_TAB5_OTA_ENABLED

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "cJSON.h"

#include "connection.hpp"

namespace tab5::ota {

namespace {

constexpr const char* kTag       = "ota";
constexpr const char* kNvsNs     = "ota";
constexpr int         kMaxUrlLen = 256;
constexpr int         kChunkSize = 256 * 1024;

// Polling schedule.
constexpr int64_t kInitialDelayUs  = 100LL * 1000 * 1000;      // 100 s after boot
constexpr int64_t kPollJitterUs    = 15LL  * 60 * 1000 * 1000; // ±15 min

// Backoff after Failed. 1 h → 2 h → 4 h ... cap 24 h.
constexpr int64_t kMinBackoffUs = 1LL  * 3600 * 1000 * 1000;
constexpr int64_t kMaxBackoffUs = 24LL * 3600 * 1000 * 1000;

// Health check window before mark_app_valid_cancel_rollback.
constexpr int64_t kHealthWindowUs = 120LL * 1000 * 1000;

// Deferred-reboot window: poll every 10 s, up to 30 min.
constexpr int64_t kDeferredRebootTimeoutUs = 30LL * 60 * 1000 * 1000;
constexpr int64_t kDeferredRebootPollUs    = 10LL * 1000 * 1000;

// Rollback cool-down after a bootloader-triggered rollback.
constexpr int64_t kRollbackCooldownUs = 24LL * 3600 * 1000 * 1000;

// ---- Module state -------------------------------------------------------

std::mutex          g_status_mu;
Status              g_status;
std::atomic<bool>   g_started{false};
std::atomic<int>    g_fail_streak{0};
QueueHandle_t       g_trigger_q = nullptr;    // Trigger enum ints
RebootReadyFn       g_reboot_ready = nullptr;
int64_t             g_rollback_cooldown_until_us = 0;

void set_phase(Status::Phase p, const char* msg = nullptr, int pct = -1) {
    std::lock_guard<std::mutex> lk(g_status_mu);
    g_status.phase   = p;
    g_status.percent = pct;
    if (msg) {
        std::snprintf(g_status.message, sizeof(g_status.message), "%s", msg);
    }
}

void set_progress(int pct) {
    std::lock_guard<std::mutex> lk(g_status_mu);
    g_status.percent = pct;
}

void set_target_version(const char* v) {
    std::lock_guard<std::mutex> lk(g_status_mu);
    std::snprintf(g_status.target_version, sizeof(g_status.target_version),
                  "%s", v ? v : "");
}

// ---- NVS helpers --------------------------------------------------------

uint8_t ns_get_u8(const char* key, uint8_t dflt) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return dflt;
    uint8_t v = dflt;
    (void)nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v;
}

esp_err_t ns_set_u8(const char* key, uint8_t v) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t ns_get_str(const char* key, char* buf, size_t buf_len) {
    if (!buf || buf_len == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = buf_len;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err;
}

esp_err_t ns_set_str(const char* key, const char* v) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, v ? v : "");
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

void bump_rollback_count() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t n = 0;
    (void)nvs_get_u32(h, "rollback_cnt", &n);
    n++;
    nvs_set_u32(h, "rollback_cnt", n);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(kTag, "bootloader rolled us back — rollback_count=%lu",
             (unsigned long)n);
}

// ---- SemVer-lite compare ------------------------------------------------
// Parses "vX.Y.Z" (or "X.Y.Z"), ignores anything after Z. Returns -1/0/1.
// On parse failure, falls back to strcmp.

bool parse_semver(const char* s, int out[3]) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    int a = 0, b = 0, c = 0;
    int n = std::sscanf(s, "%d.%d.%d", &a, &b, &c);
    if (n < 1) return false;
    out[0] = a; out[1] = b; out[2] = c;
    return true;
}

int semver_cmp(const char* a, const char* b) {
    int va[3] = {0}, vb[3] = {0};
    bool pa = parse_semver(a, va);
    bool pb = parse_semver(b, vb);
    if (!pa || !pb) return std::strcmp(a ? a : "", b ? b : "");
    for (int i = 0; i < 3; ++i) {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return 1;
    }
    return 0;
}

// ---- HTTP fetch (latest.json) -------------------------------------------

// Returns malloc'd null-terminated buffer on success, nullptr on failure.
// Caller frees.
char* http_get_string(const char* url) {
    esp_http_client_config_t cfg = {};
    cfg.url                = url;
    cfg.crt_bundle_attach  = esp_crt_bundle_attach;
    cfg.timeout_ms         = 15000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return nullptr;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return nullptr;
    }
    int64_t clen = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(kTag, "latest.json status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }
    size_t cap = (clen > 0 && clen < 32 * 1024) ? (size_t)clen + 1 : 4096;
    size_t used = 0;
    char* buf = (char*)std::malloc(cap);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }
    for (;;) {
        if (used + 512 >= cap) {
            size_t ncap = cap * 2;
            char* nbuf = (char*)std::realloc(buf, ncap);
            if (!nbuf) { std::free(buf); buf = nullptr; break; }
            buf = nbuf; cap = ncap;
        }
        int n = esp_http_client_read(client, buf + used, cap - used - 1);
        if (n < 0) { std::free(buf); buf = nullptr; break; }
        if (n == 0) break;
        used += (size_t)n;
    }
    if (buf) buf[used] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return buf;
}

// ---- latest.json parsing ------------------------------------------------

struct Manifest {
    char tag[32];
    char url[kMaxUrlLen];
    char sha256[80];
    char chip_id[16];
    bool allow_downgrade;
};

bool parse_manifest(const char* json, Manifest& out) {
    out = {};
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    auto grab_str = [&](const char* k, char* dst, size_t cap) {
        cJSON* n = cJSON_GetObjectItem(root, k);
        if (cJSON_IsString(n) && n->valuestring) {
            std::snprintf(dst, cap, "%s", n->valuestring);
        }
    };
    grab_str("tag",     out.tag,     sizeof(out.tag));
    // Accept either "url" or "bin_url" (design doc uses both spellings).
    grab_str("url",     out.url,     sizeof(out.url));
    if (out.url[0] == '\0') grab_str("bin_url", out.url, sizeof(out.url));
    grab_str("sha256",  out.sha256,  sizeof(out.sha256));
    grab_str("chip_id", out.chip_id, sizeof(out.chip_id));
    cJSON* dg = cJSON_GetObjectItem(root, "allow_downgrade");
    out.allow_downgrade = cJSON_IsTrue(dg);
    cJSON_Delete(root);
    return out.tag[0] != '\0' && out.url[0] != '\0';
}

// ---- Download + install -------------------------------------------------

bool run_https_ota(const Manifest& m) {
    ESP_LOGI(kTag, "starting OTA download: %s", m.url);
    set_phase(Status::Phase::Downloading, "downloading", 0);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url               = m.url;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms        = 15000;
    http_cfg.keep_alive_enable = true;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config             = &http_cfg;
    ota_cfg.partial_http_download   = true;
    ota_cfg.max_http_request_size   = kChunkSize;

    esp_https_ota_handle_t h = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_https_ota_begin: %s", esp_err_to_name(err));
        set_phase(Status::Phase::Failed, "begin failed");
        return false;
    }

    esp_app_desc_t new_desc{};
    if (esp_https_ota_get_img_desc(h, &new_desc) == ESP_OK) {
        ESP_LOGI(kTag, "new image: project=%s version=%s",
                 new_desc.project_name, new_desc.version);
        // Cross-check the manifest tag against the app desc. Warn but
        // don't block — allows a manifest tag that differs cosmetically.
        if (m.tag[0] && std::strcmp(m.tag, new_desc.version) != 0) {
            ESP_LOGW(kTag, "manifest tag %s != image version %s",
                     m.tag, new_desc.version);
        }
    }

    int total = esp_https_ota_get_image_size(h);
    int last_pct = -1;
    while ((err = esp_https_ota_perform(h)) ==
           ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(h);
        if (total > 0) {
            int pct = (int)((int64_t)got * 100 / total);
            if (pct != last_pct) {
                set_progress(pct);
                last_pct = pct;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "ota perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        set_phase(Status::Phase::Failed, "download failed");
        return false;
    }
    set_phase(Status::Phase::Verifying, "verifying", 100);
    if (!esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGW(kTag, "download incomplete");
        esp_https_ota_abort(h);
        set_phase(Status::Phase::Failed, "incomplete");
        return false;
    }
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "ota finish failed: %s", esp_err_to_name(err));
        set_phase(Status::Phase::Failed, "verify failed");
        return false;
    }
    ESP_LOGI(kTag, "OTA image installed — awaiting reboot");
    return true;
}

// ---- Reboot readiness ---------------------------------------------------

bool default_reboot_ready() {
    // Reboot when no active connection is up. A stricter idle-time check
    // (design doc §4.2: "30 s no send/recv") would need an IConnection
    // hook that doesn't exist yet — this MVP is safe (never reboots
    // during an active session).
    auto* c = active_connection();
    if (!c) return true;
    return !c->is_connected();
}

void wait_and_reboot() {
    set_phase(Status::Phase::DeferredReboot, "waiting for idle", -1);
    int64_t start = esp_timer_get_time();
    for (;;) {
        RebootReadyFn fn = g_reboot_ready ? g_reboot_ready
                                          : &default_reboot_ready;
        if (fn()) break;
        int64_t elapsed = esp_timer_get_time() - start;
        if (elapsed >= kDeferredRebootTimeoutUs) {
            ESP_LOGW(kTag, "deferred-reboot timeout — rebooting anyway");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kDeferredRebootPollUs / 1000));
    }
    ESP_LOGI(kTag, "rebooting for OTA");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// ---- One poll cycle -----------------------------------------------------

// Compose base_url + "latest.json". base_url may or may not end in '/'.
void build_manifest_url(char* out, size_t out_len) {
    char base[kMaxUrlLen] = {};
    (void)ns_get_str("base_url", base, sizeof(base));
    if (base[0] == '\0') {
        std::snprintf(base, sizeof(base), "%s", CONFIG_TAB5_OTA_LATEST_URL);
    }
    size_t bl = std::strlen(base);
    const char* sep = (bl > 0 && base[bl - 1] == '/') ? "" : "/";
    std::snprintf(out, out_len, "%s%slatest.json", base, sep);
}

void do_poll_cycle(bool user_initiated) {
    char url[kMaxUrlLen + 20];
    build_manifest_url(url, sizeof(url));

    ESP_LOGI(kTag, "polling %s", url);
    set_phase(Status::Phase::Polling, "polling", -1);

    {
        std::lock_guard<std::mutex> lk(g_status_mu);
        g_status.last_check_us = esp_timer_get_time();
    }

    char* body = http_get_string(url);
    if (!body) {
        ESP_LOGW(kTag, "latest.json fetch failed");
        set_phase(Status::Phase::Failed, "fetch failed");
        g_fail_streak.fetch_add(1);
        return;
    }
    Manifest m{};
    bool ok = parse_manifest(body, m);
    std::free(body);
    if (!ok) {
        ESP_LOGW(kTag, "latest.json parse failed");
        set_phase(Status::Phase::Failed, "parse failed");
        g_fail_streak.fetch_add(1);
        return;
    }
    set_target_version(m.tag);
    ESP_LOGI(kTag, "manifest: tag=%s url=%s", m.tag, m.url);

    // Pinned tag kill switch.
    char pinned[32] = {};
    (void)ns_get_str("pinned_tag", pinned, sizeof(pinned));
    if (pinned[0] && std::strcmp(pinned, m.tag) != 0) {
        ESP_LOGI(kTag, "pinned_tag=%s != %s — skipping", pinned, m.tag);
        set_phase(Status::Phase::Idle, "pinned");
        return;
    }

    const esp_app_desc_t* cur = esp_app_get_description();
    const char* cur_v = cur ? cur->version : "";
    int cmp = semver_cmp(m.tag, cur_v);
    if (cmp == 0) {
        ESP_LOGI(kTag, "already up to date (%s)", cur_v);
        set_phase(Status::Phase::Idle, "up to date");
        g_fail_streak.store(0);
        return;
    }
    if (cmp < 0 && !m.allow_downgrade) {
        ESP_LOGI(kTag, "manifest tag %s older than %s — skipping", m.tag, cur_v);
        set_phase(Status::Phase::Idle, "older tag");
        return;
    }

    (void)user_initiated;  // no distinct behaviour yet (design doc §2.5)

    if (!run_https_ota(m)) {
        g_fail_streak.fetch_add(1);
        return;
    }
    g_fail_streak.store(0);
    wait_and_reboot();
}

// ---- Supervisor task ----------------------------------------------------

int64_t next_poll_delay_us(bool first_call) {
    // Base: CONFIG_TAB5_OTA_POLL_INTERVAL_H hours.
    int64_t base = (int64_t)CONFIG_TAB5_OTA_POLL_INTERVAL_H
                   * 3600LL * 1000 * 1000;
    if (first_call) base = kInitialDelayUs;

    // ±15 min jitter seeded from the STA MAC's low bits (fleet dispersion).
    uint8_t mac[6] = {};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t seed = (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5];
    // Randomise sign per call so a fleet doesn't lock-step even at
    // identical MAC-derived offsets.
    seed ^= esp_random();
    int64_t range = 2 * kPollJitterUs;
    int64_t off   = (int64_t)(seed % range) - kPollJitterUs;
    int64_t delay = base + off;
    if (delay < 60LL * 1000 * 1000) delay = 60LL * 1000 * 1000;
    return delay;
}

int64_t backoff_us_for_streak(int streak) {
    if (streak <= 0) return 0;
    int64_t d = kMinBackoffUs << (streak - 1);
    if (d > kMaxBackoffUs || d <= 0) d = kMaxBackoffUs;
    return d;
}

void supervisor_task(void*) {
    bool first = true;
    for (;;) {
        // Wait for a trigger, or for the poll deadline. When
        // auto-polling is off, block indefinitely on the trigger queue.
        bool auto_on = ns_get_u8("auto",
                                  CONFIG_TAB5_OTA_ENABLED ? 1 : 0) != 0;
        int64_t now = esp_timer_get_time();

        // Rollback cool-down: suppress auto poll for 24 h if we were
        // rolled back this boot.
        if (g_rollback_cooldown_until_us > now) auto_on = false;

        int streak = g_fail_streak.load();
        int64_t backoff = backoff_us_for_streak(streak);

        int64_t wait_us = auto_on
            ? next_poll_delay_us(first) + backoff
            : (int64_t)portMAX_DELAY * 1000;
        if (wait_us < 0 || wait_us > 60LL * 60 * 24 * 1000 * 1000) {
            wait_us = 24LL * 3600 * 1000 * 1000;
        }

        {
            std::lock_guard<std::mutex> lk(g_status_mu);
            g_status.backoff_until_us = auto_on ? (now + wait_us) : 0;
        }

        Trigger t = Trigger::Auto;
        TickType_t ticks = (wait_us / 1000) / portTICK_PERIOD_MS;
        if (ticks == 0) ticks = 1;
        BaseType_t got = xQueueReceive(g_trigger_q, &t, ticks);
        first = false;

        // Manual trigger overrides cool-down / auto-off.
        bool user_initiated = (got == pdTRUE && t == Trigger::Manual);
        if (!user_initiated && !auto_on) continue;

        do_poll_cycle(user_initiated);
    }
}

// ---- Post-boot health check --------------------------------------------

esp_ota_img_states_t running_slot_state() {
    const esp_partition_t* p = esp_ota_get_running_partition();
    if (!p) return ESP_OTA_IMG_UNDEFINED;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(p, &st);
    return st;
}

void health_timer_cb(void* /*arg*/) {
    // Very conservative healthy check: we've been up 120 s and got here
    // without the watchdog / crash resetting us. If Wi-Fi is down we
    // *still* mark valid — the alternative is bricking a device that
    // successfully installed but roams into a Wi-Fi dead zone during
    // the health window.
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(kTag, "mark_app_valid_cancel_rollback: %s",
             esp_err_to_name(err));
}

}  // namespace

// ---- Public API ---------------------------------------------------------

Status snapshot() {
    std::lock_guard<std::mutex> lk(g_status_mu);
    return g_status;
}

esp_err_t set_base_url(const char* url) {
    return ns_set_str("base_url", url);
}

void set_reboot_ready_fn(RebootReadyFn fn) { g_reboot_ready = fn; }

void kick(Trigger t) {
    if (!g_trigger_q) return;
    xQueueSend(g_trigger_q, &t, 0);
}

void arm_post_boot_health_check() {
    // Populate current_version early so the UI has something to show.
    const esp_app_desc_t* cur = esp_app_get_description();
    if (cur) {
        std::lock_guard<std::mutex> lk(g_status_mu);
        std::snprintf(g_status.current_version,
                      sizeof(g_status.current_version),
                      "%s", cur->version);
    }

    auto st = running_slot_state();
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(kTag, "running slot PENDING_VERIFY — arming 120s timer");
        const esp_timer_create_args_t args = {
            .callback = &health_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ota_health",
            .skip_unhandled_events = true,
        };
        esp_timer_handle_t h = nullptr;
        if (esp_timer_create(&args, &h) == ESP_OK) {
            esp_timer_start_once(h, kHealthWindowUs);
        }
    } else if (st == ESP_OTA_IMG_ABORTED) {
        bump_rollback_count();
        g_rollback_cooldown_until_us =
            esp_timer_get_time() + kRollbackCooldownUs;
    }
}

void start() {
    if (g_started.exchange(true)) return;
    if (!g_trigger_q) {
        g_trigger_q = xQueueCreate(4, sizeof(Trigger));
    }
    BaseType_t rc = xTaskCreate(&supervisor_task, "ota_sup",
                                8192, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (rc != pdPASS) {
        ESP_LOGE(kTag, "supervisor task create failed");
        g_started.store(false);
    }
}

}  // namespace tab5::ota

#else  // !CONFIG_TAB5_OTA_ENABLED

// Empty-shell namespace so the translation unit still exports something
// when OTA is disabled. Callers already guard their includes and calls
// with `#if CONFIG_TAB5_OTA_ENABLED`, but keeping ota_task.cpp in the
// build (unconditionally in main/CMakeLists.txt) simplifies the CMake
// side and lets us drop stubs here if a caller ever forgets a guard.

namespace tab5::ota {}

#endif  // CONFIG_TAB5_OTA_ENABLED
