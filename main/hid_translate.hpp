// USB HID Keyboard scancode → terminal-byte translator.
//
// Shared by the USB HID host driver (main/input_usb_hid.cpp) and the Tab5
// clip-on keyboard (main/input_tab5_kbd.cpp) so both physical keyboards
// produce the same byte stream for the same key. Pure logic, no I/O.
//
// Input  : one HID Usage Code (Keyboard Page 0x07) + the 8-bit modifier
//          byte from the HID report (or the same byte synthesised by the
//          Tab5 Keyboard firmware in HID mode).
// Output : 0..N bytes appended to a small caller-owned buffer:
//          - printable keys → 1 ASCII byte (US layout, Shift-aware)
//          - Ctrl + letter  → 0x01..0x1A
//          - Alt + key      → 0x1B (ESC) prefix + the unmodified byte
//          - Enter / Esc / Backspace / Tab → \r / 0x1B / 0x7F / \t
//          - Arrows / F-keys / Home/End/PgUp/PgDn / Ins/Del → VT escapes
//          - anything else → 0 (caller should swallow)
#pragma once

#include <cstddef>
#include <cstdint>

namespace tab5::hid_translate {

// 8 modifier bits, USB HID Keyboard Page format:
//   0x01 LCtrl  0x02 LShift  0x04 LAlt  0x08 LGUI
//   0x10 RCtrl  0x20 RShift  0x40 RAlt  0x80 RGUI
using Mods = uint8_t;

// Maximum number of bytes one key can produce (an Alt-prefixed 5-byte F-key
// escape is the worst case; round up).
constexpr size_t kMaxBytes = 8;

// Translate one HID Usage Code + modifier into bytes. Writes ≤ kMaxBytes
// into `out`, returns the number written (0 = swallow). `out` is uninitialised
// on entry; callers should ignore the buffer when the return is 0.
size_t emit_key(uint8_t hid_code, Mods mods, uint8_t out[kMaxBytes]);

}  // namespace tab5::hid_translate
