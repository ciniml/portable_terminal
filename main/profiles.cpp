#include "profiles.hpp"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace tab5 {

namespace {

constexpr const char* kTag = "profiles";
constexpr const char* kNs  = "conn";

constexpr uint8_t kSchemaVersion = 1;

void slot_key(int idx, char out[8]) {
    // Profile indices are bounded by CONFIG_TAB5_MAX_PROFILES <= 16; the
    // gcc format-truncation analyzer sees a generic int and complains,
    // so clamp the type explicitly.
    snprintf(out, 8, "p%u", static_cast<unsigned>(idx) & 0xFFu);
}

bool load_slot(nvs_handle_t nh, int idx, Profile* out) {
    char k[8];
    slot_key(idx, k);
    size_t sz = sizeof(*out);
    esp_err_t err = nvs_get_blob(nh, k, out, &sz);
    return err == ESP_OK && sz == sizeof(*out);
}

bool save_slot(nvs_handle_t nh, int idx, const Profile& p) {
    char k[8];
    slot_key(idx, k);
    return nvs_set_blob(nh, k, &p, sizeof(p)) == ESP_OK;
}

void erase_slot(nvs_handle_t nh, int idx) {
    char k[8];
    slot_key(idx, k);
    nvs_erase_key(nh, k);
}

// Build a default profile 0 from any compile-time Kconfig values. Returns
// false if neither SSH nor Telnet has a configured host (= nothing useful
// to seed).
bool build_seed_profile(Profile* out) {
    *out = Profile{};
#if CONFIG_TAB5_SSH_ENABLED
    if (CONFIG_TAB5_SSH_HOST[0]) {
        snprintf(out->name, sizeof(out->name), "ssh:%.20s",
                 CONFIG_TAB5_SSH_HOST);
        out->proto = ConnProto::SSH;
        out->port  = static_cast<uint16_t>(CONFIG_TAB5_SSH_PORT);
        strncpy(out->host,     CONFIG_TAB5_SSH_HOST,     sizeof(out->host) - 1);
        strncpy(out->user,     CONFIG_TAB5_SSH_USER,     sizeof(out->user) - 1);
        strncpy(out->password, CONFIG_TAB5_SSH_PASSWORD, sizeof(out->password) - 1);
#if CONFIG_TAB5_SSH_PUBKEY_AUTH
        out->auth = SshAuth::Pubkey;
#else
        out->auth = SshAuth::Password;
#endif
        return true;
    }
#endif
#if CONFIG_TAB5_TELNET_ENABLED
    if (CONFIG_TAB5_TELNET_HOST[0]) {
        snprintf(out->name, sizeof(out->name), "telnet:%.20s",
                 CONFIG_TAB5_TELNET_HOST);
        out->proto = ConnProto::Telnet;
        out->port  = static_cast<uint16_t>(CONFIG_TAB5_TELNET_PORT);
        strncpy(out->host, CONFIG_TAB5_TELNET_HOST, sizeof(out->host) - 1);
        return true;
    }
#endif
    return false;
}

}  // namespace

Profiles profiles;

int Profiles::max() const { return CONFIG_TAB5_MAX_PROFILES; }

void Profiles::init() {
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(%s) failed; profile store unavailable", kNs);
        return;
    }

    uint8_t ver = 0;
    if (nvs_get_u8(nh, "ver", &ver) != ESP_OK || ver != kSchemaVersion) {
        // Wipe any stale data and seed.
        for (int i = 0; i < max(); ++i) erase_slot(nh, i);
        nvs_erase_key(nh, "count");
        nvs_erase_key(nh, "selected");
        nvs_set_u8(nh, "ver", kSchemaVersion);

        Profile seed{};
        if (build_seed_profile(&seed)) {
            save_slot(nh, 0, seed);
            nvs_set_u8(nh, "count", 1);
            nvs_set_u8(nh, "selected", 0);
            ESP_LOGI(kTag, "seeded profile 0 from Kconfig: %s", seed.name);
        } else {
            nvs_set_u8(nh, "count", 0);
            nvs_set_u8(nh, "selected", 0);
        }
        nvs_commit(nh);
    }

    uint8_t c = 0, s = 0;
    nvs_get_u8(nh, "count", &c);
    nvs_get_u8(nh, "selected", &s);
    count_    = c;
    selected_ = (s < c) ? s : 0;
    nvs_close(nh);
    ESP_LOGI(kTag, "%d profile(s), selected=%d, max=%d",
             count_, selected_, max());
}

std::optional<Profile> Profiles::get(int idx) const {
    if (idx < 0 || idx >= count_) return std::nullopt;
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READONLY, &nh) != ESP_OK) return std::nullopt;
    Profile p{};
    bool ok = load_slot(nh, idx, &p);
    nvs_close(nh);
    if (!ok) return std::nullopt;
    return p;
}

int Profiles::add(const Profile& p) {
    if (count_ >= max()) return -1;
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READWRITE, &nh) != ESP_OK) return -1;
    int idx = count_;
    bool ok = save_slot(nh, idx, p);
    if (ok) {
        count_ = idx + 1;
        nvs_set_u8(nh, "count", static_cast<uint8_t>(count_));
        nvs_commit(nh);
    }
    nvs_close(nh);
    return ok ? idx : -1;
}

bool Profiles::update(int idx, const Profile& p) {
    if (idx < 0 || idx >= count_) return false;
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READWRITE, &nh) != ESP_OK) return false;
    bool ok = save_slot(nh, idx, p);
    if (ok) nvs_commit(nh);
    nvs_close(nh);
    return ok;
}

bool Profiles::remove(int idx) {
    if (idx < 0 || idx >= count_) return false;
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READWRITE, &nh) != ESP_OK) return false;
    // Shift slots [idx+1 .. count_-1] down by one to keep indices dense.
    Profile shifted{};
    for (int i = idx; i < count_ - 1; ++i) {
        if (!load_slot(nh, i + 1, &shifted)) {
            nvs_close(nh);
            return false;
        }
        save_slot(nh, i, shifted);
    }
    erase_slot(nh, count_ - 1);
    count_ -= 1;
    if (selected_ >= count_) selected_ = (count_ > 0) ? count_ - 1 : 0;
    nvs_set_u8(nh, "count",    static_cast<uint8_t>(count_));
    nvs_set_u8(nh, "selected", static_cast<uint8_t>(selected_));
    nvs_commit(nh);
    nvs_close(nh);
    return true;
}

bool Profiles::select(int idx) {
    if (idx < 0 || idx >= count_) return false;
    nvs_handle_t nh;
    if (nvs_open(kNs, NVS_READWRITE, &nh) != ESP_OK) return false;
    selected_ = idx;
    nvs_set_u8(nh, "selected", static_cast<uint8_t>(idx));
    nvs_commit(nh);
    nvs_close(nh);
    return true;
}

}  // namespace tab5
