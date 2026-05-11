// Tiny on-screen layer compositor.
//
// Each visual component (terminal, status panel, soft keyboard, menu) is
// registered as a layer with a z-order and a paint function. When the
// region of the screen one layer covered becomes exposed — e.g. the menu
// closes, the keyboard hides — the orchestrator black-fills the damaged
// rectangle and walks every registered layer from bottom up, asking each
// to repaint its intersection. Layers that aren't currently visible or
// don't overlap the damage simply do nothing.
//
// The compositor doesn't own bitmaps, just paint callbacks; it relies on
// every layer's paint_fn to re-issue its draw calls. Callers must hold
// the global UI lock when invoking invalidate() — paint functions touch
// M5GFX directly.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

namespace tab5::ui_root {

struct Rect {
    int x0, y0, x1, y1;
};

constexpr bool empty(const Rect& r) {
    return r.x1 <= r.x0 || r.y1 <= r.y0;
}

constexpr Rect intersect(const Rect& a, const Rect& b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0),
            std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

constexpr Rect kFullScreen = {0, 0, 1280, 720};

using PaintFn = std::function<void(const Rect& damage)>;

// Register a layer. Lower z is painted first (bottom of the stack).
// `paint` is invoked from invalidate() with the current damage rect; it
// must early-out if its own bounds don't intersect the damage.
void register_layer(int z, PaintFn paint);

// Black-fill `damage` then walk every registered layer in z order,
// invoking its paint function. Must be called under the UI lock.
void invalidate(const Rect& damage);

// Whether any modal overlay (menu) is currently above the terminal —
// terminal RX / cursor blink check this to avoid painting through
// the overlay.
bool overlay_active();
void set_overlay_active(bool active);

}  // namespace tab5::ui_root
