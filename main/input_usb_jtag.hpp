#pragma once

#include "byte_input.hpp"
#include "term_core/error.hpp"

namespace tab5 {

// Install the on-board USB-Serial-JTAG driver and spawn a reader task that
// forwards every received byte to `sink`. The same USB connection is also
// used by ESP_LOGx output (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y), so logs
// and input share the host TTY — that is intentional for early bring-up.
//
// The sink is invoked on the reader task's stack; it must be safe to call
// from there and should not block on the same display/terminal that other
// tasks may touch concurrently.
term::Result<void> start_usb_jtag_input(ByteSink sink);

}  // namespace tab5
