#include "term_core/terminal.hpp"

#include <vector>

namespace term {

Terminal::Terminal(uint16_t cols, uint16_t rows, IDisplay& display)
    : screen_(cols, rows), display_(&display) {}

Result<void> Terminal::feed(std::span<const uint8_t> bytes) {
    parser_.feed(bytes, screen_);
    return {};
}

Result<void> Terminal::feed(std::span<const std::byte> bytes) {
    return feed(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}

Result<void> Terminal::feed(std::string_view s) {
    return feed(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

Result<void> Terminal::render_dirty() {
    if (!display_) return std::unexpected(Error::NotInitialized);
    DamageRect r = screen_.take_damage();
    if (r.empty()) return {};

    std::vector<Cell> row_buf;
    row_buf.reserve(r.col1 - r.col0);
    for (uint16_t row = r.row0; row < r.row1; ++row) {
        row_buf.clear();
        for (uint16_t col = r.col0; col < r.col1; ++col) {
            row_buf.push_back(screen_.at(row, col));
        }
        auto rc = display_->draw_cells(row, r.col0, std::span<const Cell>(row_buf));
        if (!rc) return rc;
    }
    return display_->flush(r);
}

}  // namespace term
