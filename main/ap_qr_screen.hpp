// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Full-screen QR onboarding overlay for the HTTP settings softAP.
//
// Shown when the STA connect fails (the "needs provisioning" case):
// two QR codes — one that joins the phone to the Tab5-XXXXXX AP
// (standard WIFI: payload, scanned by the stock iOS / Android camera)
// and one with the portal URL — plus the credentials in plain text.
// When Tailscale registration is awaiting interactive login (auth URL
// pending at show() time) a third QR with that login URL is added so
// the device can be registered by scanning it with a phone.
//
// Follows the menu.cpp convention: rendering is passive. app_main
// registers a ui_root layer that calls render() while visible() and
// flips ui_root overlay state around show()/dismiss(); this module
// never touches M5GFX or ui_root on its own. All calls must be made
// under the global UI lock.
#pragma once

#include "sdkconfig.h"

namespace tab5::ap_qr_screen {

#if CONFIG_TAB5_HTTP_CONFIG_ENABLED

// Mark the overlay visible. Snapshot of SSID / password is taken from
// http_config::ap_ssid()/ap_psk() at render time, so call only after
// http_config::start() succeeded. The Tailscale auth URL (if any) is
// snapshotted here, at show() time. Caller must hold the UI lock and
// invalidate the screen afterwards.
void show();

// Hide the overlay. Caller must hold the UI lock and invalidate the
// screen afterwards so the terminal repaints.
void dismiss();

bool visible();

// Paint the full-screen overlay. Caller holds the UI lock.
void render();

#else

inline void show() {}
inline void dismiss() {}
inline bool visible() { return false; }
inline void render() {}

#endif  // CONFIG_TAB5_HTTP_CONFIG_ENABLED

}  // namespace tab5::ap_qr_screen
