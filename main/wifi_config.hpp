// NVS-backed Wi-Fi credentials.
//
// Single entry for now (most users connect to one AP). Seeded from
// CONFIG_TAB5_WIFI_SSID / CONFIG_TAB5_WIFI_PSK / CONFIG_TAB5_WIFI_CONNECT_TIMEOUT_S
// on the first boot of a virgin device; afterwards NVS is the source
// of truth and the Kconfig values are ignored at runtime.
#pragma once

namespace tab5::wifi_config {

struct Config {
    char ssid[33];      // 32 + NUL
    char psk[64];       // 63 + NUL (max WPA passphrase length)
    int  timeout_s;
};

// Load from NVS, seeding from Kconfig on the first boot.
void init();

// Return the current config (zero-initialised ssid if empty).
Config get();

// Persist a new config. Returns false on NVS error.
bool set(const Config& c);

// True if ssid is non-empty (i.e. there's something to connect to).
bool has_credentials();

}  // namespace tab5::wifi_config
