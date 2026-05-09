#pragma once

#include <cstdint>
#include <functional>
#include <span>

namespace tab5 {

// Sink invoked from an input source's RX task whenever new bytes arrive.
// Keep the callback short — it runs on the source's own task.
using ByteSink = std::function<void(std::span<const uint8_t>)>;

}  // namespace tab5
