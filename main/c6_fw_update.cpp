#include "c6_fw_update.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// esp_hosted public API
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"

// rpc_ota_* live inside the host driver's rpc wrap layer. The header
// (host/drivers/rpc/wrap/rpc_wrap.h) is on the component's PRIV_INCLUDE_DIRS
// only, so we forward-declare the four entry points here — same pattern as
// c6_updater/main/main.c. They are non-static in the .c file so the
// linker resolves them just fine.
extern "C" {
esp_err_t rpc_ota_begin(void);
esp_err_t rpc_ota_write(uint8_t* ota_data, uint32_t ota_data_len);
esp_err_t rpc_ota_end(void);
esp_err_t rpc_ota_activate(void);
}

namespace tab5::c6_fw {

namespace {

constexpr const char* kTag = "c6_fw";

// Esp_hosted 1.4.x's rpc layer streams the OTA payload in 1400-byte
// chunks (see c6_updater/main/main.c). Keeping the same chunk size on
// the 2.x side avoids stressing the RPC framing.
constexpr size_t kOtaChunkSize = 1400;

// network_adapter.bin is an app image (not a full flash image): bootloader
// and partition table live in separate partitions on the C6. esp_app_desc_t
// therefore sits at the start of the .rodata segment, which in turn is at
// offset 0x20 from the start of the blob (esp_image_header_t (24 B) + one
// esp_image_segment_header_t (8 B)).
constexpr size_t kAppDescOffset = 0x20;

// Read just enough to cover the app desc (256 B) plus the leading headers.
constexpr size_t kHeaderRead = kAppDescOffset + sizeof(esp_app_desc_t);

const esp_partition_t* find_c6_partition() {
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        "c6_fw");
}

bool parse_version_triplet(const char* s, uint8_t* maj, uint8_t* min,
                           uint8_t* pat) {
    // esp_app_desc_t::version is set from PROJECT_VER (version.txt in CI).
    // Accept "X.Y.Z", "vX.Y.Z", or "vX.Y.Z-anything". Anything else → fail.
    if (!s) return false;
    if (*s == 'v' || *s == 'V') ++s;
    unsigned int a = 0, b = 0, c = 0;
    if (std::sscanf(s, "%u.%u.%u", &a, &b, &c) != 3) return false;
    if (a > 255 || b > 255 || c > 255) return false;
    *maj = static_cast<uint8_t>(a);
    *min = static_cast<uint8_t>(b);
    *pat = static_cast<uint8_t>(c);
    return true;
}

bool is_blank(const uint8_t* buf, size_t n) {
    // A freshly-erased partition reads back as 0xFF; treat that as "no
    // embedded fw" rather than try to parse it.
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] != 0xFF) return false;
    }
    return true;
}

}  // namespace

bool read_embedded_info(EmbeddedInfo* out) {
    if (!out) return false;
    const esp_partition_t* part = find_c6_partition();
    if (!part) {
        ESP_LOGI(kTag, "no c6_fw partition in table — auto-update disabled");
        return false;
    }

    uint8_t buf[kHeaderRead] = {};
    esp_err_t err = esp_partition_read(part, 0, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_partition_read header failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    if (is_blank(buf, sizeof(buf))) {
        ESP_LOGI(kTag, "c6_fw partition is blank — no embedded slave fw");
        return false;
    }

    const auto* desc = reinterpret_cast<const esp_app_desc_t*>(buf + kAppDescOffset);
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGW(kTag, "c6_fw blob has wrong magic 0x%08lx (expected 0x%08lx)",
                 static_cast<unsigned long>(desc->magic_word),
                 static_cast<unsigned long>(ESP_APP_DESC_MAGIC_WORD));
        return false;
    }

    // version[] and project_name[] are fixed-width and not guaranteed
    // NUL-terminated. Snprintf into a slightly larger out buffer so we
    // get a clean NUL regardless.
    std::snprintf(out->version_str, sizeof(out->version_str),
                  "%.*s", static_cast<int>(sizeof(desc->version)),
                  desc->version);
    std::snprintf(out->project_name, sizeof(out->project_name),
                  "%.*s", static_cast<int>(sizeof(desc->project_name)),
                  desc->project_name);

    if (!parse_version_triplet(out->version_str,
                               &out->major, &out->minor, &out->patch)) {
        ESP_LOGW(kTag, "embedded c6 fw version '%s' — can't parse X.Y.Z; "
                 "auto-update disabled", out->version_str);
        return false;
    }
    return true;
}

namespace {

// Map of the c6_fw partition for a chunked sequential read. We use
// esp_partition_mmap so chunks are read directly out of flash cache
// without staging a 1.5 MB buffer in DRAM.
struct PartitionMmap {
    const esp_partition_t* part{nullptr};
    esp_partition_mmap_handle_t handle{};
    const void* base{nullptr};
    size_t size{0};

    ~PartitionMmap() {
        if (base) {
            esp_partition_munmap(handle);
        }
    }
};

// Find the end of the actual image inside the partition (everything past
// is erased 0xFF padding). Scan from the tail in 4 KB chunks so we don't
// allocate the whole partition just to trim padding.
size_t trim_trailing_blank(const uint8_t* p, size_t size) {
    constexpr size_t kStep = 4096;
    size_t end = size;
    while (end > 0) {
        size_t start = (end >= kStep) ? (end - kStep) : 0;
        for (size_t i = end; i > start; --i) {
            if (p[i - 1] != 0xFF) return i;
        }
        if (start == 0) break;
        end = start;
    }
    return 0;
}

bool stream_ota(const esp_partition_t* part) {
    PartitionMmap m;
    m.part = part;
    m.size = part->size;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &m.base, &m.handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_partition_mmap failed: %s", esp_err_to_name(err));
        return false;
    }

    const auto* p = static_cast<const uint8_t*>(m.base);
    const size_t payload_len = trim_trailing_blank(p, m.size);
    if (payload_len < 1024) {
        ESP_LOGE(kTag, "c6_fw payload implausibly small (%u B)",
                 static_cast<unsigned>(payload_len));
        return false;
    }
    ESP_LOGI(kTag, "starting OTA — payload=%u B (partition=%u B)",
             static_cast<unsigned>(payload_len),
             static_cast<unsigned>(m.size));

    int rc = rpc_ota_begin();
    if (rc != ESP_OK) {
        ESP_LOGE(kTag, "rpc_ota_begin failed: %d", rc);
        return false;
    }

    size_t off = 0;
    size_t chunks_since_log = 0;
    while (off < payload_len) {
        size_t n = payload_len - off;
        if (n > kOtaChunkSize) n = kOtaChunkSize;
        // rpc_ota_write signature takes a non-const pointer but the
        // implementation doesn't mutate the buffer — const_cast is safe.
        rc = rpc_ota_write(const_cast<uint8_t*>(p + off),
                           static_cast<uint32_t>(n));
        if (rc != ESP_OK) {
            ESP_LOGE(kTag, "rpc_ota_write at %u/%u failed: %d",
                     static_cast<unsigned>(off),
                     static_cast<unsigned>(payload_len), rc);
            (void)rpc_ota_end();
            return false;
        }
        off += n;
        if (++chunks_since_log >= 32 || off == payload_len) {
            ESP_LOGI(kTag, "OTA progress %u / %u (%u%%)",
                     static_cast<unsigned>(off),
                     static_cast<unsigned>(payload_len),
                     static_cast<unsigned>(
                         (static_cast<uint64_t>(off) * 100) / payload_len));
            chunks_since_log = 0;
        }
    }

    rc = rpc_ota_end();
    if (rc != ESP_OK) {
        ESP_LOGE(kTag, "rpc_ota_end failed: %d", rc);
        return false;
    }

    // 2.x adds an explicit activate step. Older 1.4 didn't have this and
    // c6_updater therefore doesn't call it; the 2.x slave commits to the
    // new image on activate + reset, so call it now.
    rc = rpc_ota_activate();
    if (rc != ESP_OK) {
        // Some intermediate 2.x slave builds treat activate as a no-op
        // and return non-zero. Don't fail the whole update for it — the
        // P4 restart we trigger right after will boot the slave into its
        // new firmware anyway.
        ESP_LOGW(kTag, "rpc_ota_activate returned %d (continuing)", rc);
    }
    return true;
}

}  // namespace

int update_if_needed() {
    // The esp_hosted constructor runs setup_transport very early in boot,
    // but the slave handshake only completes after the C6 has been
    // powered up (wifi_setup.cpp toggles WLAN_PWR_EN explicitly before
    // calling us). Poll the fwversion query for up to ~6 s so a late-
    // arriving handshake doesn't make us skip the update on every cold
    // boot.
    esp_hosted_coprocessor_fwver_t cur = {};
    esp_err_t err = ESP_FAIL;
    constexpr int kRetryMs = 200;
    constexpr int kMaxWaitMs = 6000;
    for (int waited = 0; waited <= kMaxWaitMs; waited += kRetryMs) {
        err = esp_hosted_get_coprocessor_fwversion(&cur);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(kRetryMs));
    }
    if (err != ESP_OK) {
        // Most likely cause: transport never came up. This happens when
        // the C6 is still running the M5 factory 1.4.x slave fw, which
        // is wire-incompatible with our esp_hosted 2.x host. We can't
        // OTA-update from 1.4.x → 2.x ourselves — that initial step
        // still needs c6_updater/. Surface a clear pointer.
        ESP_LOGE(kTag, "fwver query failed (%s) after waiting — "
                 "esp_hosted SDIO link is not up.", esp_err_to_name(err));
        ESP_LOGE(kTag, "C6 likely still on the factory 1.4.x firmware. "
                 "Run ./c6_updater/updater.sh once to upgrade the C6 "
                 "slave to 2.x; after that this boot-time auto-update "
                 "will keep it current.");
        return 0;
    }
    ESP_LOGI(kTag, "C6 slave reports version %lu.%lu.%lu",
             static_cast<unsigned long>(cur.major1),
             static_cast<unsigned long>(cur.minor1),
             static_cast<unsigned long>(cur.patch1));

    EmbeddedInfo emb;
    if (!read_embedded_info(&emb)) {
        ESP_LOGI(kTag, "no embedded slave fw — skipping auto-update");
        return 0;
    }
    ESP_LOGI(kTag, "embedded slave fw: %u.%u.%u (%s, project=%s)",
             emb.major, emb.minor, emb.patch,
             emb.version_str, emb.project_name);

    // Strict-newer semver gate. Equal → skip (no-op churn); older → skip
    // (don't downgrade a slave that's somehow ahead of us).
    auto tup_emb = std::make_tuple(emb.major, emb.minor, emb.patch);
    auto tup_cur = std::make_tuple(
        static_cast<uint8_t>(cur.major1 & 0xFF),
        static_cast<uint8_t>(cur.minor1 & 0xFF),
        static_cast<uint8_t>(cur.patch1 & 0xFF));
    if (!(tup_emb > tup_cur)) {
        ESP_LOGI(kTag, "C6 firmware up to date (cur=%lu.%lu.%lu, "
                 "embedded=%u.%u.%u) — no action",
                 static_cast<unsigned long>(cur.major1),
                 static_cast<unsigned long>(cur.minor1),
                 static_cast<unsigned long>(cur.patch1),
                 emb.major, emb.minor, emb.patch);
        return 0;
    }

    ESP_LOGW(kTag, "C6 update available: %lu.%lu.%lu → %u.%u.%u; "
             "starting OTA",
             static_cast<unsigned long>(cur.major1),
             static_cast<unsigned long>(cur.minor1),
             static_cast<unsigned long>(cur.patch1),
             emb.major, emb.minor, emb.patch);

    const esp_partition_t* part = find_c6_partition();
    if (!part) {
        ESP_LOGE(kTag, "c6_fw partition vanished between header read and OTA");
        return -1;
    }

    if (!stream_ota(part)) {
        return -1;
    }
    ESP_LOGW(kTag, "C6 firmware updated successfully to %u.%u.%u",
             emb.major, emb.minor, emb.patch);
    return 1;
}

}  // namespace tab5::c6_fw
