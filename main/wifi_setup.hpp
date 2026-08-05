#pragma once

#include <cstdint>
#include <string_view>

#include "term_core/error.hpp"

namespace tab5 {

struct WifiStatus {
    bool connected{false};
    uint32_t ip4{0};   // network byte order
    char ssid[33]{};
};

// Bring up the Wi-Fi hardware without joining any network: PI4IOE2 init +
// C6 co-processor power-cycle, NVS / netif / default event loop, the
// default STA netif and esp_wifi_init (esp_wifi_remote against the
// on-board C6). Does NOT call esp_wifi_start() — the STA path
// (wifi_sta_connect) and the AP-only path (http_config::start on a
// virgin device) each do that themselves. Idempotent; blocks ~2 s for
// the C6 power-cycle on the first call.
term::Result<void> wifi_hw_init();

// wifi_hw_init() + start a STA connection to (ssid, psk). Blocks up to
// `timeout_s` waiting for the GOT_IP event. Returns on success or error.
//
// Safe to call once at boot. Subsequent reconnect happens automatically
// via the IDF event handler.
term::Result<void> wifi_sta_connect(std::string_view ssid,
                                    std::string_view psk,
                                    int timeout_s);

WifiStatus wifi_status();

}  // namespace tab5
