// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

// NimBLE host bring-up + GAP event handling for the Tab5 BLE provisioning
// service. The actual GATT server lives in gatt_service.cpp.

#include "ble_config/ble_config.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_store.h>
#include <host/ble_uuid.h>
#include <services/gap/ble_svc_gap.h>

#include "gatt_service.hpp"

// ESP-IDF's NimBLE port doesn't auto-initialise the bond store backend; it
// must be called explicitly (see the bleprph example). Without it every
// store access returns ENOTSUP and pairing aborts. No public header declares
// it.
extern "C" void ble_store_config_init(void);

namespace tab5::ble_config {

namespace {

constexpr const char* kTag = "ble-cfg";

static uint8_t g_own_addr_type = 0;
static char g_device_name[32] = CONFIG_TAB5_BLE_DEVICE_NAME;
static std::atomic<bool> g_ble_connected{false};

static int gap_event_cb(struct ble_gap_event* event, void* arg);

static void start_advertising()
{
    struct ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = gatt::service_uuid();
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gap_adv_set_fields: %d", rc);
        return;
    }

    // Scan response: complete device name (Web Bluetooth uses active scan).
    struct ble_hs_adv_fields rsp{};
    rsp.name = reinterpret_cast<const uint8_t*>(g_device_name);
    rsp.name_len = static_cast<uint8_t>(std::strlen(g_device_name));
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(kTag, "ble_gap_adv_rsp_set_fields: %d", rc);
    }

    struct ble_gap_adv_params adv_params{};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(kTag, "ble_gap_adv_start: %d", rc);
    } else {
        ESP_LOGI(kTag, "advertising as \"%s\"", g_device_name);
    }
}

static int gap_event_cb(struct ble_gap_event* event, void* /*arg*/)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(kTag, "connected: handle=%d", event->connect.conn_handle);
            g_ble_connected.store(true, std::memory_order_relaxed);
        } else if (event->connect.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGW(kTag, "connect failed: status=%d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_LINK_ESTAB:
        if (event->link_estab.status == 0) {
            ESP_LOGI(kTag, "link established: handle=%d",
                     event->link_estab.conn_handle);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(kTag, "disconnected: reason=%d", event->disconnect.reason);
        g_ble_connected.store(false, std::memory_order_relaxed);
        gatt::set_subscribe(BLE_HS_CONN_HANDLE_NONE, false);
        gatt::reset_session();
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == gatt::status_val_handle()) {
            gatt::set_subscribe(event->subscribe.conn_handle,
                                event->subscribe.cur_notify != 0);
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(kTag, "MTU: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

static void on_sync()
{
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_hs_id_infer_auto: %d", rc);
        return;
    }
    ble_svc_gap_device_name_set(g_device_name);
    ESP_LOGI(kTag, "BLE host ready, name=%s addr_type=%d",
             g_device_name, g_own_addr_type);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(kTag, "NimBLE host reset: reason=%d", reason);
}

static void nimble_host_task(void* /*param*/)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

std::expected<void, Error> start()
{
    ESP_LOGI(kTag, "starting BLE provisioning service");

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init: %s (likely C6 BT controller missing)",
                 esp_err_to_name(err));
        return std::unexpected{Error::NimbleInit};
    }

    // Application-layer X25519 + AES-256-GCM provides confidentiality; BLE
    // SM/bonding is intentionally not used (no _ENC chr flags).
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    gatt::init();

    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);
    return {};
}

void notify_status(const Status& s)
{
    gatt::notify_status(s);
}

bool is_connected()
{
    return g_ble_connected.load(std::memory_order_relaxed);
}

}  // namespace tab5::ble_config
