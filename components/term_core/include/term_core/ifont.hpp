#pragma once

#include <cstdint>
#include <span>

namespace term {

class IFont {
public:
    virtual ~IFont() = default;

    virtual uint8_t cell_w() const = 0;
    virtual uint8_t cell_h() const = 0;

    virtual std::span<const uint8_t> glyph(char32_t ch) const = 0;
};

}  // namespace term
