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

#include "input_touch.hpp"

namespace tab5 {

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

    // Called by the menu whenever it needs a redraw (state change,
    // selection update). Caller installs a lambda that takes the lock,
    // re-marks the obscured region if closing, then calls render().
    void set_repaint(Repaint r) { repaint_ = std::move(r); }

private:
    enum class State : uint8_t {
        Hidden,
        ProfileList,
        // ProfileEditor, TofuList — step 3 / step 5
    };

    State   state_ = State::Hidden;
    int     pressed_idx_ = -1;   // currently-touched list row, -1 if none
    int     pressed_btn_ = -1;   // -1 generic / 0..N button id within row
    Repaint repaint_;

    void render_profile_list();

    // Hit-test helpers — return -1 if no hit.
    int   hit_row(int y) const;          // y → row index, -1 outside list
    bool  hit_close(int x, int y) const; // header X
    bool  hit_delete(int x) const;       // row-local: in delete button x-range
    bool  hit_add(int x, int y) const;   // bottom "+ Add" button
};

extern Menu menu;

}  // namespace tab5
