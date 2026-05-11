// On-device connection-profile store.
//
// Profiles are persisted in NVS namespace "conn" so they can be added,
// edited, switched, and deleted on the device itself — Kconfig is only
// consulted on the very first boot to seed profile 0 from any existing
// CONFIG_TAB5_SSH_* / CONFIG_TAB5_TELNET_* values, after which NVS is
// authoritative.
//
// Up to CONFIG_TAB5_MAX_PROFILES slots are available. The currently
// selected profile drives the auto-connect path in app_main.
#pragma once

#include <cstdint>
#include <optional>

namespace tab5 {

enum class ConnProto : uint8_t {
    SSH    = 0,
    Telnet = 1,
};

enum class SshAuth : uint8_t {
    Password = 0,
    Pubkey   = 1,
};

struct Profile {
    char     name[32];      // display name
    ConnProto proto;
    uint16_t port;
    char     host[64];
    char     user[32];      // SSH only
    SshAuth  auth;          // SSH only
    char     password[64];  // SSH password; ignored if auth=Pubkey
};

class Profiles {
public:
    // Load from NVS, seeding profile 0 from Kconfig on a virgin device.
    void init();

    int max()      const;   // CONFIG_TAB5_MAX_PROFILES
    int count()    const { return count_; }
    int selected() const { return selected_; }

    // 0-based index access. Returns nullopt if out of range.
    std::optional<Profile> get(int idx) const;

    // Persist a new profile / overwrite at idx. add() returns the new
    // index, or -1 if the store is full.
    int  add(const Profile& p);
    bool update(int idx, const Profile& p);
    bool remove(int idx);

    bool select(int idx);

private:
    int count_    = 0;
    int selected_ = 0;
};

extern Profiles profiles;

}  // namespace tab5
