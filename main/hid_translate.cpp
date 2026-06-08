// Lifted verbatim from main/input_usb_hid.cpp (the kMap[]/special_seq()/
// emit_key() block) so the Tab5 Keyboard driver and the USB HID host
// driver can share one source of truth for scancode mapping.

#include "hid_translate.hpp"

#include <cstring>

#include "usb/hid_usage_keyboard.h"  // HID_KEY_* constants

namespace tab5::hid_translate {

namespace {

// HID modifier bit masks (report byte 0)
constexpr uint8_t kModLCtrl  = 0x01;
constexpr uint8_t kModLShift = 0x02;
constexpr uint8_t kModLAlt   = 0x04;
constexpr uint8_t kModRCtrl  = 0x10;
constexpr uint8_t kModRShift = 0x20;
constexpr uint8_t kModRAlt   = 0x40;

// US-layout maps. Each entry is (unshifted, shifted) for HID usage codes
// 0x04..0x38 (the printable range). Special keys handled out of band.
struct CharMap {
    char base;
    char shifted;
};
constexpr CharMap kMap[0x39] = {
    {0, 0},  {0, 0},  {0, 0},  {0, 0},                       // 0x00..0x03
    {'a','A'}, {'b','B'}, {'c','C'}, {'d','D'}, {'e','E'},   // 0x04..0x08
    {'f','F'}, {'g','G'}, {'h','H'}, {'i','I'}, {'j','J'},   // 0x09..0x0D
    {'k','K'}, {'l','L'}, {'m','M'}, {'n','N'}, {'o','O'},   // 0x0E..0x12
    {'p','P'}, {'q','Q'}, {'r','R'}, {'s','S'}, {'t','T'},   // 0x13..0x17
    {'u','U'}, {'v','V'}, {'w','W'}, {'x','X'}, {'y','Y'},   // 0x18..0x1C
    {'z','Z'},                                               // 0x1D
    {'1','!'}, {'2','@'}, {'3','#'}, {'4','$'}, {'5','%'},   // 0x1E..0x22
    {'6','^'}, {'7','&'}, {'8','*'}, {'9','('}, {'0',')'},   // 0x23..0x27
    {0, 0},   // Enter — handled specially      0x28
    {0, 0},   // Esc                            0x29
    {0, 0},   // Backspace                      0x2A
    {0, 0},   // Tab                            0x2B
    {' ',' '}, // Space                         0x2C
    {'-','_'}, {'=','+'},                                    // 0x2D, 0x2E
    {'[','{'}, {']','}'}, {'\\','|'},                        // 0x2F..0x31
    {0, 0},                                                  // 0x32 non-US hash
    {';',':'}, {'\'','"'}, {'`','~'},                        // 0x33..0x35
    {',','<'}, {'.','>'}, {'/','?'},                         // 0x36..0x38
};

// Per-key escape sequences (for special keys above 0x39). Returns a
// non-null pointer when this key produces a fixed escape sequence; the
// pointer is to a static string with length in *len_out.
const char* special_seq(uint8_t code, int* len_out) {
    *len_out = 0;
    auto lit = [&](const char* s, int n) { *len_out = n; return s; };
    switch (code) {
        case HID_KEY_RIGHT:       return lit("\x1B[C", 3);
        case HID_KEY_LEFT:        return lit("\x1B[D", 3);
        case HID_KEY_DOWN:        return lit("\x1B[B", 3);
        case HID_KEY_UP:          return lit("\x1B[A", 3);
        case HID_KEY_HOME:        return lit("\x1B[H", 3);
        case HID_KEY_END:         return lit("\x1B[F", 3);
        case HID_KEY_INSERT:      return lit("\x1B[2~", 4);
        case HID_KEY_DELETE:      return lit("\x1B[3~", 4);
        case HID_KEY_PAGEUP:      return lit("\x1B[5~", 4);
        case HID_KEY_PAGEDOWN:    return lit("\x1B[6~", 4);
        case HID_KEY_F1:          return lit("\x1BOP", 3);
        case HID_KEY_F2:          return lit("\x1BOQ", 3);
        case HID_KEY_F3:          return lit("\x1BOR", 3);
        case HID_KEY_F4:          return lit("\x1BOS", 3);
        case HID_KEY_F5:          return lit("\x1B[15~", 5);
        case HID_KEY_F6:          return lit("\x1B[17~", 5);
        case HID_KEY_F7:          return lit("\x1B[18~", 5);
        case HID_KEY_F8:          return lit("\x1B[19~", 5);
        case HID_KEY_F9:          return lit("\x1B[20~", 5);
        case HID_KEY_F10:         return lit("\x1B[21~", 5);
        case HID_KEY_F11:         return lit("\x1B[23~", 5);
        case HID_KEY_F12:         return lit("\x1B[24~", 5);
        default: return nullptr;
    }
}

}  // namespace

size_t emit_key(uint8_t code, Mods mods, uint8_t out[kMaxBytes]) {
    const bool shift = mods & (kModLShift | kModRShift);
    const bool ctrl  = mods & (kModLCtrl  | kModRCtrl);
    const bool alt   = mods & (kModLAlt   | kModRAlt);

    // Helper to write `n` bytes from `src` into `out`, optionally with an
    // ESC prefix when Alt is held. Returns the total bytes written.
    auto write_bytes = [&](const uint8_t* src, size_t n) -> size_t {
        size_t off = 0;
        if (alt && off + 1 <= kMaxBytes) out[off++] = 0x1B;
        for (size_t i = 0; i < n && off < kMaxBytes; ++i) out[off++] = src[i];
        return off;
    };

    // Fixed-byte specials.
    switch (code) {
        case HID_KEY_ENTER: { uint8_t b = '\r'; return write_bytes(&b, 1); }
        case HID_KEY_ESC:   { uint8_t b = 0x1B; return write_bytes(&b, 1); }
        case HID_KEY_DEL:   { uint8_t b = 0x7F; return write_bytes(&b, 1); }
        case HID_KEY_TAB:   { uint8_t b = '\t'; return write_bytes(&b, 1); }
        default: break;
    }

    // Multi-byte specials (arrows / F-keys / Home / End / Ins / Del / PgUp/Dn).
    {
        int len = 0;
        if (const char* s = special_seq(code, &len)) {
            return write_bytes(reinterpret_cast<const uint8_t*>(s),
                               static_cast<size_t>(len));
        }
    }

    // Printable range.
    if (code >= 0x04 && code < 0x39) {
        char ch = shift ? kMap[code].shifted : kMap[code].base;
        if (ch == 0) return 0;
        if (ctrl) {
            // Standard Ctrl- mapping.
            if (ch >= 'a' && ch <= 'z')      ch = static_cast<char>(ch - 'a' + 1);
            else if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 1);
            else if (ch >= '@' && ch <= '_') ch = static_cast<char>(ch - '@');
            // else: pass through
        }
        uint8_t b = static_cast<uint8_t>(ch);
        return write_bytes(&b, 1);
    }

    return 0;
}

}  // namespace tab5::hid_translate
