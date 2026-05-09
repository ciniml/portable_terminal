#pragma once

#include <cstdint>

namespace term {

// Return true if `cp` is an East Asian fullwidth or wide character —
// i.e. a glyph that occupies two terminal cells. Coverage is a pragmatic
// subset of Unicode UAX #11 sufficient for common CJK use; ambiguous /
// neutral classes are treated as narrow.
constexpr bool is_wide_char(char32_t cp) {
    if (cp < 0x1100) return false;
    if (cp <= 0x115F) return true;                          // Hangul Jamo
    if (cp >= 0x2329 && cp <= 0x232A) return true;          // Angle brackets
    if (cp >= 0x2E80 && cp <= 0x303E) return true;          // CJK Radicals
    if (cp >= 0x3041 && cp <= 0x33FF) return true;          // Kana, CJK Symbols
    if (cp >= 0x3400 && cp <= 0x4DBF) return true;          // CJK Ext A
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;          // CJK Unified
    if (cp >= 0xA000 && cp <= 0xA4CF) return true;          // Yi Syllables
    if (cp >= 0xA960 && cp <= 0xA97F) return true;          // Hangul Jamo Ext A
    if (cp >= 0xAC00 && cp <= 0xD7A3) return true;          // Hangul Syllables
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;          // CJK Compat
    if (cp >= 0xFE30 && cp <= 0xFE4F) return true;          // CJK Compat Forms
    if (cp >= 0xFF00 && cp <= 0xFF60) return true;          // Fullwidth Forms
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return true;          // Fullwidth Currency
    if (cp >= 0x1F300 && cp <= 0x1F64F) return true;        // Emoji block 1
    if (cp >= 0x1F680 && cp <= 0x1F6FF) return true;        // Transport/Map
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return true;        // Suppl Symbols & Pict
    if (cp >= 0x20000 && cp <= 0x2FFFD) return true;        // CJK Ext B-F
    if (cp >= 0x30000 && cp <= 0x3FFFD) return true;        // CJK Ext G+
    return false;
}

}  // namespace term
