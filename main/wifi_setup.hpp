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

// Initialise NVS / netif / event loop / Wi-Fi (esp_wifi_remote against the
// on-board C6) and start a STA connection to (ssid, psk). Blocks up to
// `timeout_s` waiting for the GOT_IP event. Returns on success or error.
//
// Safe to call once at boot. Subsequent reconnect happens automatically
// via the IDF event handler.
term::Result<void> wifi_sta_connect(std::string_view ssid,
                                    std::string_view psk,
                                    int timeout_s);

WifiStatus wifi_status();

}  // namespace tab5
