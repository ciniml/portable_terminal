// SSH transport via libssh2 + mbedTLS over lwIP BSD sockets.
//
// One internal `ssh_task` owns the libssh2 session; send() queues TX bytes
// into a FreeRTOS stream buffer and the caller-supplied rx_sink runs on
// ssh_task for incoming bytes. MVP: password auth, single session, TOFU
// host-key verification in NVS namespace "ssh_tofu".
//
// The libssh2 session state is module-static; constructing more than one
// SshConnection at a time is not supported yet.
#pragma once

#include <cstdint>

#include "connection.hpp"

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

class SshConnection final : public IConnection {
public:
    explicit SshConnection(const SshConfig& cfg);
    ~SshConnection() override;

    bool start(ByteSink rx_sink) override;
    void stop() override;
    void send(std::span<const uint8_t> bytes) override;
    void resize(uint16_t cols, uint16_t rows) override;

    bool        is_connected() const override;
    const char* host_label()   const override { return label_; }
    const char* kind()         const override { return "ssh"; }

private:
    SshConfig cfg_;
    char      label_[64]{};
};

}  // namespace tab5
