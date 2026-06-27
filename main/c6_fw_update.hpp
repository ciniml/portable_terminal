#pragma once

// Boot-time auto-update of the on-board ESP32-C6 slave firmware.
//
// Once the user has done the initial factory 1.4.x → 2.x transition
// (which still requires c6_updater/, since esp_hosted 2.x can't speak
// to the 1.4.x slave), subsequent 2.x → 2.x updates ride along with
// the main app: each release embeds network_adapter.bin into the
// dedicated `c6_fw` data partition, and on boot we compare the
// embedded version against what the C6 reports — if it's strictly
// newer, we stream the blob over the existing esp_hosted RPC link.
//
// This header is target-only; the host gtest build never sees it.

#include <cstdint>

namespace tab5::c6_fw {

struct EmbeddedInfo {
    // Parsed from esp_app_desc_t::version using "%hhu.%hhu.%hhu".
    // Whole-tuple comparison is the upgrade gate.
    uint8_t major{0};
    uint8_t minor{0};
    uint8_t patch{0};
    char    version_str[33]{};
    char    project_name[33]{};
};

// Read esp_app_desc from the c6_fw partition. Returns false if the
// partition is absent, the blob looks blank (all-0xFF), or the magic
// word is wrong. On success the out struct is fully populated.
bool read_embedded_info(EmbeddedInfo* out);

// Caller has already let esp_hosted_init (the constructor-driven init
// in port_esp_hosted_host_init.c) and the slave-side handshake run —
// i.e. the transport is up. We rely on esp_hosted_get_coprocessor_fwversion
// to surface "transport not up" as ESP_FAIL, which we treat as "skip".
//
// Returns:
//    0 — already up to date / downgrade / nothing to do (skip)
//    1 — update applied; caller should reboot the P4 to renegotiate
//   -1 — update was attempted but failed
int update_if_needed();

}  // namespace tab5::c6_fw
