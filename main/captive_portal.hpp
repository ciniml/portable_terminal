// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

namespace tab5::captive_portal {

// Start a tiny UDP/53 server that resolves every A query to the SoftAP IP
// (192.168.4.1). This is what lets iOS / Android trigger their captive-portal
// detection probes (apple/connecttest etc.) — they look up a fixed host,
// hit our HTTP probe handlers / catch-all, and pop the captive-portal
// sheet. Without DNS hijack the probe times out and iOS refuses to mark
// the network as "connected (no internet)" reliably.
//
// Idempotent. Starts a low-priority task on first call.
void start();
void stop();

}  // namespace tab5::captive_portal
