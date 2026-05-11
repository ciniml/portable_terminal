// Telnet (RFC 854) transport over a plain TCP socket.
//
// Single internal `telnet_task` polls the socket, strips IAC option
// negotiations from the RX stream (refusing every DO/WILL with
// DONT/WONT — except IAC DO NAWS, which we accept and respond to with
// our current cols/rows), and forwards everything else to the
// caller-supplied rx_sink. TX bytes are queued through a FreeRTOS
// stream buffer; literal 0xFF in the TX stream is escaped as 0xFF 0xFF.
//
// State is module-static — like SshConnection, multi-instance isn't
// supported yet.
#pragma once

#include <cstdint>

#include "connection.hpp"

namespace tab5 {

struct TelnetConfig {
    const char* host;
    uint16_t    port;
    uint16_t    cols;
    uint16_t    rows;
    uint32_t    task_stack_bytes;
};

class TelnetConnection final : public IConnection {
public:
    explicit TelnetConnection(const TelnetConfig& cfg);
    ~TelnetConnection() override;

    bool start(ByteSink rx_sink) override;
    void stop() override;
    void send(std::span<const uint8_t> bytes) override;
    void resize(uint16_t cols, uint16_t rows) override;

    bool        is_connected() const override;
    const char* host_label()   const override { return label_; }
    const char* kind()         const override { return "telnet"; }

private:
    TelnetConfig cfg_;
    char         label_[64]{};
};

}  // namespace tab5
