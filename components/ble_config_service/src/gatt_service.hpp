// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

// Private interface between ble_config.cpp (NimBLE bring-up / GAP events)
// and gatt_service.cpp (the GATT server itself).

#pragma once

#include <cstdint>

#include <host/ble_uuid.h>

#include "ble_config/ble_config.hpp"

namespace tab5::ble_config::gatt {

// Register the service with NimBLE. Must run after nimble_port_init() but
// before nimble_port_freertos_init() so the host task picks up the table.
void init();

// 128-bit service UUID — used by ble_config.cpp to populate the adv frame.
const ble_uuid128_t* service_uuid();

// Status characteristic value handle (looked up by the GAP subscribe event
// to detect CCCD writes targeting this characteristic).
std::uint16_t status_val_handle();

// Latch the per-connection subscription state. Called from gap_event_cb on
// BLE_GAP_EVENT_SUBSCRIBE and BLE_GAP_EVENT_DISCONNECT (with subscribed=
// false + handle=BLE_HS_CONN_HANDLE_NONE).
void set_subscribe(std::uint16_t conn_handle, bool subscribed);

// Apply UI status updates. Triggers a NOTIFY if a central is subscribed.
void notify_status(const Status& s);

// Drop the per-connection crypto session (called from gap on DISCONNECT)
// and re-load the password salt so a freshly-set password takes effect on
// the next central.
void reset_session();

}  // namespace tab5::ble_config::gatt
