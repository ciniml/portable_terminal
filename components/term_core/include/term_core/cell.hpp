#pragma once

#include <cstdint>

#include "term_core/color.hpp"

namespace term {

struct Attrs {
    bool bold : 1 = false;
    bool faint : 1 = false;
    bool italic : 1 = false;
    bool underline : 1 = false;
    bool blink : 1 = false;
    bool reverse : 1 = false;
    bool hidden : 1 = false;
    bool strike : 1 = false;

    // Wide-character (East Asian fullwidth) layout.
    //   wide      : this cell holds the leading half of a 2-cell glyph
    //   wide_cont : this cell is the continuation half (renderer should
    //               skip drawing — the wide glyph spans into it)
    bool wide : 1 = false;
    bool wide_cont : 1 = false;

    constexpr bool operator==(const Attrs&) const = default;
};

struct Cell {
    char32_t ch{U' '};
    Color fg{Color::default_color()};
    Color bg{Color::default_color()};
    Attrs attrs{};

    constexpr bool operator==(const Cell&) const = default;
};

}  // namespace term
