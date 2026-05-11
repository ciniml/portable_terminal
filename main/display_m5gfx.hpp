#pragma once

#include <cstdint>
#include <span>

#include "term_core/cell.hpp"
#include "term_core/idisplay.hpp"

namespace tab5 {

// IDisplay backend for the Tab5 LCD via M5Unified / M5GFX. Uses a fixed
// monospace bitmap font and draws cells immediately (no off-screen buffer)
// — flush() is a no-op.
class M5GfxDisplay final : public term::IDisplay {
public:
    M5GfxDisplay(uint16_t cols, uint16_t rows);

    term::Result<void> init() override;

    uint16_t cols() const override { return cols_; }
    uint16_t rows() const override { return rows_; }

    term::Result<void> draw_cells(uint16_t row, uint16_t col,
                                  std::span<const term::Cell> cells) override;
    term::Result<void> flush(term::DamageRect r) override;
    void bell() override;

private:
    uint16_t cols_;
    uint16_t rows_;
    uint16_t cell_w_{12};
    uint16_t cell_h_{16};
    uint16_t origin_x_{0};
    uint16_t origin_y_{0};
};

}  // namespace tab5
