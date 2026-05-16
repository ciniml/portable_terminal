// USB HID keyboard input source.
//
// When a USB keyboard is plugged into the Tab5 USB-A port, the HID host
// task reads boot-protocol reports, converts the pressed keys to bytes
// (US layout, Shift / Ctrl / Alt aware) and forwards them to the
// caller-supplied ByteSink — same surface as start_usb_jtag_input() /
// start_uart_input(), so the bytes flow into the same SSH / Telnet /
// local terminal path.
//
// Only one keyboard interface is tracked at a time. Boot protocol is
// requested explicitly so we don't have to parse a HID report descriptor.
#pragma once

#include "byte_input.hpp"
#include "term_core/error.hpp"

namespace tab5 {

// Install the USB host library + HID class driver and start a worker
// task that emits bytes to `sink`. Safe to call once at boot. Returns
// non-success if USB host init failed.
term::Result<void> start_usb_hid_input(ByteSink sink);

}  // namespace tab5
