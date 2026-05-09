#include "term_core/parser.hpp"
#include "term_core/screen.hpp"

#include <gtest/gtest.h>

namespace {

void feed(term::Parser& p, term::Screen& s, std::string_view str) {
    p.feed(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(str.data()), str.size()), s);
}

}  // namespace

TEST(AltScreen, NotActiveByDefault) {
    term::Screen s(10, 3);
    EXPECT_FALSE(s.alt_active());
}

TEST(AltScreen, Enter1049ClearsAndSwitches) {
    term::Parser p;
    term::Screen s(8, 3);
    feed(p, s, "primary");
    EXPECT_FALSE(s.alt_active());
    feed(p, s, "\x1b[?1049h");
    EXPECT_TRUE(s.alt_active());
    // Alt screen starts cleared.
    for (uint16_t c = 0; c < 8; ++c) EXPECT_EQ(s.at(0, c).ch, U' ');
    EXPECT_EQ(s.cursor_row(), 0);
    EXPECT_EQ(s.cursor_col(), 0);
}

TEST(AltScreen, ExitRestoresPrimaryContent) {
    term::Parser p;
    term::Screen s(8, 3);
    feed(p, s, "primary");
    feed(p, s, "\x1b[?1049h");
    feed(p, s, "alt-only");
    feed(p, s, "\x1b[?1049l");
    EXPECT_FALSE(s.alt_active());
    EXPECT_EQ(s.at(0, 0).ch, U'p');
    EXPECT_EQ(s.at(0, 1).ch, U'r');
    EXPECT_EQ(s.at(0, 2).ch, U'i');
    EXPECT_EQ(s.at(0, 6).ch, U'y');
}

TEST(AltScreen, CursorPositionRestoredOnExit) {
    term::Parser p;
    term::Screen s(8, 3);
    feed(p, s, "abc");                  // cursor now at (0, 3)
    feed(p, s, "\x1b[?1049h");          // enter alt; primary cursor saved
    feed(p, s, "X\r\nY");               // alt cursor moves around
    feed(p, s, "\x1b[?1049l");          // back to primary
    EXPECT_EQ(s.cursor_row(), 0);
    EXPECT_EQ(s.cursor_col(), 3);
}

TEST(AltScreen, ScrollRegionIsPerScreen) {
    term::Parser p;
    term::Screen s(8, 5);
    feed(p, s, "\x1b[2;4r");            // primary scroll region rows 2..4
    feed(p, s, "\x1b[?1049h");
    // Alt enters with full-screen scroll region.
    feed(p, s, "\x1b[?1049l");
    // Back on primary — scroll region should still be rows 2..4 because we
    // saved/restored it on swap. Verify by checking that an LF at the
    // bottom of the region scrolls (rather than continuing to row 4+).
    feed(p, s, "\x1b[4;1H");            // cursor at row 4 (last of region)
    feed(p, s, "X\n");                  // LF should scroll inside region, not advance
    EXPECT_EQ(s.cursor_row(), 3);       // stayed at last row of region
}

TEST(AltScreen, SgrIsPerScreen) {
    term::Parser p;
    term::Screen s(8, 2);
    feed(p, s, "\x1b[31m");              // primary fg = red
    feed(p, s, "\x1b[?1049h");
    // Alt entered with default SGR — write should be default fg.
    feed(p, s, "A");
    EXPECT_EQ(s.at(0, 0).fg.kind, term::Color::Kind::Default);
    feed(p, s, "\x1b[?1049l");
    // Back to primary; SGR should still be red.
    feed(p, s, "B");
    EXPECT_EQ(s.at(0, 0).fg.kind, term::Color::Kind::Index);
    EXPECT_EQ(s.at(0, 0).fg.value, 1u);  // red
}

TEST(AltScreen, IdempotentEnterExit) {
    term::Parser p;
    term::Screen s(4, 2);
    feed(p, s, "AB");
    feed(p, s, "\x1b[?1049h");
    feed(p, s, "\x1b[?1049h");          // enter while already in alt — no-op
    feed(p, s, "X");
    feed(p, s, "\x1b[?1049l");
    feed(p, s, "\x1b[?1049l");          // exit while already in primary — no-op
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_EQ(s.at(0, 1).ch, U'B');
    EXPECT_FALSE(s.alt_active());
}

TEST(AltScreen, DecscPerScreen) {
    term::Parser p;
    term::Screen s(8, 3);
    feed(p, s, "abc");
    feed(p, s, "\x1b" "7");              // DECSC: save (0, 3) on primary
    feed(p, s, "\x1b[?1049h");
    // Alt: DECSC saves a different position
    feed(p, s, "Z\x1b" "7");             // alt cursor at (0, 1), saved
    feed(p, s, "\x1b[5G");               // move cursor away
    feed(p, s, "\x1b" "8");              // DECRC -> (0, 1)
    EXPECT_EQ(s.cursor_col(), 1);
    feed(p, s, "\x1b[?1049l");
    // Back on primary; DECRC should restore to (0, 3)
    feed(p, s, "\x1b[5G");
    feed(p, s, "\x1b" "8");
    EXPECT_EQ(s.cursor_col(), 3);
}
