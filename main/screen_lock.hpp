// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Idle screen-lock with PIN unlock.
//
// After a configurable idle period without any *user* input (touch,
// soft keyboard, physical keyboards, UART / USB-JTAG bytes — remote RX
// does NOT count), the backlight is switched off and the device enters
// the locked state. The first input wakes the display onto a fullscreen
// PIN pad (never the terminal content); the correct PIN returns to the
// terminal with a full repaint.
//
// Configuration lives in NVS namespace "scrlock":
//   "pin_hash"    32-byte blob — SHA-256 of the ASCII PIN digits
//   "enabled"     u8   — idle auto-lock on/off
//   "timeout_min" u16  — idle timeout in minutes
// No PIN stored + enabled=false is the default (feature off). This is
// casual privacy protection, not cryptographic security — anyone with
// physical access can clear the PIN through the HTTP settings portal.
//
// Rendering follows the ap_qr_screen convention: passive. app_main
// registers a ui_root layer that calls render() while locked() and
// routes input into handle_touch()/feed_key(); this module never calls
// ui_root itself — overlay-flag / invalidate transitions run through
// the two UiHook callbacks app_main installs. All runtime entry points
// below marked "UI lock" must be called with the global UI lock held.
#pragma once

#include <cstdint>
#include <functional>

#include "input_touch.hpp"

namespace tab5::screen_lock {

// ---- Configuration (NVS-backed, thread-safe) ---------------------------

struct Config {
    bool     enabled;      // idle auto-lock armed
    uint16_t timeout_min;  // idle timeout in minutes (>= 1)
};

Config get_config();

// Persist enabled + timeout. timeout_min is clamped to 1..1440.
bool set_config(bool enabled, uint16_t timeout_min);

// Hash + persist a new PIN (4-8 ASCII digits). nullptr / "" clears the
// stored PIN (and thereby disarms locking). Returns false on invalid
// PIN or NVS failure.
bool set_pin(const char* pin_digits);

bool pin_is_set();

// Constant-shape check of `pin_digits` against the stored hash.
bool verify_pin(const char* pin_digits);

// ---- Runtime -----------------------------------------------------------

// Load config from NVS and start the idle clock. Call once at boot,
// after nvs_flash_init().
void init();

// Note user activity — call from every input chokepoint. Cheap
// (relaxed atomic timestamp store), safe from any task, no UI lock.
void activity_note();

// Callbacks app_main installs so lock/unlock transitions can close
// overlays and drive ui_root. Both are invoked with the UI lock held
// (lock_now()/tick() callers hold it).
using UiHook = std::function<void()>;
void set_ui_hooks(UiHook on_lock, UiHook on_unlock);

// Lock immediately (menu [Lock] button). No-op unless a PIN is set —
// locking without a PIN would be unrecoverable from the device itself.
// UI lock.
void lock_now();

// True while the lock screen owns the display; input paths consult
// this and route into handle_touch()/feed_key() instead of the
// terminal / menu / keyboard. Lock-free.
bool locked();

// Idle check — call periodically (app_main drives it from a 30 s
// esp_timer). Locks via lock_now() when the timeout elapsed. Never
// locks while the AP-QR provisioning overlay is up. UI lock.
void tick();

// If locked and the backlight is off: restore brightness and return
// true (the waking input event is consumed — the caller must not
// forward it to handle_touch/feed_key). UI lock.
bool wake_if_dark();

// Touch input while locked (numeric pad). Returns true when consumed
// (always, while locked). UI lock.
bool handle_touch(const TouchPoint& p);

// Physical-keyboard byte while locked: '0'..'9' enter digits, BS/DEL
// erases, CR/LF submits; everything else is swallowed. Returns true
// when consumed (always, while locked). UI lock.
bool feed_key(uint8_t byte);

// 500 ms cadence hook (app_main's blink timer) — refreshes the
// wrong-PIN lockout countdown while it is on screen. UI lock.
void blink_tick();

// Paint the fullscreen lock screen. Called by the ui_root layer while
// locked(). UI lock.
void render();

}  // namespace tab5::screen_lock
