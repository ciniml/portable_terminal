// Tab5 clip-on QWERTY keyboard (ExtPort1, I2C, addr 0x6D) as a byte-input
// source. The keyboard is configured into HID mode so its events carry
// USB-HID Keyboard Page (0x07) modifier + keycode pairs; we translate
// those through the shared main/hid_translate so it produces the same
// byte stream as a USB-A HID dongle for the same physical key.
#pragma once

#include "byte_input.hpp"
#include "sdkconfig.h"
#include "term_core/error.hpp"

namespace tab5 {

struct Tab5KbdConfig {
#ifdef CONFIG_TAB5_KEYBOARD_I2C_SDA
    int      sda_gpio   = CONFIG_TAB5_KEYBOARD_I2C_SDA;
#else
    int      sda_gpio   = 0;
#endif
#ifdef CONFIG_TAB5_KEYBOARD_I2C_SCL
    int      scl_gpio   = CONFIG_TAB5_KEYBOARD_I2C_SCL;
#else
    int      scl_gpio   = 1;
#endif
#ifdef CONFIG_TAB5_KEYBOARD_INT_GPIO
    int      int_gpio   = CONFIG_TAB5_KEYBOARD_INT_GPIO;
#else
    int      int_gpio   = 50;
#endif
    uint32_t i2c_hz     = 400000;
    int      task_stack = 4096;
};

// Bring up the Tab5 Keyboard (re-binds the Arduino-compat Wire object to
// the keyboard's pins), configure HID mode, install the INT-driven event
// loop, and spawn a FreeRTOS task that drains events into `sink`.
//
// Returns NotInitialized if the I2C probe / handshake fails (no keyboard
// plugged in, address mismatch, …). The rest of the firmware keeps
// running — input simply has one fewer source.
term::Result<void> start_tab5_kbd_input(ByteSink sink,
                                        const Tab5KbdConfig& cfg = {});

}  // namespace tab5
