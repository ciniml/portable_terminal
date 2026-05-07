#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "term_core/cell.hpp"
#include "term_core/idisplay.hpp"

namespace term::testing {

class FakeDisplay final : public IDisplay {
public:
    FakeDisplay(uint16_t cols, uint16_t rows)
        : cols_(cols), rows_(rows), grid_(static_cast<size_t>(cols) * rows) {}

    Result<void> init() override { return {}; }
    uint16_t cols() const override { return cols_; }
    uint16_t rows() const override { return rows_; }

    Result<void> draw_cells(uint16_t row, uint16_t col,
                            std::span<const Cell> cells) override {
        ++draw_calls_;
        for (size_t i = 0; i < cells.size(); ++i) {
            grid_[size_t(row) * cols_ + col + i] = cells[i];
        }
        return {};
    }

    Result<void> flush(DamageRect r) override {
        ++flush_calls_;
        last_flush_ = r;
        return {};
    }

    const Cell& at(uint16_t row, uint16_t col) const {
        return grid_[size_t(row) * cols_ + col];
    }

    int draw_calls() const { return draw_calls_; }
    int flush_calls() const { return flush_calls_; }
    DamageRect last_flush() const { return last_flush_; }

private:
    uint16_t cols_;
    uint16_t rows_;
    std::vector<Cell> grid_;
    int draw_calls_{0};
    int flush_calls_{0};
    DamageRect last_flush_{};
};

}  // namespace term::testing
