// OTA update client.
//
// Polls a `latest.json` manifest on GitHub Pages (or an override URL from
// NVS), downloads the referenced app image via `esp_https_ota`, and lets
// the bootloader roll us back if the new image doesn't stay healthy for
// 120 s after boot.
//
// Public surface is minimal: start() from app_main once Wi-Fi is up,
// kick(Manual) from a menu / hook, snapshot() for the status panel.
// The design and threat model live in docs/OTA_UPDATE_DESIGN.md.
#pragma once

#include <cstdint>

#include "esp_err.h"

namespace tab5::ota {

enum class Trigger { Auto, Manual };

struct Status {
    enum class Phase {
        Idle,
        Polling,
        Downloading,
        Verifying,
        DeferredReboot,
        Failed,
    };
    Phase   phase          = Phase::Idle;
    int     percent        = -1;               // 0..100 during Downloading
    char    current_version[32] = {};          // esp_app_desc.version at boot
    char    target_version[32]  = {};          // from latest.json
    int64_t last_check_us       = 0;           // esp_timer_get_time when last poll ran
    int64_t backoff_until_us    = 0;           // next allowed retry after Failed
    char    message[80]         = {};          // last log line for UI
};

// Start the OTA supervisor task. Idempotent. Safe to call after Wi-Fi
// (and optionally Tailscale) come up. Reads NVS "ota.auto" to decide
// whether to arm the periodic polling loop.
void start();

// Trigger a check-and-install cycle immediately. Non-blocking.
void kick(Trigger t);

// Read-only status snapshot, safe from any task.
Status snapshot();

// Set the polling base URL at runtime (NVS-backed). Empty resets to the
// Kconfig default. The value is the *directory* URL — the module appends
// "latest.json".
esp_err_t set_base_url(const char* url);

// Callback for deferred-reboot readiness. Default (nullptr) is
// "reboot when active_connection() == nullptr".
using RebootReadyFn = bool (*)(void);
void set_reboot_ready_fn(RebootReadyFn fn);

// Post-boot health check: schedules the 120 s rollback-marker timer.
// Call this once, early in app_main, before OTA polling can start.
// If the running slot is PENDING_VERIFY, arms a timer that marks the
// slot valid after 120 s of continuous healthy operation. If the slot
// was rolled back on the previous boot (ABORTED state), bumps
// NVS "ota.rollback_count" and installs a 24 h auto-poll cool-down.
void arm_post_boot_health_check();

}  // namespace tab5::ota
