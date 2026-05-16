// USB host library bring-up.
//
// Several class drivers (HID for the on-device keyboard, FTDI for the
// USB-serial Connection backend) need usb_host_install() to have run
// and lib events to be pumped. Each driver shouldn't install the host
// library itself — call start_usb_host_root() once at boot, then bring
// up the class drivers.
#pragma once

#include "term_core/error.hpp"

namespace tab5 {

// Idempotent: subsequent calls are no-ops.
term::Result<void> start_usb_host_root();

}  // namespace tab5
