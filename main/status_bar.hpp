// Right-margin status panel (160 px wide × 720 px tall).
// Renders battery %, charging state, Wi-Fi IP, and SSH state. Owned by a
// dedicated FreeRTOS task that refreshes every 5 s. All M5GFX access goes
// through the caller-supplied lock callback so it stays serialised with
// the terminal/cursor renderer.
#pragma once

#include <functional>

namespace tab5 {

using StatusLock = std::function<void(std::function<void()>)>;

// Spawn the status-bar task. `lock` is called for every refresh and is
// expected to take the global UI mutex around its argument.
// Safe to call once during boot.
void start_status_bar(StatusLock lock);

}  // namespace tab5
