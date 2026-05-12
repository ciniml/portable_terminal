#include "tofu.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace tab5::tofu {

namespace {

constexpr const char* kTag = "tofu";
constexpr const char* kNs  = "ssh_tofu";

// On-disk record. New entries always write the full 98-byte struct;
// legacy installs have 32-byte (fingerprint-only) blobs and are
// read-only — caller will see host="" / port=0.
struct StoredEntry {
    char     host[64];
    uint16_t port;
    uint8_t  fp[32];
} __attribute__((packed));

static_assert(sizeof(StoredEntry) == 64 + 2 + 32);

}  // namespace

void key_for(const char* host, uint16_t port, char out[16]) {
    uint32_t h = 2166136261u;
    for (const char* p = host; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
    h ^= port;
    snprintf(out, 16, "%08lx", static_cast<unsigned long>(h));
}

int check_or_record(const char* host, uint16_t port,
                    const uint8_t fp[32], bool* first_use_out) {
    char nvs_key[16];
    key_for(host, port, nvs_key);

    nvs_handle_t nh;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &nh);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(%s) -> %s", kNs, esp_err_to_name(err));
        return -2;
    }

    // Try to read the entry. Two valid sizes: legacy 32 bytes (fp only)
    // or new StoredEntry (98 bytes).
    uint8_t buf[sizeof(StoredEntry)] = {0};
    size_t stored_len = sizeof(buf);
    err = nvs_get_blob(nh, nvs_key, buf, &stored_len);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        StoredEntry e{};
        strncpy(e.host, host ? host : "", sizeof(e.host) - 1);
        e.port = port;
        memcpy(e.fp, fp, 32);
        err = nvs_set_blob(nh, nvs_key, &e, sizeof(e));
        if (err == ESP_OK) err = nvs_commit(nh);
        nvs_close(nh);
        if (first_use_out) *first_use_out = true;
        return err == ESP_OK ? 0 : -2;
    }
    if (err != ESP_OK) {
        nvs_close(nh);
        return -2;
    }
    nvs_close(nh);

    const uint8_t* stored_fp = nullptr;
    if (stored_len == 32) {
        stored_fp = buf;                  // legacy
    } else if (stored_len == sizeof(StoredEntry)) {
        stored_fp = reinterpret_cast<StoredEntry*>(buf)->fp;
    } else {
        return -2;
    }
    if (memcmp(stored_fp, fp, 32) != 0) return -1;
    if (first_use_out) *first_use_out = false;
    return 0;
}

int list_entries(std::vector<Entry>& out) {
    out.clear();
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, kNs,
                                   NVS_TYPE_BLOB, &it);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (err != ESP_OK) return -1;

    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READONLY, &nh) != ESP_OK) {
        nvs_release_iterator(it);
        return -1;
    }

    while (err == ESP_OK && it) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);

        Entry e{};
        strncpy(e.key, info.key, sizeof(e.key) - 1);

        uint8_t buf[sizeof(StoredEntry)] = {0};
        size_t sz = sizeof(buf);
        esp_err_t gr = nvs_get_blob(nh, info.key, buf, &sz);
        if (gr == ESP_OK) {
            if (sz == 32) {
                memcpy(e.fp, buf, 32);
                e.host[0] = '\0';
                e.port = 0;
            } else if (sz == sizeof(StoredEntry)) {
                auto* s = reinterpret_cast<StoredEntry*>(buf);
                strncpy(e.host, s->host, sizeof(e.host) - 1);
                e.port = s->port;
                memcpy(e.fp, s->fp, 32);
            }
            out.push_back(e);
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(nh);
    return static_cast<int>(out.size());
}

bool remove_by_key(const char* nvs_key) {
    if (!nvs_key || !*nvs_key) return false;
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READWRITE, &nh) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(nh, nvs_key);
    if (err == ESP_OK) err = nvs_commit(nh);
    nvs_close(nh);
    return err == ESP_OK;
}

}  // namespace tab5::tofu
