// Minimal SSH client: libssh2 + mbedTLS over lwIP BSD sockets.
//
// One `ssh_task` owns the libssh2 session; the public API queues TX bytes
// into a FreeRTOS stream buffer and invokes a caller-supplied RX callback
// for bytes coming back from the remote pty. The RX callback runs on
// `ssh_task` — the caller is responsible for any locking it needs around
// shared terminal / display state.
//
// MVP: password auth only, single session, TOFU host-key verification
// against NVS namespace "ssh_tofu".
#pragma once

#include <cstdint>
#include <functional>
#include <span>

namespace tab5 {

struct SshConfig {
    const char* host;
    uint16_t port;
    const char* user;
    const char* password;
    uint16_t cols;
    uint16_t rows;
    uint32_t task_stack_bytes;
};

using SshRxApply = std::function<void(std::span<const uint8_t>)>;

class SshClient {
public:
    // Connect, authenticate, open an interactive shell channel, and spawn
    // ssh_task. Blocks until the shell is up or a step fails.
    bool start(const SshConfig& cfg, SshRxApply rx_apply);

    // Non-blocking enqueue. Bytes are dropped silently if the stream
    // buffer is full or the session has gone away.
    void send(std::span<const uint8_t> bytes);

    // Request a remote pty resize (WINCH). Latches the requested size;
    // ssh_task picks it up and calls libssh2_channel_request_pty_size on
    // its next poll. No-op if not connected.
    void resize_pty(uint16_t cols, uint16_t rows);

    bool is_connected() const;
};

extern SshClient ssh_client;

}  // namespace tab5
