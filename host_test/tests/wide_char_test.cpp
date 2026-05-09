#include "term_core/east_asian_width.hpp"
#include "term_core/parser.hpp"
#include "term_core/screen.hpp"

#include <gtest/gtest.h>

namespace {

void feed(term::Parser& p, term::Screen& s, std::string_view str) {
    p.feed(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(str.data()), str.size()), s);
}

}  // namespace

TEST(EastAsianWidth, AsciiIsNarrow) {
    EXPECT_FALSE(term::is_wide_char(U'A'));
    EXPECT_FALSE(term::is_wide_char(U'~'));
    EXPECT_FALSE(term::is_wide_char(U' '));
}

TEST(EastAsianWidth, CommonCjk) {
    EXPECT_TRUE(term::is_wide_char(U'あ'));      // Hiragana
    EXPECT_TRUE(term::is_wide_char(U'カ'));      // Katakana
    EXPECT_TRUE(term::is_wide_char(U'漢'));      // Han
    EXPECT_TRUE(term::is_wide_char(U'한'));      // Hangul Syllable
}

TEST(EastAsianWidth, Latin1IsNarrow) {
    EXPECT_FALSE(term::is_wide_char(U'é'));
    EXPECT_FALSE(term::is_wide_char(U'ñ'));
}

TEST(Screen, WideCharOccupiesTwoCells) {
    term::Parser p;
    term::Screen s(10, 1);
    feed(p, s, "\xE3\x81\x82");  // U+3042 あ
    EXPECT_EQ(s.at(0, 0).ch, U'あ');
    EXPECT_TRUE(s.at(0, 0).attrs.wide);
    EXPECT_FALSE(s.at(0, 0).attrs.wide_cont);
    EXPECT_TRUE(s.at(0, 1).attrs.wide_cont);
    EXPECT_EQ(s.cursor_col(), 2);
}

TEST(Screen, WideCharFollowedByAscii) {
    term::Parser p;
    term::Screen s(10, 1);
    feed(p, s, "A\xE3\x81\x82" "B");  // A あ B
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_FALSE(s.at(0, 0).attrs.wide);
    EXPECT_EQ(s.at(0, 1).ch, U'あ');
    EXPECT_TRUE(s.at(0, 1).attrs.wide);
    EXPECT_TRUE(s.at(0, 2).attrs.wide_cont);
    EXPECT_EQ(s.at(0, 3).ch, U'B');
    EXPECT_FALSE(s.at(0, 3).attrs.wide);
}

TEST(Screen, WideCharWrapsToNextLine) {
    term::Parser p;
    term::Screen s(4, 2);
    // "AAA" fills cols 0..2, leaving col 3 free. A wide char doesn't fit
    // (would need col 3 + col 4) so it wraps to row 1.
    feed(p, s, "AAA\xE3\x81\x82");
    EXPECT_EQ(s.at(0, 0).ch, U'A');
    EXPECT_EQ(s.at(0, 2).ch, U'A');
    EXPECT_EQ(s.at(1, 0).ch, U'あ');
    EXPECT_TRUE(s.at(1, 0).attrs.wide);
    EXPECT_TRUE(s.at(1, 1).attrs.wide_cont);
}

TEST(Screen, OverwritingWideHeadClearsContinuation) {
    term::Parser p;
    term::Screen s(10, 1);
    feed(p, s, "\xE3\x81\x82");          // あ at col 0..1
    feed(p, s, "\x1b[1G");                // cursor to col 0 (CSI G is 1-based)
    feed(p, s, "X");                      // overwrite head
    EXPECT_EQ(s.at(0, 0).ch, U'X');
    EXPECT_FALSE(s.at(0, 0).attrs.wide);
    EXPECT_FALSE(s.at(0, 1).attrs.wide_cont);
    EXPECT_EQ(s.at(0, 1).ch, U' ');
}

TEST(Screen, OverwritingWideContClearsHead) {
    term::Parser p;
    term::Screen s(10, 1);
    feed(p, s, "\xE3\x81\x82");          // あ at col 0..1
    feed(p, s, "\x1b[2G");                // cursor to col 1
    feed(p, s, "X");                      // overwrite continuation
    EXPECT_FALSE(s.at(0, 0).attrs.wide);
    EXPECT_EQ(s.at(0, 0).ch, U' ');
    EXPECT_EQ(s.at(0, 1).ch, U'X');
    EXPECT_FALSE(s.at(0, 1).attrs.wide_cont);
}
