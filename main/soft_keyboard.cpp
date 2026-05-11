#include "soft_keyboard.hpp"

#include <cstring>

#include "M5Unified.h"

namespace tab5 {

namespace {

constexpr uint16_t kBgColor      = 0x10A2;  // dark slate
constexpr uint16_t kKeyFg        = 0xFFFF;
constexpr uint16_t kKeyBg        = 0x4208;
constexpr uint16_t kKeyEdge      = 0x8410;
constexpr uint16_t kKeyArmedBg   = 0xFD00;  // amber
constexpr uint16_t kKeyArmedFg   = 0x0000;
constexpr uint16_t kKeyPressedBg = 0x07FF;  // cyan
constexpr uint16_t kKeyPressedFg = 0x0000;
constexpr uint16_t kTglBg        = 0x2965;
constexpr uint16_t kTglEdge      = 0xC618;
constexpr uint16_t kTglFg        = 0xFFFF;

}  // namespace

SoftKeyboard::SoftKeyboard(ByteSink sink) : sink_(std::move(sink)) {
    build_layout();
}

void SoftKeyboard::build_layout() {
    using Kind = Key::Kind;
    auto K = [](const char* l, uint8_t b, uint8_t s = 0,
                Kind k = Kind::Char, uint8_t w = 1) -> Key {
        return Key{l, b, s, k, w};
    };

    // Row 0: F1..F12 Home End  (14 slots)
    rows_[0][0]  = K("F1",  0, 0, Kind::F1);
    rows_[0][1]  = K("F2",  0, 0, Kind::F2);
    rows_[0][2]  = K("F3",  0, 0, Kind::F3);
    rows_[0][3]  = K("F4",  0, 0, Kind::F4);
    rows_[0][4]  = K("F5",  0, 0, Kind::F5);
    rows_[0][5]  = K("F6",  0, 0, Kind::F6);
    rows_[0][6]  = K("F7",  0, 0, Kind::F7);
    rows_[0][7]  = K("F8",  0, 0, Kind::F8);
    rows_[0][8]  = K("F9",  0, 0, Kind::F9);
    rows_[0][9]  = K("F10", 0, 0, Kind::F10);
    rows_[0][10] = K("F11", 0, 0, Kind::F11);
    rows_[0][11] = K("F12", 0, 0, Kind::F12);
    rows_[0][12] = K("Home", 0, 0, Kind::Home);
    rows_[0][13] = K("End",  0, 0, Kind::End);

    // Row 1: Esc 1..0 - = BS  (1 + 12 + 1 = 14)
    rows_[1][0]  = K("Esc", 0, 0, Kind::Esc);
    rows_[1][1]  = K("1", '1', '!');
    rows_[1][2]  = K("2", '2', '@');
    rows_[1][3]  = K("3", '3', '#');
    rows_[1][4]  = K("4", '4', '$');
    rows_[1][5]  = K("5", '5', '%');
    rows_[1][6]  = K("6", '6', '^');
    rows_[1][7]  = K("7", '7', '&');
    rows_[1][8]  = K("8", '8', '*');
    rows_[1][9]  = K("9", '9', '(');
    rows_[1][10] = K("0", '0', ')');
    rows_[1][11] = K("-", '-', '_');
    rows_[1][12] = K("=", '=', '+');
    rows_[1][13] = K("BS", 0, 0, Kind::Backspace);

    // Row 2: Tab q w e r t y u i o p [ ] \  (14)
    rows_[2][0]  = K("Tab", 0, 0, Kind::Tab);
    const char* qwerty = "qwertyuiop";
    for (int i = 0; i < 10; ++i) {
        char b = qwerty[i];
        char s = static_cast<char>(b - 0x20);
        static char buf[10][2];
        buf[i][0] = b;
        buf[i][1] = 0;
        rows_[2][1 + i] = K(buf[i], static_cast<uint8_t>(b),
                            static_cast<uint8_t>(s));
    }
    rows_[2][11] = K("[", '[', '{');
    rows_[2][12] = K("]", ']', '}');
    rows_[2][13] = K("\\", '\\', '|');

    // Row 3: Ctrl a s d f g h j k l ; ' Enter(w=2)  (12 + 2 = 14)
    rows_[3][0] = K("Ctrl", 0, 0, Kind::Ctrl);
    const char* asdf = "asdfghjkl";
    for (int i = 0; i < 9; ++i) {
        char b = asdf[i];
        char s = static_cast<char>(b - 0x20);
        static char buf[9][2];
        buf[i][0] = b;
        buf[i][1] = 0;
        rows_[3][1 + i] = K(buf[i], static_cast<uint8_t>(b),
                            static_cast<uint8_t>(s));
    }
    rows_[3][10] = K(";", ';', ':');
    rows_[3][11] = K("'", '\'', '"');
    rows_[3][12] = K("Enter", 0, 0, Kind::Enter, 2);

    // Row 4: Shift(w=2) z x c v b n m , . / Up Ins Del  (2+7+3+1+1+1 = 14... = 2+7+3+1+1=14? recount)
    // 2 (Shift) + 7 (zxcvbnm) + 3 (,./) + 1 (Up) + 1 (Ins) + 1 (Del) = wait that's only 15
    // Let me lay it out by slot index:
    //   0..1  Shift (w=2)
    //   2..8  z x c v b n m   (7 keys)
    //   9..11 , . /            (3 keys)
    //   12    Up
    //   13    Ins  -- only one of Ins/Del fits here. Move Del to row 5.
    rows_[4][0] = K("Shift", 0, 0, Kind::Shift, 2);
    const char* zxcv = "zxcvbnm";
    for (int i = 0; i < 7; ++i) {
        char b = zxcv[i];
        char s = static_cast<char>(b - 0x20);
        static char buf[7][2];
        buf[i][0] = b;
        buf[i][1] = 0;
        rows_[4][2 + i] = K(buf[i], static_cast<uint8_t>(b),
                            static_cast<uint8_t>(s));
    }
    rows_[4][9]  = K(",", ',', '<');
    rows_[4][10] = K(".", '.', '>');
    rows_[4][11] = K("/", '/', '?');
    rows_[4][12] = K("Up",  0, 0, Kind::Up);
    rows_[4][13] = K("Ins", 0, 0, Kind::Insert);

    // Row 5: Hide Alt Space(w=5) Alt Del PgU PgD Lt Dn Rt
    //   1 + 1 + 5 + 1 + 1 + 1 + 1 + 1 + 1 + 1 = 14
    rows_[5][0]  = K("Hide", 0, 0, Kind::Hide);
    rows_[5][1]  = K("Alt",  0, 0, Kind::Alt);
    rows_[5][2]  = K("Space", ' ', 0, Kind::Space, 5);
    // slots 3..6 absorbed
    rows_[5][7]  = K("Alt", 0, 0, Kind::Alt);
    rows_[5][8]  = K("Del", 0, 0, Kind::Delete);
    rows_[5][9]  = K("PgU", 0, 0, Kind::PageUp);
    rows_[5][10] = K("PgD", 0, 0, Kind::PageDown);
    rows_[5][11] = K("Lt",  0, 0, Kind::Left);
    rows_[5][12] = K("Dn",  0, 0, Kind::Down);
    rows_[5][13] = K("Rt",  0, 0, Kind::Right);
}

// ----- geometry helpers -----

SoftKeyboard::Rect SoftKeyboard::panel_rect() const {
    return {kPanelX, kPanelY, kPanelX + kPanelW, kPanelY + kPanelH};
}

SoftKeyboard::Rect SoftKeyboard::toggle_rect() const {
    return {kTglX, kTglY, kTglX + kTglW, kTglY + kTglH};
}

SoftKeyboard::Rect SoftKeyboard::menu_rect() const {
    return {kMenuX, kMenuY, kMenuX + kMenuW, kMenuY + kMenuH};
}

SoftKeyboard::Rect SoftKeyboard::buttons_rect() const {
    return {kTglX, kTglY, kTglX + kTglW, kMenuY + kMenuH};
}

bool SoftKeyboard::hit_menu(int x, int y) const {
    return x >= kMenuX && x < kMenuX + kMenuW &&
           y >= kMenuY && y < kMenuY + kMenuH;
}

bool SoftKeyboard::hit_panel(int x, int y) const {
    return x >= kPanelX && x < kPanelX + kPanelW &&
           y >= kPanelY && y < kPanelY + kPanelH;
}

bool SoftKeyboard::hit_toggle(int x, int y) const {
    return x >= kTglX && x < kTglX + kTglW &&
           y >= kTglY && y < kTglY + kTglH;
}

int SoftKeyboard::key_at(int x, int y, int* row_out, int* slot_out) const {
    int row  = (y - kPanelY) / kRowH;
    int slot = (x - kPanelX) / kSlotW;
    if (row < 0 || row >= kRows || slot < 0 || slot >= kSlots) return -1;
    // If slot points into a wider key, walk backward to its start.
    while (slot > 0 && rows_[row][slot].width == 0) --slot;
    if (rows_[row][slot].label == nullptr) return -1;
    *row_out  = row;
    *slot_out = slot;
    return 0;
}

// ----- drawing -----

void SoftKeyboard::draw_key(int row, int slot_start, int slot_w,
                            const char* label, bool armed, bool pressed) {
    auto& d = M5.Display;
    int x = kPanelX + slot_start * kSlotW + 3;
    int y = kPanelY + row * kRowH + 3;
    int w = slot_w * kSlotW - 6;
    int h = kRowH - 6;

    uint16_t bg = pressed ? kKeyPressedBg : (armed ? kKeyArmedBg : kKeyBg);
    uint16_t fg = pressed ? kKeyPressedFg : (armed ? kKeyArmedFg : kKeyFg);
    d.fillRoundRect(x, y, w, h, 6, bg);
    d.drawRoundRect(x, y, w, h, 6, kKeyEdge);

    d.setTextColor(fg, bg);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.drawString(label, x + w / 2, y + h / 2);
}

void SoftKeyboard::draw_toggle() {
    auto& d = M5.Display;
    d.fillRoundRect(kTglX, kTglY, kTglW, kTglH, 8, kTglBg);
    d.drawRoundRect(kTglX, kTglY, kTglW, kTglH, 8, kTglEdge);
    d.setTextColor(kTglFg, kTglBg);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.drawString(visible_ ? "Hide kbd" : "Show kbd",
                 kTglX + kTglW / 2, kTglY + kTglH / 2);

    d.fillRoundRect(kMenuX, kMenuY, kMenuW, kMenuH, 8, kTglBg);
    d.drawRoundRect(kMenuX, kMenuY, kMenuW, kMenuH, 8, kTglEdge);
    d.setTextColor(kTglFg, kTglBg);
    d.drawString("Menu", kMenuX + kMenuW / 2, kMenuY + kMenuH / 2);
}

void SoftKeyboard::render_buttons() {
    draw_toggle();
    auto& d = M5.Display;
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.setTextDatum(top_left);
}

void SoftKeyboard::render_panel() {
    using Kind = Key::Kind;
    if (!visible_) return;
    auto& d = M5.Display;
    d.fillRect(kPanelX, kPanelY, kPanelW, kPanelH, kBgColor);
    for (int row = 0; row < kRows; ++row) {
        int slot = 0;
        while (slot < kSlots) {
            const Key& k = rows_[row][slot];
            if (!k.label || k.width == 0) { ++slot; continue; }
            bool armed = (k.kind == Kind::Shift && shift_armed_) ||
                         (k.kind == Kind::Ctrl  && ctrl_armed_) ||
                         (k.kind == Kind::Alt   && alt_armed_);
            bool pressed = (pressed_row_ == row && pressed_slot_ == slot);
            draw_key(row, slot, k.width, k.label, armed, pressed);
            slot += k.width;
        }
    }
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.setTextDatum(top_left);
}

void SoftKeyboard::render() {
    render_buttons();
    render_panel();
}

// ----- byte emission -----

namespace {
void send_seq(ByteSink& sink, std::span<const uint8_t> bytes) {
    sink(bytes);
}
}  // namespace

void SoftKeyboard::emit_key(const Key& k) {
    using Kind = Key::Kind;
    if (!sink_) return;

    auto consume_one_shots = [&] {
        shift_armed_ = ctrl_armed_ = alt_armed_ = false;
    };

    switch (k.kind) {
        case Kind::Shift:
            shift_armed_ = !shift_armed_;
            return;
        case Kind::Ctrl:
            ctrl_armed_ = !ctrl_armed_;
            return;
        case Kind::Alt:
            alt_armed_ = !alt_armed_;
            return;
        case Kind::Hide:
            toggle();
            return;
        default:
            break;
    }

    uint8_t prefix[1] = { 0x1B };
    bool with_alt = alt_armed_;

    auto maybe_alt = [&](std::span<const uint8_t> body) {
        if (with_alt) send_seq(sink_, std::span<const uint8_t>(prefix, 1));
        send_seq(sink_, body);
        consume_one_shots();
    };

    auto lit = [](const char* s, size_t n) {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s), n);
    };

    switch (k.kind) {
        case Kind::Esc:       maybe_alt(lit("\x1B", 1));    return;
        case Kind::Tab:       maybe_alt(lit("\t", 1));      return;
        case Kind::Enter:     maybe_alt(lit("\r", 1));      return;
        case Kind::Backspace: maybe_alt(lit("\x7F", 1));    return;
        case Kind::Space:     maybe_alt(lit(" ", 1));       return;
        case Kind::Up:        maybe_alt(lit("\x1B[A", 3));  return;
        case Kind::Down:      maybe_alt(lit("\x1B[B", 3));  return;
        case Kind::Right:     maybe_alt(lit("\x1B[C", 3));  return;
        case Kind::Left:      maybe_alt(lit("\x1B[D", 3));  return;
        case Kind::Home:      maybe_alt(lit("\x1B[H", 3));  return;
        case Kind::End:       maybe_alt(lit("\x1B[F", 3));  return;
        case Kind::PageUp:    maybe_alt(lit("\x1B[5~", 4)); return;
        case Kind::PageDown:  maybe_alt(lit("\x1B[6~", 4)); return;
        case Kind::Insert:    maybe_alt(lit("\x1B[2~", 4)); return;
        case Kind::Delete:    maybe_alt(lit("\x1B[3~", 4)); return;
        case Kind::F1:        maybe_alt(lit("\x1BOP", 3));  return;
        case Kind::F2:        maybe_alt(lit("\x1BOQ", 3));  return;
        case Kind::F3:        maybe_alt(lit("\x1BOR", 3));  return;
        case Kind::F4:        maybe_alt(lit("\x1BOS", 3));  return;
        case Kind::F5:        maybe_alt(lit("\x1B[15~", 5)); return;
        case Kind::F6:        maybe_alt(lit("\x1B[17~", 5)); return;
        case Kind::F7:        maybe_alt(lit("\x1B[18~", 5)); return;
        case Kind::F8:        maybe_alt(lit("\x1B[19~", 5)); return;
        case Kind::F9:        maybe_alt(lit("\x1B[20~", 5)); return;
        case Kind::F10:       maybe_alt(lit("\x1B[21~", 5)); return;
        case Kind::F11:       maybe_alt(lit("\x1B[23~", 5)); return;
        case Kind::F12:       maybe_alt(lit("\x1B[24~", 5)); return;
        default: break;
    }

    // Char (or shifted variant). Ctrl maps 'a'..'z' / '@'..'_' to 0x01..0x1F.
    uint8_t b = shift_armed_ && k.shifted ? k.shifted : k.base;
    if (b == 0) return;

    if (ctrl_armed_) {
        // Standard Ctrl-key mapping: 'a'..'z' -> 0x01..0x1A,
        // '@'..'_' -> 0x00..0x1F. Shift on letters is irrelevant.
        if (b >= 'a' && b <= 'z') b = static_cast<uint8_t>(b - 'a' + 1);
        else if (b >= '@' && b <= '_') b = static_cast<uint8_t>(b - '@');
        // else: pass through
    }

    uint8_t one[1] = { b };
    maybe_alt(std::span<const uint8_t>(one, 1));
}

bool SoftKeyboard::handle_touch(const TouchPoint& p) {
    // Always-on toggle button.
    if (hit_toggle(p.x, p.y)) {
        if (p.event == TouchEvent::Up) toggle();
        return true;
    }
    // ☰ Menu button — only acts on touch Up so a stray Down then drag
    // away doesn't fire.
    if (hit_menu(p.x, p.y)) {
        if (p.event == TouchEvent::Up && on_menu_) on_menu_();
        return true;
    }

    if (!visible_) return false;
    if (!hit_panel(p.x, p.y)) return false;

    int row = 0, slot = 0;
    if (key_at(p.x, p.y, &row, &slot) < 0) return true;
    const Key& k = rows_[row][slot];

    // Press-highlight changes only repaint the panel itself — calling
    // the general repaint_ callback here would trigger the compositor's
    // full panel-rect invalidate on every Down/Move/Up.
    if (p.event == TouchEvent::Down) {
        pressed_row_  = row;
        pressed_slot_ = slot;
        render_panel();
        return true;
    }
    if (p.event == TouchEvent::Move) {
        if (pressed_row_ != row || pressed_slot_ != slot) {
            pressed_row_ = pressed_slot_ = -1;
            render_panel();
        }
        return true;
    }
    if (p.event == TouchEvent::Up) {
        bool same = (pressed_row_ == row && pressed_slot_ == slot);
        pressed_row_ = pressed_slot_ = -1;
        if (same) emit_key(k);
        render_panel();
        return true;
    }
    return true;
}

void SoftKeyboard::toggle() {
    visible_ = !visible_;
    if (repaint_) repaint_();
}

}  // namespace tab5
