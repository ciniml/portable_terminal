#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "term_core/cell.hpp"
#include "term_core/error.hpp"
#include "term_core/idisplay.hpp"
#include "term_core/parser.hpp"

namespace term {

class Screen : public IParserSink {
public:
    Screen(uint16_t cols, uint16_t rows);

    uint16_t cols() const { return cols_; }
    uint16_t rows() const { return rows_; }

    // Read-only cell access. Caller must keep row<rows() and col<cols().
    const Cell& at(uint16_t row, uint16_t col) const;

    uint16_t cursor_row() const { return cursor_row_; }
    uint16_t cursor_col() const { return cursor_col_; }

    // Damage region (smallest bounding box of all cells modified since
    // the last take_damage() call). After taking, the region is reset.
    DamageRect take_damage();
    bool dirty() const { return !damage_.empty(); }

    void reset();

    // IParserSink:
    void put_char(char32_t ch) override;
    void execute(uint8_t c0) override;
    void csi(char final_byte,
             std::span<const int> params,
             std::span<const uint8_t> intermediates,
             bool private_marker) override;
    void esc(char final_byte,
             std::span<const uint8_t> intermediates) override;
    void osc(std::span<const uint8_t> data) override;

private:
    Cell& mut(uint16_t row, uint16_t col);
    void mark(uint16_t row, uint16_t col);
    void mark_range(uint16_t row, uint16_t c0, uint16_t c1);
    void mark_rect(uint16_t r0, uint16_t c0, uint16_t r1, uint16_t c1);

    void place_cursor(uint16_t row, uint16_t col);
    void carriage_return();
    void line_feed();         // moves down, scrolls within scroll region
    void reverse_index();     // ESC M
    void backspace();
    void tab();

    void scroll_up(uint16_t n);    // content moves up, lines vacated at bottom
    void scroll_down(uint16_t n);  // content moves down, vacated at top

    void erase_display(int mode);
    void erase_line(int mode);
    void clear_cells(uint16_t r0, uint16_t c0, uint16_t r1, uint16_t c1);

    void apply_sgr(std::span<const int> params);

    int param_or(std::span<const int> p, int idx, int def) const;

    uint16_t cols_;
    uint16_t rows_;
    std::vector<Cell> grid_;

    uint16_t cursor_row_{0};
    uint16_t cursor_col_{0};
    bool wrap_pending_{false};  // VT100 "last column" deferred wrap

    // Scroll region: [scroll_top_, scroll_bot_) inclusive top, exclusive bot.
    uint16_t scroll_top_{0};
    uint16_t scroll_bot_{0};  // == rows_ when full screen

    // Saved state (DECSC / DECRC).
    uint16_t saved_row_{0};
    uint16_t saved_col_{0};
    Attrs saved_attrs_{};
    Color saved_fg_{Color::default_color()};
    Color saved_bg_{Color::default_color()};
    bool has_saved_{false};

    // Current SGR state.
    Attrs cur_attrs_{};
    Color cur_fg_{Color::default_color()};
    Color cur_bg_{Color::default_color()};

    DamageRect damage_{};
};

}  // namespace term
