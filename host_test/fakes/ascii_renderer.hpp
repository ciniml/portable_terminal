#pragma once

#include <string>

#include "term_core/screen.hpp"

namespace term::testing {

// Render a Screen as a newline-joined ASCII grid. Non-ASCII / control
// characters become '.', spaces remain ' '. Trailing whitespace per row
// is preserved so tests can pin column counts exactly.
inline std::string ascii_dump(const Screen& s) {
    std::string out;
    out.reserve(static_cast<size_t>(s.rows()) * (s.cols() + 1));
    for (uint16_t r = 0; r < s.rows(); ++r) {
        for (uint16_t c = 0; c < s.cols(); ++c) {
            char32_t cp = s.at(r, c).ch;
            out.push_back((cp >= 0x20 && cp < 0x7F) ? char(cp) : '.');
        }
        out.push_back('\n');
    }
    return out;
}

}  // namespace term::testing
