// Touch input plumbing. A dedicated FreeRTOS task polls M5.update() and
// dispatches edge events (down / move / up) to a caller-supplied sink.
// The actual UI on top — soft keyboard, status-bar buttons — lives in
// later layers.
#pragma once

#include <cstdint>
#include <functional>

namespace tab5 {

enum class TouchEvent : uint8_t {
    Down,
    Move,
    Up,
};

struct TouchPoint {
    TouchEvent event;
    int16_t    x;
    int16_t    y;
};

using TouchSink = std::function<void(const TouchPoint&)>;

// Spawn the polling task. Calls M5.update() periodically and reports
// state transitions for finger 0 to `sink`. Safe to call once at boot.
void start_touch_input(TouchSink sink);

}  // namespace tab5
