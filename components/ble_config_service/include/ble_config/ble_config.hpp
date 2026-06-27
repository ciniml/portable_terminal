// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <expected>

namespace tab5::ble_config {

// Snapshot of subsystems whose state is surfaced on the BLE Status chr.
struct Status {
    bool wifi_connected;
    bool tailscale_up;
    bool ssh_up;
};

enum class Error {
    NvsInit,
    NimbleInit,
    GapAdvStart,
    GattRegister,
    // Application-layer crypto session (X25519 + AES-256-GCM).
    CryptoNotReady, // operation attempted before key exchange completed
    CryptoBadKey,   // peer pubkey rejected / ECDH failed
    CryptoAuth,     // AES-GCM authentication tag mismatch
    CryptoRng,      // ctr_drbg seeding / random failure
};

// Start NimBLE host + GATT server + advertising. Non-fatal — on failure
// (e.g. C6 firmware lacks BT controller) the function logs and returns
// the Error so the caller can continue without provisioning.
std::expected<void, Error> start();

// Apply UI status updates (called from Wi-Fi / Tailscale / SSH subsystems).
// Triggers a NOTIFY on the Status characteristic if a central is subscribed.
void notify_status(const Status& s);

// True while a BLE central is connected (for any on-LCD status bar use).
bool is_connected();

}  // namespace tab5::ble_config
