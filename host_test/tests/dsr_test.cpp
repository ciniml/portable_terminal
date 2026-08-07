#include "term_core/parser.hpp"
#include "term_core/screen.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

void feed(term::Parser& p, term::Screen& s, std::string_view str) {
    p.feed(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(str.data()), str.size()), s);
}

// Screen with the response sink captured into a string.
struct DsrFixture {
    term::Parser p;
    term::Screen s;
    std::string out;

    DsrFixture(uint16_t cols, uint16_t rows) : s(cols, rows) {
        s.set_response_sink([this](std::span<const uint8_t> bytes) {
            out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        });
    }
};

}  // namespace

TEST(Dsr, OperatingStatusReportsOk) {
    DsrFixture f(10, 3);
    feed(f.p, f.s, "\x1b[5n");
    EXPECT_EQ(f.out, "\x1b[0n");
}

TEST(Dsr, CursorPositionHome) {
    DsrFixture f(10, 3);
    feed(f.p, f.s, "\x1b[6n");
    EXPECT_EQ(f.out, "\x1b[1;1R");
}

TEST(Dsr, CursorPositionAfterMove) {
    DsrFixture f(80, 24);
    feed(f.p, f.s, "\x1b[5;12H\x1b[6n");
    EXPECT_EQ(f.out, "\x1b[5;12R");
}

TEST(Dsr, CursorPositionAfterPrint) {
    DsrFixture f(80, 24);
    feed(f.p, f.s, "abc\x1b[6n");
    EXPECT_EQ(f.out, "\x1b[1;4R");
}

TEST(Dsr, DecPrivateVariantGetsQuestionMark) {
    DsrFixture f(80, 24);
    feed(f.p, f.s, "\x1b[3;3H\x1b[?6n");
    EXPECT_EQ(f.out, "\x1b[?3;3R");
}

TEST(Dsr, NoSinkIsSilentlyDropped) {
    term::Parser p;
    term::Screen s(10, 3);
    feed(p, s, "\x1b[6n");  // must not crash
    EXPECT_EQ(s.cursor_row(), 0);
}

TEST(Dsr, UnknownPsIgnored) {
    DsrFixture f(10, 3);
    feed(f.p, f.s, "\x1b[99n");
    EXPECT_TRUE(f.out.empty());
}
