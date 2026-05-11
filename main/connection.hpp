// Connection abstraction: a uniform surface over remote byte streams
// (SSH, Telnet, future raw-TCP / UART / USB-serial). Each concrete
// implementation owns its protocol task, exposes a non-blocking send()
// queue, and reports remote-pty resizes (where the protocol supports
// it — for plain TCP it's a no-op). The RX path runs the caller's
// ByteSink on the connection's own task; the sink is responsible for
// its own locking.
#pragma once

#include <cstdint>
#include <span>

#include "byte_input.hpp"

namespace tab5 {

class IConnection {
public:
    virtual ~IConnection() = default;

    // Open the transport, perform protocol setup, and spawn the I/O task.
    // Blocks until the session is up or a step fails. Returns false on
    // failure; the object is then left in a clean "not started" state.
    virtual bool start(ByteSink rx_sink) = 0;

    // Stop the I/O task and tear down the transport. Safe to call after
    // a failed start() or in the destructor.
    virtual void stop() = 0;

    // Non-blocking enqueue. Bytes dropped silently if the TX buffer is
    // full or the session has gone away.
    virtual void send(std::span<const uint8_t> bytes) = 0;

    // Notify the remote of a new pty size (SSH WINCH / Telnet NAWS).
    // No-op for protocols without window-size signalling.
    virtual void resize(uint16_t cols, uint16_t rows) = 0;

    virtual bool        is_connected() const = 0;
    virtual const char* host_label()   const = 0;  // "user@host" / "host:port"
    virtual const char* kind()         const = 0;  // "ssh" / "telnet"
};

// Active connection registry (single-session MVP). app_main sets this
// after a successful start(); status panel and other observers read it.
IConnection* active_connection();
void         set_active_connection(IConnection* c);

}  // namespace tab5
