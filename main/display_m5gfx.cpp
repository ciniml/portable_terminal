#include "display_m5gfx.hpp"

#include <algorithm>
#include <cstddef>

#include "M5Unified.h"

namespace tab5 {

namespace {

// Standard ANSI 16-color palette (RGB 8-8-8).
constexpr uint32_t kAnsiPalette[16] = {
    0x000000, 0xC00000, 0x00C000, 0xC0C000,
    0x0000C0, 0xC000C0, 0x00C0C0, 0xC0C0C0,
    0x404040, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

uint16_t to_565(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint16_t color_to_565(term::Color c, uint32_t def_rgb) {
    using Kind = term::Color::Kind;
    switch (c.kind) {
        case Kind::Default: return to_565(def_rgb);
        case Kind::Index: {
            uint8_t i = uint8_t(c.value & 0xFF);
            if (i < 16) return to_565(kAnsiPalette[i]);
            if (i >= 232) {
                uint8_t v = (i - 232) * 10 + 8;
                return to_565((uint32_t(v) << 16) | (uint32_t(v) << 8) | v);
            }
            uint8_t n = i - 16;
            uint8_t r = (n / 36) * 51;
            uint8_t g = ((n / 6) % 6) * 51;
            uint8_t b = (n % 6) * 51;
            return to_565((uint32_t(r) << 16) | (uint32_t(g) << 8) | b);
        }
        case Kind::Rgb: return to_565(c.value);
    }
    return to_565(def_rgb);
}

// Encode `cp` as UTF-8 into `out` (max 4 bytes). Returns byte length.
size_t encode_utf8(char32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

// lgfxJapanGothic_24 (IPA Gothic, monospace) advances 12 px per half-width
// glyph and 24 px per fullwidth. Unlike efontJA_*, this font includes the
// halfwidth-katakana range U+FF61..U+FF9F. No bold variant ships with it,
// so bold is emulated via a 1 px overstrike (see draw_cells).
constexpr uint16_t kCellW = 12;
constexpr uint16_t kCellH = 24;

}  // namespace

M5GfxDisplay::M5GfxDisplay(uint16_t cols, uint16_t rows)
    : cols_(cols), rows_(rows) {}

term::Result<void> M5GfxDisplay::init() {
    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Display.setRotation(1);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setFont(&fonts::lgfxJapanGothic_24);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_left);

    cell_w_ = kCellW;
    cell_h_ = kCellH;

    uint16_t needed_w = uint32_t(cell_w_) * cols_;
    uint16_t screen_w = M5.Display.width();
    origin_x_ = (screen_w > needed_w) ? (screen_w - needed_w) / 2 : 0;
    // Anchor at the top: the soft keyboard, when shown, occupies the
    // bottom of the LCD and would otherwise overlap a vertically-centred
    // terminal grid.
    origin_y_ = 0;
    return {};
}

term::Result<void> M5GfxDisplay::draw_cells(uint16_t row, uint16_t col,
                                            std::span<const term::Cell> cells) {
    if (cell_w_ == 0 || cell_h_ == 0) {
        return std::unexpected(term::Error::NotInitialized);
    }
    auto& d = M5.Display;
    d.startWrite();
    int y = origin_y_ + int(row) * cell_h_;
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& cell = cells[i];
        if (cell.attrs.wide_cont) continue;  // covered by the leading half

        // Bold remaps standard 8 -> bright 8 (the ANSI convention).
        term::Color fg_src = cell.fg;
        if (cell.attrs.bold &&
            fg_src.kind == term::Color::Kind::Index &&
            fg_src.value < 8) {
            fg_src = term::Color::indexed(uint8_t(fg_src.value + 8));
        }
        uint16_t fg = color_to_565(fg_src, 0xFFFFFF);
        uint16_t bg = color_to_565(cell.bg, 0x000000);
        if (cell.attrs.reverse) std::swap(fg, bg);

        int x = origin_x_ + (int(col) + int(i)) * cell_w_;
        int w = cell.attrs.wide ? (cell_w_ * 2) : cell_w_;
        d.fillRect(x, y, w, cell_h_, bg);

        // Skip rendering for blank cells — fillRect already painted the bg.
        if (cell.ch == U' ' || cell.ch == 0) continue;

        d.setTextColor(fg, bg);
        d.setCursor(x, y);
        char utf8[5];
        size_t n = encode_utf8(cell.ch, utf8);
        utf8[n] = '\0';
        d.print(utf8);
        if (cell.attrs.bold) {
            // No bold IPA variant ships with M5GFX; thicken via overstrike.
            d.setTextColor(fg);
            d.setCursor(x + 1, y);
            d.print(utf8);
        }
    }
    d.endWrite();
    return {};
}

term::Result<void> M5GfxDisplay::flush(term::DamageRect) {
    return {};  // immediate-mode rendering
}

void M5GfxDisplay::set_grid_size(uint16_t cols, uint16_t rows) {
    cols_ = cols;
    rows_ = rows;
    uint16_t needed_w = uint32_t(cell_w_) * cols_;
    uint16_t screen_w = M5.Display.width();
    origin_x_ = (screen_w > needed_w) ? (screen_w - needed_w) / 2 : 0;
    origin_y_ = 0;
}

void M5GfxDisplay::bell() {
    // Tab5 routes audio through an I2S codec. M5Unified's Speaker_Class
    // compiles under our IDF 6 shim; the runtime path is best-effort.
    // Short, mid-frequency tone — about as terminal-bell-ish as it gets.
    M5.Speaker.tone(880.0f, 60);
}

}  // namespace tab5
