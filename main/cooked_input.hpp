#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace tab5 {

// Per-stream input filter that turns the byte stream a typical line-mode
// host TTY sends into one a VT100 expects:
//
//  - lone CR (Enter)        -> CR LF
//  - lone LF                -> CR LF
//  - CR LF                  -> CR LF (pass-through, not double-mapped)
//  - 0x08 (BS) / 0x7F (DEL) -> BS SP BS  (visible erase)
//
// Stateful: tracks the previous byte to recognise CR-LF pairs across
// chunk boundaries. Output is appended into the caller's buffer.
class CookedInputFilter {
public:
    void process(std::span<const uint8_t> in, std::vector<uint8_t>& out);

private:
    bool prev_was_cr_{false};
};

}  // namespace tab5
