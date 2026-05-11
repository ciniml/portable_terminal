#include "ui_root.hpp"

#include <atomic>
#include <vector>

#include "M5Unified.h"

namespace tab5::ui_root {

namespace {

struct Layer {
    int     z;
    PaintFn paint;
};

std::vector<Layer>  g_layers;
std::atomic<bool>   g_overlay{false};

}  // namespace

void register_layer(int z, PaintFn paint) {
    g_layers.push_back({z, std::move(paint)});
    std::stable_sort(g_layers.begin(), g_layers.end(),
                     [](const Layer& a, const Layer& b) { return a.z < b.z; });
}

void invalidate(const Rect& d) {
    if (empty(d)) return;
    M5.Display.fillRect(d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0, TFT_BLACK);
    for (const auto& l : g_layers) {
        l.paint(d);
    }
}

bool overlay_active()              { return g_overlay.load(); }
void set_overlay_active(bool a)    { g_overlay.store(a); }

}  // namespace tab5::ui_root
