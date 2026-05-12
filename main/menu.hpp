// On-device settings menu: profile list / editor / TOFU manager.
//
// Modal full-screen overlay accessed via the ☰ button in the left
// margin (drawn by SoftKeyboard alongside the keyboard toggle). When
// `visible()` is true, touch events route to handle_touch() instead of
// the keyboard, and the terminal beneath is logically obscured —
// closing the menu re-marks the affected cells dirty so the terminal
// repaints.
//
// Rendering is passive: app_main holds the global UI lock and calls
// render() on visibility transitions and after handle_touch(). The
// menu never touches M5GFX on its own.
#pragma once

#include <cstdint>
#include <functional>

#include <vector>

#include "byte_input.hpp"
#include "input_touch.hpp"
#include "profiles.hpp"
#include "tofu.hpp"

namespace tab5 {

class SoftKeyboard;

class Menu {
public:
    using Repaint = std::function<void()>;

    Menu();

    // Process a touch event. Returns true if consumed.
    bool handle_touch(const TouchPoint& p);

    // Paint the current view. Caller must hold the UI lock.
    void render();

    bool visible() const { return state_ != State::Hidden; }
    void open();
    void close();

    // Open the profile editor for a new (idx<0) or existing profile.
    // Pulls the soft keyboard up if it isn't already and redirects its
    // byte sink into the editor's focused field. The keyboard's sink
    // is restored when the editor closes (save or cancel).
    void open_editor(int idx);
    void close_editor(bool save);

    // Editor-only: bytes coming back from the soft keyboard while the
    // editor is up. Public so app_main can route through here when
    // installing the swapped sink.
    void editor_feed(std::span<const uint8_t> bytes);

    // Called by the menu whenever it needs a redraw (state change,
    // selection update). Caller installs a lambda that takes the lock,
    // re-marks the obscured region if closing, then calls render().
    void set_repaint(Repaint r) { repaint_ = std::move(r); }

    // The profile editor needs to redirect the keyboard's byte sink
    // into its own field buffers. app_main injects the SoftKeyboard
    // instance after constructing it.
    void bind_keyboard(SoftKeyboard* kbd) { kbd_ = kbd; }

private:
    enum class State : uint8_t {
        Hidden,
        ProfileList,
        ProfileEditor,
        TofuList,
    };

    State   state_ = State::Hidden;
    int     pressed_idx_ = -1;   // currently-touched list row, -1 if none
    int     pressed_btn_ = -1;   // -1 generic / 0..N button id within row
    Repaint repaint_;

    // ---- ProfileEditor state ----
    struct Editor {
        Profile  working;        // working copy
        int      editing_idx;    // -1 = new, else slot to overwrite
        int      focused_field;  // -1 = none, else field index
        int      pressed_field;  // -1 = none, else field tap in progress
        bool     pressed_save;
        bool     pressed_cancel;
        bool     pressed_show_pw;
        bool     show_password;  // render password unmasked
        ByteSink saved_sink;     // restore on close
    };
    Editor editor_{};
    SoftKeyboard* kbd_ = nullptr;

    // ---- TofuList state ----
    struct TofuView {
        std::vector<tofu::Entry> entries;
        int  pressed_idx;   // row index
        int  pressed_btn;   // 0 generic / 1 delete
        bool pressed_back;
    };
    TofuView tofu_{};

    void render_profile_list();
    void render_profile_editor();
    void render_tofu_list();
    bool handle_touch_profile_list(const TouchPoint& p);
    bool handle_touch_profile_editor(const TouchPoint& p);
    bool handle_touch_tofu_list(const TouchPoint& p);

    void open_tofu();
    void refresh_tofu();

    // Hit-test helpers — return -1 if no hit.
    int   hit_row(int y) const;            // y → row index, -1 outside list
    bool  hit_close(int x, int y) const;   // header X
    bool  hit_edit(int x) const;           // row-local: edit button x-range
    bool  hit_delete(int x) const;         // row-local: delete button x-range
    bool  hit_add(int x, int y) const;     // bottom "+ Add" button

    int   hit_editor_field(int x, int y) const;  // -1 if no hit
    bool  hit_editor_save(int x, int y) const;
    bool  hit_editor_cancel(int x, int y) const;
    bool  hit_editor_show_pw(int x, int y) const;

    int   hit_tofu_row(int y) const;
    bool  hit_tofu_delete(int x) const;
    bool  hit_tofu_back(int x, int y) const;
    bool  hit_manage_tofu(int x, int y) const;   // on profile list
};

extern Menu menu;

}  // namespace tab5
