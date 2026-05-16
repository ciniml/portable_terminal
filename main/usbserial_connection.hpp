// USB-serial transport over an FTDI device plugged into the Tab5
// USB-A port. Wraps components/usb_host_ftdi_sio behind the
// IConnection interface so app_main can pick it from a profile the
// same way it picks SSH or Telnet.
//
// Single-device MVP — only the first FTDI on the bus is handled.
#pragma once

#include <cstdint>

#include "connection.hpp"

namespace tab5 {

struct UsbSerialConfig {
    uint32_t baud;            // e.g. 115200
    uint8_t  data_bits;       // 7 or 8
    uint8_t  stop_bits;       // 0=1, 1=1.5, 2=2 (per FTDI_STOP_BITS_*)
    uint8_t  parity;          // 0..4 (per FTDI_PARITY_*)
};

class UsbSerialConnection final : public IConnection {
public:
    explicit UsbSerialConnection(const UsbSerialConfig& cfg);
    ~UsbSerialConnection() override;

    bool start(ByteSink rx_sink) override;
    void stop() override;
    void send(std::span<const uint8_t> bytes) override;
    void resize(uint16_t /*cols*/, uint16_t /*rows*/) override {}

    bool        is_connected() const override;
    const char* host_label()   const override { return label_; }
    const char* kind()         const override { return "usb-serial"; }

private:
    UsbSerialConfig cfg_;
    char            label_[64]{};
};

}  // namespace tab5
