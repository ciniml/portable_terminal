// On-screen software keyboard for the Tab5 GT911 touch panel.
//
// Layout: a 1120 px wide × 384 px tall panel that overlays the bottom of
// the LCD when shown. Six rows × fourteen 80-px slots: F-keys / nav row,
// number row, two alpha rows, shift+arrows+ins/del, modifier+space row.
// A persistent toggle button in the left margin opens / closes the panel
// without typing. Modifier keys (Shift / Ctrl / Alt) are sticky one-shot.
//
// All bytes go out through a caller-supplied ByteSink. Rendering is
// passive — call render() under the global UI lock from app_main; the
// keyboard never touches M5GFX on its own thread.
#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "byte_input.hpp"
#include "input_touch.hpp"

namespace tab5 {

class SoftKeyboard {
public:
    explicit SoftKeyboard(ByteSink sink);

    // Process a touch event. Returns true if the event was consumed
    // (toggle pressed, key pressed, etc.) so the caller can suppress
    // further handlers. Calls may emit bytes through the sink and / or
    // request a redraw via the caller-installed redraw callback.
    bool handle_touch(const TouchPoint& p);

    // Repaint everything (buttons + panel-if-visible). Caller holds UI lock.
    void render();
    // Granular paints used by the compositor when only part of the
    // keyboard region is exposed.
    void render_buttons();
    void render_panel();

    // Mark dirty cells underneath when keyboard is hidden so the
    // terminal can repaint underneath. Returns the rectangular region
    // (in screen-pixel coordinates) that the keyboard previously
    // occupied; caller uses it to mark the matching terminal rows
    // dirty. {x0,y0,x1,y1}.
    struct Rect { int x0, y0, x1, y1; };
    Rect panel_rect() const;
    Rect toggle_rect() const;
    Rect menu_rect() const;
    // Union of the two always-visible left-margin buttons.
    Rect buttons_rect() const;

    void toggle();
    bool visible() const { return visible_; }

    // Caller installs a callback that requests a re-render (typically
    // wraps "Lock + render()" with terminal repaint of uncovered area).
    using Repaint = std::function<void()>;
    void set_repaint(Repaint r) { repaint_ = std::move(r); }

    // Called when the user taps the ☰ Menu button. Caller wires this to
    // open the settings menu overlay.
    using MenuOpen = std::function<void()>;
    void set_on_menu(MenuOpen f) { on_menu_ = std::move(f); }

private:
    struct Key {
        const char* label;
        uint8_t base;       // base ASCII byte for plain key, 0 for special
        uint8_t shifted;    // ASCII when Shift armed, 0 if same as base
        enum class Kind : uint8_t {
            Char, Esc, Tab, Enter, Backspace, Space,
            Up, Down, Left, Right,
            Home, End, PageUp, PageDown, Insert, Delete,
            F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
            Shift, Ctrl, Alt, Hide,
        } kind = Kind::Char;
        uint8_t width = 1;  // in slot units
    };

    static constexpr int kRows  = 6;
    static constexpr int kSlots = 14;

    // Geometry
    static constexpr int kPanelX = 0;
    static constexpr int kPanelY = 336;
    static constexpr int kPanelW = 1120;
    static constexpr int kPanelH = 384;
    static constexpr int kSlotW  = kPanelW / kSlots;   // 80
    static constexpr int kRowH   = kPanelH / kRows;    // 64
    // Toggle button lives in the left margin, away from both the terminal
    // grid (x>=160) and the keyboard panel (y>=336). Always visible.
    static constexpr int kTglX = 20, kTglY = 20, kTglW = 120, kTglH = 50;

    // ☰ Menu button. Placed below the kbd toggle in the same margin.
    // Always visible too; tapping invokes the menu open callback.
    static constexpr int kMenuX = 20, kMenuY = 80, kMenuW = 120, kMenuH = 50;

    // Layout: one std::span<const Key> per row, populated at startup.
    void build_layout();

    void draw_key(int row, int slot_start, int slot_w, const char* label,
                  bool armed, bool pressed);
    void draw_toggle();

    bool hit_panel(int x, int y) const;
    bool hit_toggle(int x, int y) const;
    bool hit_menu(int x, int y) const;
    int key_at(int x, int y, int* row_out, int* slot_out) const;
    void emit_key(const Key& k);

    ByteSink sink_;
    Repaint  repaint_;
    MenuOpen on_menu_;
    bool visible_ = false;

    bool shift_armed_ = false;
    bool ctrl_armed_  = false;
    bool alt_armed_   = false;

    int pressed_row_  = -1;
    int pressed_slot_ = -1;

    // Each row is a fixed-size array of up to kSlots Keys. Unused slots
    // have width 0 and are skipped.
    Key rows_[kRows][kSlots];
};

}  // namespace tab5
