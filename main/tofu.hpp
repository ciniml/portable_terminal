// Trust-on-first-use (TOFU) host-key store.
//
// SSH connections record each remote's host key fingerprint on first
// connect; subsequent connects require the fingerprint to match
// exactly. Entries live in NVS namespace "ssh_tofu" and are keyed by a
// short FNV-1a hash of "host:port" so they fit within NVS's 15-char
// key limit. Each entry stores the host/port string alongside the
// fingerprint so the on-device manager can display human-readable
// rows (legacy 32-byte fingerprint-only blobs are still accepted on
// read for back-compat).
#pragma once

#include <cstdint>
#include <vector>

namespace tab5::tofu {

struct Entry {
    char     key[16];     // NVS key (FNV-1a hex of host:port)
    char     host[64];    // empty if loaded from a legacy 32-byte blob
    uint16_t port;
    uint8_t  fp[32];      // SHA-256 of libssh2_session_hostkey()
};

// Compute the 8-hex-char NVS key for (host, port).
void key_for(const char* host, uint16_t port, char out[16]);

// Check the stored fingerprint for (host, port). Records the current
// fp if the entry is missing. Returns:
//   0  : match (or trusted-on-first-use, *first_use_out=true)
//   -1 : mismatch — caller MUST refuse the connection
//   -2 : NVS unavailable, treat as TOFU disabled
int check_or_record(const char* host, uint16_t port,
                    const uint8_t fp[32], bool* first_use_out);

// Enumerate all entries currently in NVS namespace "ssh_tofu". The
// vector is reset before filling. Returns the count or -1 on error.
int list_entries(std::vector<Entry>& out);

// Erase one entry by its NVS key (the 8-hex-char string stored in
// Entry::key). Returns true on success.
bool remove_by_key(const char* nvs_key);

}  // namespace tab5::tofu
