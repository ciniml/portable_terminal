// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

// GATT server for the Tab5 BLE provisioning service.
//
// UUID layout (base b5a50000-9c1b-4f3d-8a2e-1f6c4e9b7d0a; third byte
// varies per characteristic):
//   0001 KeyExchange       (plaintext, RW)
//   0002 WifiSsid          (encrypted, RW)
//   0003 WifiPsk           (encrypted, W only)
//   0004 TailscaleAuthKey  (encrypted, W only)
//   0005 TailscaleHostname (encrypted, RW)
//   0006 SshHost           (encrypted, RW)
//   0007 SshPort           (encrypted, RW; uint16 LE)
//   0008 SshUser           (encrypted, RW)
//   0009 SshPassword       (encrypted, W only)
//   000a AuthPassword      (encrypted, W only)
//   000b Status            (plaintext, R + NOTIFY; 3 bytes)
//   000c Apply             (encrypted, W only; triggers esp_restart)
//
// Confidentiality is provided by an application-layer X25519 + AES-256-GCM
// session (see crypto.cpp). BLE link encryption (_ENC flags) is intentionally
// not used — NimBLE Just Works + Web Bluetooth was unreliable in stackchan's
// experience. KeyExchange + Status are the only plaintext characteristics;
// all other reads/writes carry [12B nonce][ciphertext][16B tag] and require
// an established session.

#include "gatt_service.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

#include <mbedtls/sha256.h>

#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include "ble_config/ble_config.hpp"
#include "ble_config/crypto.hpp"

// Reach into main/ so we can apply provisioned values directly into the
// existing persistence modules (wifi_config, profiles). The component
// PRIV_REQUIRES main in CMakeLists.txt to make these headers visible.
#include "profiles.hpp"
#include "wifi_config.hpp"

namespace tab5::ble_config::gatt {

namespace {

constexpr const char* kTag = "ble-cfg-gatt";

// --- UUIDs ---------------------------------------------------------------
// 128-bit, stored little-endian (byte[0] = LSB of the 128-bit value).
// Base = b5a50000-9c1b-4f3d-8a2e-1f6c4e9b7d0a.
// LSB-first byte sequence of the base:
//   0a 7d 9b 4e 6c 1f 2e 8a  3d 4f 1b 9c 00 00 a5 b5
// Third byte (offset 12 in LE form) is the per-chr discriminator.

#define TAB5_BLE_UUID128(suffix)                                                       \
    BLE_UUID128_INIT(0x0a, 0x7d, 0x9b, 0x4e, 0x6c, 0x1f, 0x2e, 0x8a, 0x3d, 0x4f, 0x1b, \
                     0x9c, (suffix), 0x00, 0xa5, 0xb5)

static const ble_uuid128_t kSvcUuid              = TAB5_BLE_UUID128(0x00);
static const ble_uuid128_t kKeyExchangeUuid      = TAB5_BLE_UUID128(0x01);
static const ble_uuid128_t kWifiSsidUuid         = TAB5_BLE_UUID128(0x02);
static const ble_uuid128_t kWifiPskUuid          = TAB5_BLE_UUID128(0x03);
static const ble_uuid128_t kTailscaleAuthKeyUuid = TAB5_BLE_UUID128(0x04);
static const ble_uuid128_t kTailscaleHostnameUuid= TAB5_BLE_UUID128(0x05);
static const ble_uuid128_t kSshHostUuid          = TAB5_BLE_UUID128(0x06);
static const ble_uuid128_t kSshPortUuid          = TAB5_BLE_UUID128(0x07);
static const ble_uuid128_t kSshUserUuid          = TAB5_BLE_UUID128(0x08);
static const ble_uuid128_t kSshPasswordUuid      = TAB5_BLE_UUID128(0x09);
static const ble_uuid128_t kAuthPasswordUuid     = TAB5_BLE_UUID128(0x0a);
static const ble_uuid128_t kStatusUuid           = TAB5_BLE_UUID128(0x0b);
static const ble_uuid128_t kApplyUuid            = TAB5_BLE_UUID128(0x0c);

// --- State ---------------------------------------------------------------

static SemaphoreHandle_t g_mutex = nullptr;

// Per-connection crypto session. Reset on BLE_GAP_EVENT_DISCONNECT by
// ble_config.cpp.
static crypto::Session g_session;

// Latest status snapshot pushed by main via notify_status().
static Status g_status{false, false, false};

// Subscriber tracking for the Status NOTIFY.
static uint16_t g_status_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_status_subscribed = false;

// Cached current values surfaced via encrypted READ on the RW chars. We
// snapshot wifi_config / profiles on init() and refresh on every WRITE so
// reads return up-to-date values without round-tripping through NVS each
// time.
struct CurrentValues {
    std::string wifi_ssid;
    std::string tailscale_hostname;
    std::string ssh_host;
    std::uint16_t ssh_port = 22;
    std::string ssh_user;
};
static CurrentValues g_current{};

// Val handles populated by NimBLE on GATT registration.
static std::uint16_t g_kx_handle = 0;
static std::uint16_t g_ssid_handle = 0;
static std::uint16_t g_psk_handle = 0;
static std::uint16_t g_ts_auth_handle = 0;
static std::uint16_t g_ts_host_handle = 0;
static std::uint16_t g_ssh_host_handle = 0;
static std::uint16_t g_ssh_port_handle = 0;
static std::uint16_t g_ssh_user_handle = 0;
static std::uint16_t g_ssh_pw_handle = 0;
static std::uint16_t g_auth_pw_handle = 0;
static std::uint16_t g_status_handle = 0;
static std::uint16_t g_apply_handle = 0;

// Restart timer used by the Apply chr — fired ~500 ms after the WRITE so
// the ATT response can flush before esp_restart() yanks the radio.
static esp_timer_handle_t g_restart_timer = nullptr;

// --- NVS helpers ---------------------------------------------------------

constexpr const char* kNvsTailscale = "tailscale";
constexpr const char* kNvsTailscaleAuth = "auth_key";
constexpr const char* kNvsTailscaleHost = "hostname";
constexpr const char* kNvsBleCfg = "ble_cfg";
constexpr const char* kNvsAuthSalt = "auth_salt";

static bool nvs_set_str_in(const char* ns, const char* key, const std::string& value)
{
    nvs_handle_t nh;
    if (nvs_open(ns, NVS_READWRITE, &nh) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(nh, key, value.c_str());
    if (err == ESP_OK) {
        nvs_commit(nh);
    }
    nvs_close(nh);
    return err == ESP_OK;
}

static std::string nvs_get_str_in(const char* ns, const char* key)
{
    nvs_handle_t nh;
    if (nvs_open(ns, NVS_READONLY, &nh) != ESP_OK) return {};
    std::size_t sz = 0;
    if (nvs_get_str(nh, key, nullptr, &sz) != ESP_OK || sz == 0) {
        nvs_close(nh);
        return {};
    }
    std::string out(sz, '\0');
    if (nvs_get_str(nh, key, out.data(), &sz) != ESP_OK) {
        nvs_close(nh);
        return {};
    }
    if (!out.empty() && out.back() == '\0') out.pop_back();
    nvs_close(nh);
    return out;
}

// --- Persistence shims ---------------------------------------------------
//
// Wi-Fi / SSH-profile / Tailscale credentials are written into the existing
// modules so the rest of the firmware picks them up on the next boot.
// Caller must hold g_mutex for the duration so concurrent writes don't
// observe a half-applied profile.

static void persist_wifi_pair_locked(const std::optional<std::string>& ssid_in,
                                     const std::optional<std::string>& psk_in)
{
    auto wc = wifi_config::get();
    if (ssid_in) {
        std::snprintf(wc.ssid, sizeof(wc.ssid), "%s", ssid_in->c_str());
    }
    if (psk_in) {
        std::snprintf(wc.psk, sizeof(wc.psk), "%s", psk_in->c_str());
    }
    if (!wifi_config::set(wc)) {
        ESP_LOGW(kTag, "wifi_config::set failed");
    }
}

// Take whatever lives in profile 0 (creating it if the store is empty) and
// patch the fields the BLE central wrote. SshAuth is fixed to Password in
// this MVP — pubkey provisioning is out of scope.
static void persist_ssh_profile_locked(const std::optional<std::string>& host,
                                       const std::optional<std::uint16_t>& port,
                                       const std::optional<std::string>& user,
                                       const std::optional<std::string>& password)
{
    Profile p{};
    bool have = false;
    if (profiles.count() > 0) {
        auto cur = profiles.get(0);
        if (cur) {
            p = *cur;
            have = true;
        }
    }
    if (!have) {
        std::snprintf(p.name, sizeof(p.name), "ble-provisioned");
        p.proto = ConnProto::SSH;
        p.port = 22;
        p.auth = SshAuth::Password;
    }
    if (host)     std::snprintf(p.host, sizeof(p.host), "%s", host->c_str());
    if (port)     p.port = *port;
    if (user)     std::snprintf(p.user, sizeof(p.user), "%s", user->c_str());
    if (password) std::snprintf(p.password, sizeof(p.password), "%s", password->c_str());
    // BLE-provisioning is currently SSH-only.
    p.proto = ConnProto::SSH;
    p.auth  = SshAuth::Password;

    if (have) {
        if (!profiles.update(0, p)) {
            ESP_LOGW(kTag, "profiles::update(0) failed");
        }
    } else {
        int idx = profiles.add(p);
        if (idx < 0) {
            ESP_LOGW(kTag, "profiles::add failed");
        } else {
            profiles.select(idx);
        }
    }
}

static void persist_tailscale_locked(const std::optional<std::string>& auth_key,
                                     const std::optional<std::string>& hostname)
{
    if (auth_key) {
        if (!nvs_set_str_in(kNvsTailscale, kNvsTailscaleAuth, *auth_key)) {
            ESP_LOGW(kTag, "tailscale auth_key NVS write failed");
        }
    }
    if (hostname) {
        if (!nvs_set_str_in(kNvsTailscale, kNvsTailscaleHost, *hostname)) {
            ESP_LOGW(kTag, "tailscale hostname NVS write failed");
        }
    }
}

static void persist_auth_password_locked(const std::string& pw)
{
    nvs_handle_t nh;
    if (nvs_open(kNvsBleCfg, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGW(kTag, "ble_cfg NVS open failed");
        return;
    }
    if (pw.empty()) {
        nvs_erase_key(nh, kNvsAuthSalt);
        nvs_commit(nh);
        ESP_LOGI(kTag, "BLE auth gate cleared");
    } else {
        std::array<std::uint8_t, 32> hash{};
        // SHA-256(password). is224=0 selects SHA-256.
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(pw.data()), pw.size(),
                       hash.data(), 0);
        if (nvs_set_blob(nh, kNvsAuthSalt, hash.data(), hash.size()) == ESP_OK) {
            nvs_commit(nh);
            ESP_LOGI(kTag, "BLE auth gate installed (takes effect next session)");
        } else {
            ESP_LOGW(kTag, "auth_salt NVS write failed");
        }
    }
    nvs_close(nh);
}

// --- GATT access callback ------------------------------------------------

static bool append_encrypted(struct os_mbuf* om, std::span<const std::uint8_t> plain)
{
    auto enc = g_session.encrypt(plain);
    if (!enc) return false;
    return os_mbuf_append(om, enc->data(), enc->size()) == 0;
}

static int read_encrypted_string(struct os_mbuf* om, const std::string& s)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (!g_session.is_established()) {
        xSemaphoreGive(g_mutex);
        // Pre-handshake reads return 0 bytes per the design brief.
        return 0;
    }
    const bool ok = append_encrypted(
        om, {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
    xSemaphoreGive(g_mutex);
    return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static void restart_cb(void* /*arg*/)
{
    ESP_LOGI(kTag, "Apply: restarting now");
    esp_restart();
}

static int gatt_access_cb(uint16_t /*conn_handle*/, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* /*arg*/)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // KeyExchange — plaintext bootstrap. Lazily generate the device's
        // ephemeral keypair on first read and return the 32-byte public key.
        if (attr_handle == g_kx_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            auto pub = g_session.ensure_device_keypair();
            xSemaphoreGive(g_mutex);
            if (!pub) {
                ESP_LOGW(kTag, "ensure_device_keypair failed");
                return BLE_ATT_ERR_UNLIKELY;
            }
            int rc = os_mbuf_append(ctxt->om, pub->data(), pub->size());
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        // Status — plaintext 3-byte struct [wifi][tailscale][ssh].
        if (attr_handle == g_status_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            const std::array<std::uint8_t, 3> bytes{
                static_cast<std::uint8_t>(g_status.wifi_connected ? 1 : 0),
                static_cast<std::uint8_t>(g_status.tailscale_up ? 1 : 0),
                static_cast<std::uint8_t>(g_status.ssh_up ? 1 : 0)};
            xSemaphoreGive(g_mutex);
            int rc = os_mbuf_append(ctxt->om, bytes.data(), bytes.size());
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        // Encrypted RW string chars — pre-handshake reads return empty.
        if (attr_handle == g_ssid_handle)    return read_encrypted_string(ctxt->om, g_current.wifi_ssid);
        if (attr_handle == g_ts_host_handle) return read_encrypted_string(ctxt->om, g_current.tailscale_hostname);
        if (attr_handle == g_ssh_host_handle) return read_encrypted_string(ctxt->om, g_current.ssh_host);
        if (attr_handle == g_ssh_user_handle) return read_encrypted_string(ctxt->om, g_current.ssh_user);
        if (attr_handle == g_ssh_port_handle) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (!g_session.is_established()) {
                xSemaphoreGive(g_mutex);
                return 0;
            }
            const std::array<std::uint8_t, 2> le{
                static_cast<std::uint8_t>(g_current.ssh_port & 0xff),
                static_cast<std::uint8_t>((g_current.ssh_port >> 8) & 0xff)};
            const bool ok = append_encrypted(ctxt->om, {le.data(), le.size()});
            xSemaphoreGive(g_mutex);
            return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        // Write-only chars READ returns 0 bytes per design.
        if (attr_handle == g_psk_handle || attr_handle == g_ts_auth_handle ||
            attr_handle == g_ssh_pw_handle || attr_handle == g_auth_pw_handle ||
            attr_handle == g_apply_handle) {
            return 0;
        }
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Static scratch (same rationale as stackchan): a 1 KiB buffer would
        // eat a quarter of the NimBLE host task stack. All GATT access cbs
        // run on the single NimBLE host task — no reentrancy.
        static std::array<std::uint8_t, 512> buf;
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf.data(),
                                     static_cast<uint16_t>(buf.size()), &out_len);
        if (rc != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;

        // KeyExchange WRITE completes the X25519 handshake. Plaintext, 32 B.
        if (attr_handle == g_kx_handle) {
            if (out_len != 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            auto result = g_session.complete_handshake(
                std::span<const std::uint8_t, 32>{buf.data(), 32});
            xSemaphoreGive(g_mutex);
            if (!result) {
                ESP_LOGW(kTag, "complete_handshake failed: %d",
                         static_cast<int>(result.error()));
                return result.error() == Error::CryptoNotReady
                           ? BLE_ATT_ERR_UNLIKELY
                           : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            return 0;
        }

        // All other writes require an established session and carry
        // [12B nonce][ciphertext][16B tag]. Decrypt before validating lengths.
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        if (!g_session.is_established()) {
            xSemaphoreGive(g_mutex);
            return BLE_ATT_ERR_UNLIKELY;
        }
        auto pt_result = g_session.decrypt({buf.data(), out_len});
        xSemaphoreGive(g_mutex);
        if (!pt_result) {
            ESP_LOGW(kTag, "decrypt failed on handle=%d", attr_handle);
            return BLE_ATT_ERR_UNLIKELY;
        }
        const std::vector<std::uint8_t>& pt = *pt_result;
        const std::string s(reinterpret_cast<const char*>(pt.data()), pt.size());

        if (attr_handle == g_ssid_handle) {
            if (pt.size() > 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_current.wifi_ssid = s;
            persist_wifi_pair_locked(s, std::nullopt);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_psk_handle) {
            if (pt.size() > 63) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            persist_wifi_pair_locked(std::nullopt, s);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_ts_auth_handle) {
            if (pt.size() > 256) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            persist_tailscale_locked(s, std::nullopt);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_ts_host_handle) {
            if (pt.size() > 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_current.tailscale_hostname = s;
            persist_tailscale_locked(std::nullopt, s);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_ssh_host_handle) {
            if (pt.size() > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_current.ssh_host = s;
            persist_ssh_profile_locked(s, std::nullopt, std::nullopt, std::nullopt);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_ssh_port_handle) {
            if (pt.size() != 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            const std::uint16_t port =
                static_cast<std::uint16_t>(pt[0]) |
                (static_cast<std::uint16_t>(pt[1]) << 8);
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_current.ssh_port = port;
            persist_ssh_profile_locked(std::nullopt, port, std::nullopt, std::nullopt);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_ssh_user_handle) {
            if (pt.size() > 32) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_current.ssh_user = s;
            persist_ssh_profile_locked(std::nullopt, std::nullopt, s, std::nullopt);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_ssh_pw_handle) {
            if (pt.size() > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            persist_ssh_profile_locked(std::nullopt, std::nullopt, std::nullopt, s);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_auth_pw_handle) {
            if (pt.size() > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            persist_auth_password_locked(s);
            xSemaphoreGive(g_mutex);
            return 0;
        }
        if (attr_handle == g_apply_handle) {
            // Any non-empty body triggers a delayed reboot so the central
            // can receive the ATT_WRITE_RSP before the radio goes away.
            if (pt.empty()) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            if (g_restart_timer == nullptr) {
                esp_timer_create_args_t args{};
                args.callback = restart_cb;
                args.name = "ble_apply_restart";
                esp_timer_create(&args, &g_restart_timer);
            }
            ESP_LOGI(kTag, "Apply: scheduling restart in 500 ms");
            esp_timer_start_once(g_restart_timer, 500 * 1000);
            return 0;
        }
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

// --- GATT service table --------------------------------------------------

static ble_gatt_chr_def kChrs[] = {
    {
        .uuid = &kKeyExchangeUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_kx_handle,
    },
    {
        .uuid = &kWifiSsidUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ssid_handle,
    },
    {
        .uuid = &kWifiPskUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_psk_handle,
    },
    {
        .uuid = &kTailscaleAuthKeyUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ts_auth_handle,
    },
    {
        .uuid = &kTailscaleHostnameUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ts_host_handle,
    },
    {
        .uuid = &kSshHostUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ssh_host_handle,
    },
    {
        .uuid = &kSshPortUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ssh_port_handle,
    },
    {
        .uuid = &kSshUserUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ssh_user_handle,
    },
    {
        .uuid = &kSshPasswordUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_ssh_pw_handle,
    },
    {
        .uuid = &kAuthPasswordUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_auth_pw_handle,
    },
    {
        .uuid = &kStatusUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_status_handle,
    },
    {
        .uuid = &kApplyUuid.u,
        .access_cb = gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &g_apply_handle,
    },
    {}  // terminator
};

static ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = kChrs,
    },
    {}  // terminator
};

}  // namespace

const ble_uuid128_t* service_uuid()
{
    return &kSvcUuid;
}

void init()
{
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }

    // Seed current snapshot from existing persistence so encrypted READs
    // surface the device's actual settings after the handshake.
    auto wc = wifi_config::get();
    g_current.wifi_ssid = wc.ssid;
    g_current.tailscale_hostname = nvs_get_str_in(kNvsTailscale, kNvsTailscaleHost);
    if (profiles.count() > 0) {
        if (auto p = profiles.get(0); p) {
            g_current.ssh_host = p->host;
            g_current.ssh_port = p->port;
            g_current.ssh_user = p->user;
        }
    }

    // Install AuthPassword HKDF salt if one was set in a prior session.
    nvs_handle_t nh;
    if (nvs_open(kNvsBleCfg, NVS_READONLY, &nh) == ESP_OK) {
        std::array<std::uint8_t, 32> salt{};
        std::size_t sz = salt.size();
        if (nvs_get_blob(nh, kNvsAuthSalt, salt.data(), &sz) == ESP_OK && sz == salt.size()) {
            g_session.set_hkdf_salt(std::span<const std::uint8_t>{salt});
            ESP_LOGI(kTag, "BLE auth gate ON (HKDF salt installed)");
        } else {
            g_session.set_hkdf_salt({});
        }
        nvs_close(nh);
    } else {
        g_session.set_hkdf_salt({});
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(kSvcs);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gatts_count_cfg: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(kSvcs);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gatts_add_svcs: %d", rc);
    }
}

std::uint16_t status_val_handle()
{
    return g_status_handle;
}

void set_subscribe(std::uint16_t conn_handle, bool subscribed)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_status_conn_handle = conn_handle;
    g_status_subscribed = subscribed;
    xSemaphoreGive(g_mutex);
    ESP_LOGD(kTag, "Status CCCD: conn=%d subscribed=%d",
             conn_handle, subscribed ? 1 : 0);
}

void notify_status(const Status& s)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_status = s;
    const bool subscribed = g_status_subscribed;
    const std::uint16_t conn_h = g_status_conn_handle;
    const std::uint16_t val_h = g_status_handle;
    const std::array<std::uint8_t, 3> bytes{
        static_cast<std::uint8_t>(s.wifi_connected ? 1 : 0),
        static_cast<std::uint8_t>(s.tailscale_up ? 1 : 0),
        static_cast<std::uint8_t>(s.ssh_up ? 1 : 0)};
    xSemaphoreGive(g_mutex);

    if (subscribed && conn_h != BLE_HS_CONN_HANDLE_NONE && val_h != 0) {
        struct os_mbuf* om = ble_hs_mbuf_from_flat(bytes.data(), bytes.size());
        if (om != nullptr) {
            ble_gatts_notify_custom(conn_h, val_h, om);
        }
    }
}

void reset_session()
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_session.reset();
    // On disconnect re-load the password salt so a freshly-set password from
    // the now-closing session takes effect on the next central.
    nvs_handle_t nh;
    if (nvs_open(kNvsBleCfg, NVS_READONLY, &nh) == ESP_OK) {
        std::array<std::uint8_t, 32> salt{};
        std::size_t sz = salt.size();
        if (nvs_get_blob(nh, kNvsAuthSalt, salt.data(), &sz) == ESP_OK && sz == salt.size()) {
            g_session.set_hkdf_salt(std::span<const std::uint8_t>{salt});
        } else {
            g_session.set_hkdf_salt({});
        }
        nvs_close(nh);
    }
    xSemaphoreGive(g_mutex);
}

}  // namespace tab5::ble_config::gatt
