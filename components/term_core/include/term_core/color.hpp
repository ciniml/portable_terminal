#pragma once

#include <cstdint>

namespace term {

struct Color {
    enum class Kind : uint8_t { Default, Index, Rgb };
    Kind kind{Kind::Default};
    uint32_t value{0};

    static constexpr Color default_color() { return {Kind::Default, 0}; }
    static constexpr Color indexed(uint8_t i) { return {Kind::Index, i}; }
    static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
        return {Kind::Rgb, (uint32_t(r) << 16) | (uint32_t(g) << 8) | b};
    }

    constexpr bool operator==(const Color&) const = default;
};

}  // namespace term
