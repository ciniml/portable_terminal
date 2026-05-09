#pragma once

#include <cstdint>

#include "term_core/idisplay.hpp"
#include "term_core/screen.hpp"

namespace tab5 {

// Renders a block cursor (cell-sized inversion) at the terminal cursor
// position, on top of whatever the IDisplay backend has already drawn.
//
// The renderer is "edge-triggered": call sync_after_render() right after
// terminal.render_dirty() to redraw the cursor at its current position
// (render_dirty will have overwritten any previous cursor visualization).
// Call toggle_blink() from a periodic timer to flash it.
//
// Not thread-safe. The caller is expected to hold a single mutex around
// any sequence that involves both terminal mutation and cursor rendering.
class CursorRenderer {
public:
    CursorRenderer(const term::Screen& screen, term::IDisplay& display);

    // Erase whatever cursor visualization is currently on screen, if any.
    void erase();

    // Draw the cursor at the terminal's current cursor position.
    void draw();

    // Convenience: erase old + draw at current position. Use right after
    // terminal.render_dirty() to keep the cursor tracking the screen.
    void sync_after_render();

    // Flip visibility (blink). Idempotent across two calls.
    void toggle_blink();

    // Force the cursor visible / hidden regardless of current state.
    void set_visible(bool visible);

private:
    const term::Screen& screen_;
    term::IDisplay& display_;
    bool drawn_{false};
    uint16_t drawn_row_{0};
    uint16_t drawn_col_{0};
};

}  // namespace tab5
