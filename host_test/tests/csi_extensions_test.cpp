#include "term_core/parser.hpp"
#include "term_core/screen.hpp"

#include <gtest/gtest.h>

namespace {

void feed(term::Parser& p, term::Screen& s, std::string_view str) {
    p.feed(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(str.data()), str.size()), s);
}

}  // namespace

TEST(DECSet, CursorVisibilityOnByDefault) {
    term::Screen s(10, 3);
    EXPECT_TRUE(s.cursor_visible());
}

TEST(DECSet, Mode25Toggles) {
    term::Parser p;
    term::Screen s(10, 3);
    feed(p, s, "\x1b[?25l");
    EXPECT_FALSE(s.cursor_visible());
    feed(p, s, "\x1b[?25h");
    EXPECT_TRUE(s.cursor_visible());
}

TEST(DECSet, Mode7DisablesAutowrap) {
    term::Parser p;
    term::Screen s(4, 2);
    feed(p, s, "\x1b[?7l");
    EXPECT_FALSE(s.autowrap());
    // Without autowrap, writing past the last col should not move to row 1.
    feed(p, s, "ABCDE");  // 5 chars on a 4-wide screen
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_EQ(s.at(0, 1).ch, U'B');
    EXPECT_EQ(s.at(0, 2).ch, U'C');
    // 'D' written at last column; 'E' overwrites the same cell.
    EXPECT_EQ(s.at(0, 3).ch, U'E');
    EXPECT_EQ(s.cursor_row(), 0);
}

TEST(DECSet, Mode7PassThroughOtherParams) {
    term::Parser p;
    term::Screen s(10, 3);
    // Multi-mode set: ?7l;?25l should both apply.
    feed(p, s, "\x1b[?7;25l");
    EXPECT_FALSE(s.autowrap());
    EXPECT_FALSE(s.cursor_visible());
}

TEST(InsertChars, BasicInsert) {
    term::Parser p;
    term::Screen s(8, 1);
    feed(p, s, "ABCDE");
    feed(p, s, "\x1b[2G");        // cursor to col 2 (1-based -> col index 1)
    feed(p, s, "\x1b[2@");        // insert 2 blanks
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_EQ(s.at(0, 1).ch, U' ');
    EXPECT_EQ(s.at(0, 2).ch, U' ');
    EXPECT_EQ(s.at(0, 3).ch, U'B');
    EXPECT_EQ(s.at(0, 4).ch, U'C');
}

TEST(DeleteChars, BasicDelete) {
    term::Parser p;
    term::Screen s(8, 1);
    feed(p, s, "ABCDE");
    feed(p, s, "\x1b[2G");        // cursor to col index 1
    feed(p, s, "\x1b[2P");        // delete 2 chars
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_EQ(s.at(0, 1).ch, U'D');
    EXPECT_EQ(s.at(0, 2).ch, U'E');
    EXPECT_EQ(s.at(0, 3).ch, U' ');
}

TEST(EraseChars, FillsWithSpacesNoCursorMove) {
    term::Parser p;
    term::Screen s(8, 1);
    feed(p, s, "ABCDE");
    feed(p, s, "\x1b[2G");        // cursor at col index 1
    feed(p, s, "\x1b[3X");        // erase 3 chars
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_EQ(s.at(0, 1).ch, U' ');
    EXPECT_EQ(s.at(0, 2).ch, U' ');
    EXPECT_EQ(s.at(0, 3).ch, U' ');
    EXPECT_EQ(s.at(0, 4).ch, U'E');
    EXPECT_EQ(s.cursor_col(), 1);
}

TEST(InsertLines, ShiftsLinesDown) {
    term::Parser p;
    term::Screen s(4, 4);
    feed(p, s, "AAA\r\nBBB\r\nCCC\r\nDDD");
    feed(p, s, "\x1b[2;1H");      // cursor to row 2, col 1 (index 1, 0)
    feed(p, s, "\x1b[1L");        // insert 1 line at row index 1
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_EQ(s.at(1, 0).ch, U' ');  // newly inserted blank
    EXPECT_EQ(s.at(2, 0).ch, U'B');  // shifted down
    EXPECT_EQ(s.at(3, 0).ch, U'C');
    // 'DDD' fell off the bottom
}

TEST(DeleteLines, ShiftsLinesUp) {
    term::Parser p;
    term::Screen s(4, 4);
    feed(p, s, "AAA\r\nBBB\r\nCCC\r\nDDD");
    feed(p, s, "\x1b[2;1H");      // cursor to row index 1
    feed(p, s, "\x1b[1M");        // delete 1 line
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_EQ(s.at(1, 0).ch, U'C');  // shifted up
    EXPECT_EQ(s.at(2, 0).ch, U'D');
    EXPECT_EQ(s.at(3, 0).ch, U' ');  // blank at bottom
}

TEST(InsertLines, RespectsScrollRegion) {
    term::Parser p;
    term::Screen s(4, 5);
    feed(p, s, "AAA\r\nBBB\r\nCCC\r\nDDD\r\nEEE");
    feed(p, s, "\x1b[2;4r");      // scroll region rows 2..4 (index 1..3)
    feed(p, s, "\x1b[2;1H");      // cursor to row index 1
    feed(p, s, "\x1b[1L");        // insert 1 line within region
    EXPECT_EQ(s.at(0, 0).ch, U'A');  // outside region — untouched
    EXPECT_EQ(s.at(1, 0).ch, U' ');  // new blank
    EXPECT_EQ(s.at(2, 0).ch, U'B');  // shifted within region
    EXPECT_EQ(s.at(3, 0).ch, U'C');
    EXPECT_EQ(s.at(4, 0).ch, U'E');  // outside region — untouched ('DDD' fell off region)
}
