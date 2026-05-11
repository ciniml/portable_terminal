#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "term_core/error.hpp"
#include "term_core/idisplay.hpp"
#include "term_core/parser.hpp"
#include "term_core/screen.hpp"

namespace term {

// Top-level glue: owns Screen + Parser, drives an IDisplay.
class Terminal {
public:
    Terminal(uint16_t cols, uint16_t rows, IDisplay& display);

    Result<void> feed(std::span<const std::byte> bytes);
    Result<void> feed(std::span<const uint8_t> bytes);
    Result<void> feed(std::string_view s);

    // Push the smallest dirty rectangle out to the IDisplay, then mark clean.
    Result<void> render_dirty();

    const Screen& screen() const { return screen_; }
    Screen& screen() { return screen_; }

    // Change the grid dimensions. Forwards to Screen::resize. The caller
    // is responsible for notifying the display backend (so it can update
    // its own cached geometry) and the remote pty (SIGWINCH).
    void resize(uint16_t cols, uint16_t rows) { screen_.resize(cols, rows); }

private:
    Screen screen_;
    Parser parser_;
    IDisplay* display_;
};

}  // namespace term
