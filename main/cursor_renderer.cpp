#include "cursor_renderer.hpp"

#include <span>

namespace tab5 {

CursorRenderer::CursorRenderer(const term::Screen& screen,
                               term::IDisplay& display)
    : screen_(screen), display_(display) {}

void CursorRenderer::erase() {
    if (!drawn_) return;
    if (drawn_row_ >= screen_.rows() || drawn_col_ >= screen_.cols()) {
        drawn_ = false;
        return;
    }
    term::Cell c = screen_.at(drawn_row_, drawn_col_);
    (void)display_.draw_cells(drawn_row_, drawn_col_,
                              std::span<const term::Cell>(&c, 1));
    term::DamageRect r{drawn_row_, drawn_col_,
                       static_cast<uint16_t>(drawn_row_ + 1),
                       static_cast<uint16_t>(drawn_col_ + 1)};
    (void)display_.flush(r);
    drawn_ = false;
}

void CursorRenderer::draw() {
    uint16_t row = screen_.cursor_row();
    uint16_t col = screen_.cursor_col();
    if (row >= screen_.rows() || col >= screen_.cols()) return;

    if (drawn_) erase();

    // Honour DECTCEM (?25) — apps toggle this to suppress the cursor while
    // they redraw their UI.
    if (!screen_.cursor_visible()) return;

    term::Cell c = screen_.at(row, col);
    c.attrs.reverse = !c.attrs.reverse;
    (void)display_.draw_cells(row, col,
                              std::span<const term::Cell>(&c, 1));
    term::DamageRect r{row, col,
                       static_cast<uint16_t>(row + 1),
                       static_cast<uint16_t>(col + 1)};
    (void)display_.flush(r);
    drawn_ = true;
    drawn_row_ = row;
    drawn_col_ = col;
}

void CursorRenderer::sync_after_render() {
    // render_dirty() repainted underlying cells (including any that previously
    // showed the cursor visualization), so our `drawn_` state is logically
    // false now. Force a fresh draw at the current cursor position.
    drawn_ = false;
    draw();
}

void CursorRenderer::toggle_blink() {
    if (drawn_) {
        erase();
    } else if (screen_.cursor_visible()) {
        draw();
    }
}

void CursorRenderer::set_visible(bool visible) {
    if (visible) draw(); else erase();
}

}  // namespace tab5
