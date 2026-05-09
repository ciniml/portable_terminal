#include "term_core/screen.hpp"

#include <algorithm>
#include <cstdint>

#include "term_core/east_asian_width.hpp"

namespace term {

namespace {

constexpr uint16_t kTabWidth = 8;

uint16_t clamp_pos(int v, uint16_t lo, uint16_t hi_inclusive) {
    if (v < int(lo)) return lo;
    if (v > int(hi_inclusive)) return hi_inclusive;
    return static_cast<uint16_t>(v);
}

}  // namespace

Screen::Screen(uint16_t cols, uint16_t rows)
    : cols_(cols),
      rows_(rows),
      grid_(static_cast<size_t>(cols) * rows),
      scroll_top_(0),
      scroll_bot_(rows) {
    backup_.grid.resize(static_cast<size_t>(cols) * rows);
    backup_.scroll_bot = rows;
}

const Cell& Screen::at(uint16_t row, uint16_t col) const {
    return grid_[static_cast<size_t>(row) * cols_ + col];
}

Cell& Screen::mut(uint16_t row, uint16_t col) {
    return grid_[static_cast<size_t>(row) * cols_ + col];
}

void Screen::mark(uint16_t row, uint16_t col) {
    mark_rect(row, col, row + 1, col + 1);
}

void Screen::mark_range(uint16_t row, uint16_t c0, uint16_t c1) {
    if (c1 <= c0) return;
    mark_rect(row, c0, row + 1, c1);
}

void Screen::mark_rect(uint16_t r0, uint16_t c0, uint16_t r1, uint16_t c1) {
    if (r1 <= r0 || c1 <= c0) return;
    if (damage_.empty()) {
        damage_ = {r0, c0, r1, c1};
        return;
    }
    damage_.row0 = std::min(damage_.row0, r0);
    damage_.col0 = std::min(damage_.col0, c0);
    damage_.row1 = std::max(damage_.row1, r1);
    damage_.col1 = std::max(damage_.col1, c1);
}

DamageRect Screen::take_damage() {
    DamageRect r = damage_;
    damage_ = {};
    return r;
}

void Screen::reset() {
    std::fill(grid_.begin(), grid_.end(), Cell{});
    cursor_row_ = 0;
    cursor_col_ = 0;
    wrap_pending_ = false;
    autowrap_ = true;
    cursor_visible_ = true;
    scroll_top_ = 0;
    scroll_bot_ = rows_;
    cur_attrs_ = {};
    cur_fg_ = Color::default_color();
    cur_bg_ = Color::default_color();
    has_saved_ = false;
    mark_rect(0, 0, rows_, cols_);
}

void Screen::place_cursor(uint16_t row, uint16_t col) {
    cursor_row_ = std::min<uint16_t>(row, rows_ ? rows_ - 1 : 0);
    cursor_col_ = std::min<uint16_t>(col, cols_ ? cols_ - 1 : 0);
    wrap_pending_ = false;
}

void Screen::carriage_return() {
    cursor_col_ = 0;
    wrap_pending_ = false;
}

void Screen::line_feed() {
    wrap_pending_ = false;
    if (cursor_row_ + 1 < scroll_bot_) {
        ++cursor_row_;
    } else if (cursor_row_ + 1 == scroll_bot_) {
        scroll_up(1);
    } else {
        // outside scroll region; just bump until hitting last row.
        if (cursor_row_ + 1 < rows_) ++cursor_row_;
    }
}

void Screen::reverse_index() {
    wrap_pending_ = false;
    if (cursor_row_ > scroll_top_) {
        --cursor_row_;
    } else if (cursor_row_ == scroll_top_) {
        scroll_down(1);
    } else {
        if (cursor_row_ > 0) --cursor_row_;
    }
}

void Screen::backspace() {
    if (cursor_col_ > 0) --cursor_col_;
    wrap_pending_ = false;
}

void Screen::tab() {
    if (cols_ == 0) return;
    uint16_t next = (cursor_col_ / kTabWidth + 1) * kTabWidth;
    if (next >= cols_) next = cols_ - 1;
    cursor_col_ = next;
    wrap_pending_ = false;
}

void Screen::scroll_up(uint16_t n) {
    if (scroll_bot_ <= scroll_top_ || n == 0) return;
    uint16_t region = scroll_bot_ - scroll_top_;
    if (n >= region) {
        clear_cells(scroll_top_, 0, scroll_bot_, cols_);
        return;
    }
    for (uint16_t r = scroll_top_; r + n < scroll_bot_; ++r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            mut(r, c) = at(r + n, c);
        }
    }
    for (uint16_t r = scroll_bot_ - n; r < scroll_bot_; ++r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            mut(r, c) = Cell{U' ', cur_fg_, cur_bg_, cur_attrs_};
        }
    }
    mark_rect(scroll_top_, 0, scroll_bot_, cols_);
}

void Screen::scroll_down(uint16_t n) {
    if (scroll_bot_ <= scroll_top_ || n == 0) return;
    uint16_t region = scroll_bot_ - scroll_top_;
    if (n >= region) {
        clear_cells(scroll_top_, 0, scroll_bot_, cols_);
        return;
    }
    for (int r = int(scroll_bot_) - 1; r >= int(scroll_top_) + int(n); --r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            mut(static_cast<uint16_t>(r), c) =
                at(static_cast<uint16_t>(r - n), c);
        }
    }
    for (uint16_t r = scroll_top_; r < scroll_top_ + n; ++r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            mut(r, c) = Cell{U' ', cur_fg_, cur_bg_, cur_attrs_};
        }
    }
    mark_rect(scroll_top_, 0, scroll_bot_, cols_);
}

void Screen::insert_lines(uint16_t n) {
    if (cursor_row_ < scroll_top_ || cursor_row_ >= scroll_bot_) return;
    if (n == 0) n = 1;
    uint16_t region = static_cast<uint16_t>(scroll_bot_ - cursor_row_);
    if (n > region) n = region;
    // shift down
    for (int r = int(scroll_bot_) - 1; r >= int(cursor_row_) + int(n); --r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            mut(static_cast<uint16_t>(r), c) =
                at(static_cast<uint16_t>(r - n), c);
        }
    }
    Cell blank{U' ', cur_fg_, cur_bg_, cur_attrs_};
    for (uint16_t r = cursor_row_;
         r < cursor_row_ + n && r < scroll_bot_; ++r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            mut(r, c) = blank;
        }
    }
    mark_rect(cursor_row_, 0, scroll_bot_, cols_);
    cursor_col_ = 0;
    wrap_pending_ = false;
}

void Screen::delete_lines(uint16_t n) {
    if (cursor_row_ < scroll_top_ || cursor_row_ >= scroll_bot_) return;
    if (n == 0) n = 1;
    uint16_t region = static_cast<uint16_t>(scroll_bot_ - cursor_row_);
    if (n > region) n = region;
    for (uint16_t r = cursor_row_; r + n < scroll_bot_; ++r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            mut(r, c) = at(static_cast<uint16_t>(r + n), c);
        }
    }
    Cell blank{U' ', cur_fg_, cur_bg_, cur_attrs_};
    for (uint16_t r = static_cast<uint16_t>(scroll_bot_ - n);
         r < scroll_bot_; ++r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            mut(r, c) = blank;
        }
    }
    mark_rect(cursor_row_, 0, scroll_bot_, cols_);
    cursor_col_ = 0;
    wrap_pending_ = false;
}

void Screen::insert_chars(uint16_t n) {
    if (cursor_col_ >= cols_) return;
    if (n == 0) n = 1;
    if (n > cols_ - cursor_col_) n = static_cast<uint16_t>(cols_ - cursor_col_);
    for (int c = int(cols_) - 1; c >= int(cursor_col_) + int(n); --c) {
        mut(cursor_row_, static_cast<uint16_t>(c)) =
            at(cursor_row_, static_cast<uint16_t>(c - n));
    }
    Cell blank{U' ', cur_fg_, cur_bg_, cur_attrs_};
    for (uint16_t c = cursor_col_; c < cursor_col_ + n; ++c) {
        mut(cursor_row_, c) = blank;
    }
    mark_range(cursor_row_, cursor_col_, cols_);
    wrap_pending_ = false;
}

void Screen::delete_chars(uint16_t n) {
    if (cursor_col_ >= cols_) return;
    if (n == 0) n = 1;
    if (n > cols_ - cursor_col_) n = static_cast<uint16_t>(cols_ - cursor_col_);
    for (uint16_t c = cursor_col_; c + n < cols_; ++c) {
        mut(cursor_row_, c) = at(cursor_row_, static_cast<uint16_t>(c + n));
    }
    Cell blank{U' ', cur_fg_, cur_bg_, cur_attrs_};
    for (uint16_t c = static_cast<uint16_t>(cols_ - n); c < cols_; ++c) {
        mut(cursor_row_, c) = blank;
    }
    mark_range(cursor_row_, cursor_col_, cols_);
    wrap_pending_ = false;
}

void Screen::erase_chars(uint16_t n) {
    if (cursor_col_ >= cols_) return;
    if (n == 0) n = 1;
    uint16_t end = static_cast<uint16_t>(cursor_col_ + n);
    if (end > cols_) end = cols_;
    Cell blank{U' ', cur_fg_, cur_bg_, cur_attrs_};
    for (uint16_t c = cursor_col_; c < end; ++c) {
        mut(cursor_row_, c) = blank;
    }
    mark_range(cursor_row_, cursor_col_, end);
    wrap_pending_ = false;
}

void Screen::set_dec_modes(std::span<const int> params, bool set) {
    for (int p : params) {
        switch (p) {
            case 7:  autowrap_ = set; break;
            case 25: cursor_visible_ = set; break;
            case 47:
            case 1047:
            case 1049:
                if (set) enter_alt_screen(); else exit_alt_screen();
                break;
            // Other DEC private modes (mouse, paste, etc.) are not yet
            // implemented; silently ignored.
            default: break;
        }
    }
}

void Screen::swap_with_backup() {
    using std::swap;
    swap(grid_, backup_.grid);
    swap(cursor_row_, backup_.cursor_row);
    swap(cursor_col_, backup_.cursor_col);
    swap(wrap_pending_, backup_.wrap_pending);
    swap(scroll_top_, backup_.scroll_top);
    swap(scroll_bot_, backup_.scroll_bot);
    swap(cur_attrs_, backup_.cur_attrs);
    swap(cur_fg_, backup_.cur_fg);
    swap(cur_bg_, backup_.cur_bg);
    swap(saved_row_, backup_.saved_row);
    swap(saved_col_, backup_.saved_col);
    swap(saved_attrs_, backup_.saved_attrs);
    swap(saved_fg_, backup_.saved_fg);
    swap(saved_bg_, backup_.saved_bg);
    swap(has_saved_, backup_.has_saved);
}

void Screen::enter_alt_screen() {
    if (alt_active_) return;
    swap_with_backup();
    // Newly-active alt screen starts cleared with default state.
    Cell blank{};
    std::fill(grid_.begin(), grid_.end(), blank);
    cursor_row_ = 0;
    cursor_col_ = 0;
    wrap_pending_ = false;
    scroll_top_ = 0;
    scroll_bot_ = rows_;
    cur_attrs_ = {};
    cur_fg_ = Color::default_color();
    cur_bg_ = Color::default_color();
    has_saved_ = false;
    alt_active_ = true;
    mark_rect(0, 0, rows_, cols_);
}

void Screen::exit_alt_screen() {
    if (!alt_active_) return;
    swap_with_backup();
    alt_active_ = false;
    mark_rect(0, 0, rows_, cols_);
}

void Screen::clear_cells(uint16_t r0, uint16_t c0, uint16_t r1, uint16_t c1) {
    if (r1 <= r0 || c1 <= c0) return;
    Cell blank{U' ', cur_fg_, cur_bg_, cur_attrs_};
    for (uint16_t r = r0; r < r1; ++r)
        for (uint16_t c = c0; c < c1; ++c)
            mut(r, c) = blank;
    mark_rect(r0, c0, r1, c1);
}

void Screen::erase_display(int mode) {
    switch (mode) {
        case 0:  // cursor → end
            clear_cells(cursor_row_, cursor_col_, cursor_row_ + 1, cols_);
            if (cursor_row_ + 1 < rows_)
                clear_cells(cursor_row_ + 1, 0, rows_, cols_);
            break;
        case 1:  // start → cursor
            if (cursor_row_ > 0)
                clear_cells(0, 0, cursor_row_, cols_);
            clear_cells(cursor_row_, 0, cursor_row_ + 1, cursor_col_ + 1);
            break;
        case 2:
        case 3:
            clear_cells(0, 0, rows_, cols_);
            break;
        default:
            break;
    }
}

void Screen::erase_line(int mode) {
    switch (mode) {
        case 0:
            clear_cells(cursor_row_, cursor_col_, cursor_row_ + 1, cols_);
            break;
        case 1:
            clear_cells(cursor_row_, 0, cursor_row_ + 1, cursor_col_ + 1);
            break;
        case 2:
            clear_cells(cursor_row_, 0, cursor_row_ + 1, cols_);
            break;
        default:
            break;
    }
}

int Screen::param_or(std::span<const int> p, int idx, int def) const {
    if (idx >= int(p.size())) return def;
    int v = p[idx];
    return v == 0 ? def : v;  // SGR uses 0 as "reset"; for cursor, 0 means default
}

void Screen::apply_sgr(std::span<const int> params) {
    if (params.empty()) {
        cur_attrs_ = {};
        cur_fg_ = Color::default_color();
        cur_bg_ = Color::default_color();
        return;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        int p = params[i];
        switch (p) {
            case 0:
                cur_attrs_ = {};
                cur_fg_ = Color::default_color();
                cur_bg_ = Color::default_color();
                break;
            case 1: cur_attrs_.bold = true; break;
            case 2: cur_attrs_.faint = true; break;
            case 3: cur_attrs_.italic = true; break;
            case 4: cur_attrs_.underline = true; break;
            case 5: cur_attrs_.blink = true; break;
            case 7: cur_attrs_.reverse = true; break;
            case 8: cur_attrs_.hidden = true; break;
            case 9: cur_attrs_.strike = true; break;
            case 22: cur_attrs_.bold = false; cur_attrs_.faint = false; break;
            case 23: cur_attrs_.italic = false; break;
            case 24: cur_attrs_.underline = false; break;
            case 25: cur_attrs_.blink = false; break;
            case 27: cur_attrs_.reverse = false; break;
            case 28: cur_attrs_.hidden = false; break;
            case 29: cur_attrs_.strike = false; break;
            case 39: cur_fg_ = Color::default_color(); break;
            case 49: cur_bg_ = Color::default_color(); break;
            case 38:
                if (i + 1 < params.size()) {
                    if (params[i + 1] == 5 && i + 2 < params.size()) {
                        cur_fg_ = Color::indexed(static_cast<uint8_t>(params[i + 2]));
                        i += 2;
                    } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                        cur_fg_ = Color::rgb(static_cast<uint8_t>(params[i + 2]),
                                             static_cast<uint8_t>(params[i + 3]),
                                             static_cast<uint8_t>(params[i + 4]));
                        i += 4;
                    } else {
                        // malformed; skip remainder safely
                        i = params.size();
                    }
                }
                break;
            case 48:
                if (i + 1 < params.size()) {
                    if (params[i + 1] == 5 && i + 2 < params.size()) {
                        cur_bg_ = Color::indexed(static_cast<uint8_t>(params[i + 2]));
                        i += 2;
                    } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                        cur_bg_ = Color::rgb(static_cast<uint8_t>(params[i + 2]),
                                             static_cast<uint8_t>(params[i + 3]),
                                             static_cast<uint8_t>(params[i + 4]));
                        i += 4;
                    } else {
                        i = params.size();
                    }
                }
                break;
            default:
                if (p >= 30 && p <= 37) {
                    cur_fg_ = Color::indexed(static_cast<uint8_t>(p - 30));
                } else if (p >= 40 && p <= 47) {
                    cur_bg_ = Color::indexed(static_cast<uint8_t>(p - 40));
                } else if (p >= 90 && p <= 97) {
                    cur_fg_ = Color::indexed(static_cast<uint8_t>(p - 90 + 8));
                } else if (p >= 100 && p <= 107) {
                    cur_bg_ = Color::indexed(static_cast<uint8_t>(p - 100 + 8));
                }
                break;
        }
    }
}

// IParserSink ----------------------------------------------------------------

void Screen::clear_orphan_pair(uint16_t row, uint16_t col,
                               const Cell& fill_cell) {
    if (col >= cols_) return;
    const Cell& victim = at(row, col);
    if (victim.attrs.wide && col + 1 < cols_) {
        // Replacing the leading half — strip continuation next.
        Cell& next = mut(row, static_cast<uint16_t>(col + 1));
        if (next.attrs.wide_cont) {
            next = fill_cell;
            mark(row, static_cast<uint16_t>(col + 1));
        }
    }
    if (victim.attrs.wide_cont && col > 0) {
        Cell& prev = mut(row, static_cast<uint16_t>(col - 1));
        if (prev.attrs.wide) {
            prev = fill_cell;
            mark(row, static_cast<uint16_t>(col - 1));
        }
    }
}

void Screen::put_char(char32_t ch) {
    if (cols_ == 0 || rows_ == 0) return;
    bool wide = is_wide_char(ch);

    // Honour deferred wrap from a previous put_char that filled the last col.
    if (wrap_pending_) {
        cursor_col_ = 0;
        line_feed();
        wrap_pending_ = false;
    }
    // Wide char that would not fit on the current line — wrap first.
    if (wide && cursor_col_ + 1 >= cols_) {
        if (!autowrap_) return;  // can't fit and wrap is disabled — drop
        cursor_col_ = 0;
        line_feed();
    }

    Cell blank{U' ', cur_fg_, cur_bg_, cur_attrs_};

    if (wide) {
        // Clean up orphans on either side of the new pair before installing.
        clear_orphan_pair(cursor_row_, cursor_col_, blank);
        clear_orphan_pair(cursor_row_,
                          static_cast<uint16_t>(cursor_col_ + 1), blank);
        Cell head{ch, cur_fg_, cur_bg_, cur_attrs_};
        head.attrs.wide = true;
        Cell cont = blank;
        cont.attrs.wide_cont = true;
        mut(cursor_row_, cursor_col_) = head;
        mut(cursor_row_, static_cast<uint16_t>(cursor_col_ + 1)) = cont;
        mark(cursor_row_, cursor_col_);
        mark(cursor_row_, static_cast<uint16_t>(cursor_col_ + 1));
        if (cursor_col_ + 2 < cols_) {
            cursor_col_ = static_cast<uint16_t>(cursor_col_ + 2);
        } else {
            cursor_col_ = static_cast<uint16_t>(cursor_col_ + 1);
            if (autowrap_) wrap_pending_ = true;
        }
        return;
    }

    clear_orphan_pair(cursor_row_, cursor_col_, blank);
    Cell c{ch, cur_fg_, cur_bg_, cur_attrs_};
    mut(cursor_row_, cursor_col_) = c;
    mark(cursor_row_, cursor_col_);
    if (cursor_col_ + 1 < cols_) {
        ++cursor_col_;
    } else if (autowrap_) {
        // last column: defer wrap until next printable char.
        wrap_pending_ = true;
    }
    // else: cursor stays at last column; subsequent prints overwrite.
}


void Screen::execute(uint8_t c0) {
    switch (c0) {
        case 0x07: break;  // BEL
        case 0x08: backspace(); break;
        case 0x09: tab(); break;
        case 0x0A:
        case 0x0B:
        case 0x0C: line_feed(); break;
        case 0x0D: carriage_return(); break;
        default: break;
    }
}

void Screen::csi(char final_byte,
                 std::span<const int> params,
                 std::span<const uint8_t> intermediates,
                 bool private_marker) {
    (void)intermediates;
    auto get = [&](size_t i, int def) -> int {
        if (i >= params.size()) return def;
        int v = params[i];
        return v == 0 ? def : v;
    };

    switch (final_byte) {
        case 'A': {
            int n = get(0, 1);
            int r = int(cursor_row_) - n;
            place_cursor(clamp_pos(r, 0, rows_ ? rows_ - 1 : 0), cursor_col_);
            break;
        }
        case 'B': {
            int n = get(0, 1);
            int r = int(cursor_row_) + n;
            place_cursor(clamp_pos(r, 0, rows_ ? rows_ - 1 : 0), cursor_col_);
            break;
        }
        case 'C': {
            int n = get(0, 1);
            int c = int(cursor_col_) + n;
            place_cursor(cursor_row_, clamp_pos(c, 0, cols_ ? cols_ - 1 : 0));
            break;
        }
        case 'D': {
            int n = get(0, 1);
            int c = int(cursor_col_) - n;
            place_cursor(cursor_row_, clamp_pos(c, 0, cols_ ? cols_ - 1 : 0));
            break;
        }
        case 'E': {
            int n = get(0, 1);
            place_cursor(clamp_pos(int(cursor_row_) + n, 0, rows_ ? rows_ - 1 : 0), 0);
            break;
        }
        case 'F': {
            int n = get(0, 1);
            place_cursor(clamp_pos(int(cursor_row_) - n, 0, rows_ ? rows_ - 1 : 0), 0);
            break;
        }
        case 'G': {
            int c = get(0, 1) - 1;
            place_cursor(cursor_row_, clamp_pos(c, 0, cols_ ? cols_ - 1 : 0));
            break;
        }
        case 'd': {
            int r = get(0, 1) - 1;
            place_cursor(clamp_pos(r, 0, rows_ ? rows_ - 1 : 0), cursor_col_);
            break;
        }
        case 'H':
        case 'f': {
            int r = get(0, 1) - 1;
            int c = get(1, 1) - 1;
            place_cursor(clamp_pos(r, 0, rows_ ? rows_ - 1 : 0),
                         clamp_pos(c, 0, cols_ ? cols_ - 1 : 0));
            break;
        }
        case 'J':
            erase_display(params.empty() ? 0 : params[0]);
            break;
        case 'K':
            erase_line(params.empty() ? 0 : params[0]);
            break;
        case 'L':
            insert_lines(static_cast<uint16_t>(get(0, 1)));
            break;
        case 'M':
            delete_lines(static_cast<uint16_t>(get(0, 1)));
            break;
        case 'P':
            delete_chars(static_cast<uint16_t>(get(0, 1)));
            break;
        case 'S':
            scroll_up(static_cast<uint16_t>(get(0, 1)));
            break;
        case 'T':
            scroll_down(static_cast<uint16_t>(get(0, 1)));
            break;
        case 'X':
            erase_chars(static_cast<uint16_t>(get(0, 1)));
            break;
        case '@':
            insert_chars(static_cast<uint16_t>(get(0, 1)));
            break;
        case 'h':
            if (private_marker) set_dec_modes(params, true);
            break;
        case 'l':
            if (private_marker) set_dec_modes(params, false);
            break;
        case 'm':
            if (!private_marker) apply_sgr(params);
            break;
        case 'r': {
            int top = get(0, 1) - 1;
            int bot = params.size() >= 2 && params[1] != 0 ? params[1] : rows_;
            top = std::max(0, top);
            bot = std::min(int(rows_), bot);
            if (bot > top) {
                scroll_top_ = static_cast<uint16_t>(top);
                scroll_bot_ = static_cast<uint16_t>(bot);
                place_cursor(0, 0);
            }
            break;
        }
        case 's':
            saved_row_ = cursor_row_;
            saved_col_ = cursor_col_;
            saved_attrs_ = cur_attrs_;
            saved_fg_ = cur_fg_;
            saved_bg_ = cur_bg_;
            has_saved_ = true;
            break;
        case 'u':
            if (has_saved_) {
                place_cursor(saved_row_, saved_col_);
                cur_attrs_ = saved_attrs_;
                cur_fg_ = saved_fg_;
                cur_bg_ = saved_bg_;
            }
            break;
        default:
            break;
    }
}

void Screen::esc(char final_byte, std::span<const uint8_t> intermediates) {
    if (!intermediates.empty()) return;
    switch (final_byte) {
        case '7':
            saved_row_ = cursor_row_;
            saved_col_ = cursor_col_;
            saved_attrs_ = cur_attrs_;
            saved_fg_ = cur_fg_;
            saved_bg_ = cur_bg_;
            has_saved_ = true;
            break;
        case '8':
            if (has_saved_) {
                place_cursor(saved_row_, saved_col_);
                cur_attrs_ = saved_attrs_;
                cur_fg_ = saved_fg_;
                cur_bg_ = saved_bg_;
            }
            break;
        case 'D': line_feed(); break;
        case 'M': reverse_index(); break;
        case 'E': carriage_return(); line_feed(); break;
        case 'c': reset(); break;
        default: break;
    }
}

void Screen::osc(std::span<const uint8_t>) {
    // Ignored in Phase 1.
}

}  // namespace term
