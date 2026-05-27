// VPN bring-up helper.
//
// After Wi-Fi connects, syncs the clock via SNTP and brings up
// whichever VPN backend is compiled in:
//   - Tailscale (preferred when CONFIG_TAILSCALE_ENABLE=y) — registers
//     with the control server, fetches netmap, runs WireGuard in
//     managed mode.
//   - WireGuard (when only CONFIG_WIREGUARD_ENABLE=y) — fixed peer
//     config from NVS/Kconfig.
// SNTP is important on a fresh boot — WireGuard's TAI64N replay
// protection rejects handshakes when the clock is too far off.
#pragma once

#include <cstddef>

namespace tab5::vpn {

enum class Kind { None, WireGuard, Tailscale };

// Kind compiled in (independent of runtime up/down).
Kind kind();

// Synchronise system time via SNTP and bring up the VPN. Blocks up to
// `timeout_s` waiting for the tunnel to come up. Returns true if the
// peer / Tailscale registration reached the "up" state. Safe to call
// when no VPN backend is enabled (returns false).
bool start(int timeout_s);

// Whether the VPN is currently established.
bool is_up();

// Copy the Tailscale-assigned IPv4 (e.g. "100.x.y.z") into `out`.
// Returns false if no Tailscale or not connected.
bool get_tailscale_ip(char* out, size_t out_len);

// If Tailscale is awaiting approval via the admin console, copies the
// pending auth URL into `out` and returns true. Returns false if no
// URL is pending (already connected, no Tailscale, etc.).
bool get_pending_auth_url(char* out, size_t out_len);

}  // namespace tab5::vpn
