// Boot-time connection progress + cancel signal.
//
// The boot path (Wi-Fi → VPN → SSH/Telnet/USB-serial auto-connect) ran
// inline in app_main and blocked for up to ~60 s with no feedback. This
// module exposes a tiny shared state so:
//   - the status panel can show which stage is in flight,
//   - a touch on the panel's [Cancel] button can ask the boot task to
//     abort gracefully (it polls cancel_requested() between waits).
//
// The boot task lives in app_main (see do_boot_sequence). Stage updates
// invalidate the status panel through ui_root so the UI repaints
// immediately rather than waiting for the 5 s tick.
#pragma once

#include <cstdint>

namespace tab5 { struct TouchPoint; }

namespace tab5::boot_progress {

enum class Stage : uint8_t {
    Idle = 0,             // boot task hasn't started or no work to do
    WifiConnecting,       // wifi_sta_connect() in flight
    VpnSyncingClock,      // SNTP step of vpn::start()
    VpnConnecting,        // Tailscale / WireGuard handshake
    VpnAwaitAuth,         // Tailscale needs admin-console approval
    RemoteConnecting,     // SSH / Telnet / USB-serial dial
    Done,                 // auto-connect finished, remote up (or not needed)
    Failed,               // last attempted stage failed
    Cancelled,            // user pressed [Cancel] on the status panel
};

struct Snapshot {
    Stage stage;
    char  detail[64];     // human-readable: SSID, "host:port", etc.
    bool  cancel_requested;
};

// Set the current stage + optional short detail string. Thread-safe.
// Triggers a status-panel repaint via ui_root::invalidate().
void set(Stage s, const char* detail = nullptr);

Snapshot get();

// True while the stage is something the user can usefully cancel
// (WifiConnecting / VpnSyncingClock / VpnConnecting / VpnAwaitAuth /
// RemoteConnecting). The status panel shows the [Cancel] button for
// exactly this set.
bool is_busy();

// User-side cancel signal. The boot task is expected to poll
// cancel_requested() at every iteration of its wait loops and bail
// out via set(Stage::Cancelled, ...) when it sees the flag.
void request_cancel();
bool cancel_requested();
void clear_cancel();

// Touch hit-test for the [Cancel] button. Returns true if the tap was
// inside the button rect AND is_busy() — in which case the cancel flag
// is also set. The button rect is fixed (see status panel render).
bool handle_touch(const TouchPoint& p);

// Render the boot-progress block into the status panel at (x, y).
// Returns the y coordinate just below the block. Caller (status_bar)
// holds the UI lock and has already set font / background. When the
// state is Idle (no boot work ever started), returns y unchanged so
// the panel layout doesn't shift.
int render_block(int x, int y, int panel_w);

}  // namespace tab5::boot_progress
