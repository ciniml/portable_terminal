#include "term_core/screen.hpp"

#include <gtest/gtest.h>

#include "ascii_renderer.hpp"
#include "term_core/parser.hpp"

namespace {

void feed(term::Parser& p, term::Screen& s, std::string_view str) {
    p.feed(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(str.data()), str.size()), s);
}

}  // namespace

TEST(Screen, BlankAfterConstruction) {
    term::Screen s(10, 3);
    EXPECT_EQ(s.cursor_row(), 0);
    EXPECT_EQ(s.cursor_col(), 0);
    for (uint16_t r = 0; r < 3; ++r)
        for (uint16_t c = 0; c < 10; ++c) {
            EXPECT_EQ(s.at(r, c).ch, U' ');
        }
}

TEST(Screen, PutCharAdvancesCursor) {
    term::Parser p;
    term::Screen s(10, 2);
    feed(p, s, "abc");
    EXPECT_EQ(s.at(0, 0).ch, U'a');
    EXPECT_EQ(s.at(0, 1).ch, U'b');
    EXPECT_EQ(s.at(0, 2).ch, U'c');
    EXPECT_EQ(s.cursor_row(), 0);
    EXPECT_EQ(s.cursor_col(), 3);
}

TEST(Screen, CarriageReturnAndLineFeed) {
    term::Parser p;
    term::Screen s(10, 3);
    feed(p, s, "ab\r\ncd");
    EXPECT_EQ(s.at(0, 0).ch, U'a');
    EXPECT_EQ(s.at(0, 1).ch, U'b');
    EXPECT_EQ(s.at(1, 0).ch, U'c');
    EXPECT_EQ(s.at(1, 1).ch, U'd');
    EXPECT_EQ(s.cursor_row(), 1);
    EXPECT_EQ(s.cursor_col(), 2);
}

TEST(Screen, CursorPositionAbsolute) {
    term::Parser p;
    term::Screen s(20, 5);
    feed(p, s, "\x1b[3;10HX");
    EXPECT_EQ(s.at(2, 9).ch, U'X');
    EXPECT_EQ(s.cursor_row(), 2);
    EXPECT_EQ(s.cursor_col(), 10);
}

TEST(Screen, EraseDisplayAll) {
    term::Parser p;
    term::Screen s(5, 3);
    feed(p, s, "abc\r\ndef");
    feed(p, s, "\x1b[2J");
    for (uint16_t r = 0; r < 3; ++r)
        for (uint16_t c = 0; c < 5; ++c)
            EXPECT_EQ(s.at(r, c).ch, U' ');
}

TEST(Screen, EraseLineToEnd) {
    term::Parser p;
    term::Screen s(8, 1);
    feed(p, s, "abcdefgh");
    feed(p, s, "\x1b[3G\x1b[K");
    EXPECT_EQ(s.at(0, 0).ch, U'a');
    EXPECT_EQ(s.at(0, 1).ch, U'b');
    for (uint16_t c = 2; c < 8; ++c) EXPECT_EQ(s.at(0, c).ch, U' ');
}

TEST(Screen, SgrSetsForeground) {
    term::Parser p;
    term::Screen s(5, 1);
    feed(p, s, "\x1b[31mA");
    auto fg = s.at(0, 0).fg;
    EXPECT_EQ(fg.kind, term::Color::Kind::Index);
    EXPECT_EQ(fg.value, 1u);  // red index
}

TEST(Screen, SgrResetClearsAttrs) {
    term::Parser p;
    term::Screen s(5, 1);
    feed(p, s, "\x1b[1;31mA\x1b[0mB");
    EXPECT_TRUE(s.at(0, 0).attrs.bold);
    EXPECT_FALSE(s.at(0, 1).attrs.bold);
    EXPECT_EQ(s.at(0, 1).fg.kind, term::Color::Kind::Default);
}

TEST(Screen, ScrollOnLineFeedAtBottom) {
    term::Parser p;
    term::Screen s(3, 2);
    feed(p, s, "ab\r\ncd\r\nef");
    // Bottom row "ef" should have caused a scroll-up; row 0 = "cd", row 1 = "ef"
    EXPECT_EQ(s.at(0, 0).ch, U'c');
    EXPECT_EQ(s.at(0, 1).ch, U'd');
    EXPECT_EQ(s.at(1, 0).ch, U'e');
    EXPECT_EQ(s.at(1, 1).ch, U'f');
}

TEST(Screen, DECSTBMScrollRegion) {
    term::Parser p;
    term::Screen s(4, 5);
    // Set region rows 2-4 (1-based: 2;4 -> rows index 1..3 inclusive)
    feed(p, s, "\x1b[2;4r");
    // After DECSTBM cursor at (0,0). Move into the region.
    feed(p, s, "\x1b[2;1HAA\r\nBB\r\nCC\r\nDD");
    // Row 0 untouched (outside region), region scrolled.
    EXPECT_EQ(s.at(0, 0).ch, U' ');
    // Region final state: region holds last 3 inputs after scrolling.
    EXPECT_EQ(s.at(1, 0).ch, U'B');
    EXPECT_EQ(s.at(2, 0).ch, U'C');
    EXPECT_EQ(s.at(3, 0).ch, U'D');
}

TEST(Screen, SaveRestoreCursor) {
    term::Parser p;
    term::Screen s(10, 5);
    feed(p, s, "\x1b[3;5H\x1b" "7\x1b[1;1H\x1b" "8X");
    EXPECT_EQ(s.at(2, 4).ch, U'X');
}

TEST(Screen, DamageTracksWrites) {
    term::Parser p;
    term::Screen s(10, 3);
    (void)s.take_damage();  // clear initial
    feed(p, s, "\x1b[2;4HZ");
    auto d = s.take_damage();
    EXPECT_EQ(d.row0, 1);
    EXPECT_EQ(d.col0, 3);
    EXPECT_EQ(d.row1, 2);
    EXPECT_EQ(d.col1, 4);
    EXPECT_FALSE(s.dirty());
}
