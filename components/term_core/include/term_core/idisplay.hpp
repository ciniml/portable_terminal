#pragma once

#include <cstdint>
#include <span>

#include "term_core/cell.hpp"
#include "term_core/error.hpp"

namespace term {

struct DamageRect {
    uint16_t row0{0};
    uint16_t col0{0};
    uint16_t row1{0};  // exclusive
    uint16_t col1{0};  // exclusive

    constexpr bool empty() const { return row1 <= row0 || col1 <= col0; }
};

class IDisplay {
public:
    virtual ~IDisplay() = default;

    virtual Result<void> init() = 0;

    virtual uint16_t cols() const = 0;
    virtual uint16_t rows() const = 0;

    virtual Result<void> draw_cells(uint16_t row, uint16_t col,
                                    std::span<const Cell> cells) = 0;

    virtual Result<void> flush(DamageRect r) = 0;
};

}  // namespace term
