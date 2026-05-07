#include "term_core/parser.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

namespace {

struct CsiCall {
    char final_byte;
    std::vector<int> params;
    std::vector<uint8_t> intermediates;
    bool private_marker;
};

struct EscCall {
    char final_byte;
    std::vector<uint8_t> intermediates;
};

class RecordingSink final : public term::IParserSink {
public:
    void put_char(char32_t ch) override { chars.push_back(ch); }
    void execute(uint8_t c0) override { c0_bytes.push_back(c0); }
    void csi(char final_byte, std::span<const int> params,
             std::span<const uint8_t> intermediates,
             bool private_marker) override {
        csi_calls.push_back({final_byte,
                             {params.begin(), params.end()},
                             {intermediates.begin(), intermediates.end()},
                             private_marker});
    }
    void esc(char final_byte, std::span<const uint8_t> intermediates) override {
        esc_calls.push_back({final_byte,
                             {intermediates.begin(), intermediates.end()}});
    }
    void osc(std::span<const uint8_t> data) override {
        osc_calls.emplace_back(data.begin(), data.end());
    }

    std::vector<char32_t> chars;
    std::vector<uint8_t> c0_bytes;
    std::vector<CsiCall> csi_calls;
    std::vector<EscCall> esc_calls;
    std::vector<std::vector<uint8_t>> osc_calls;
};

void feed(term::Parser& p, RecordingSink& sink, std::string_view s) {
    p.feed(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size()), sink);
}

}  // namespace

TEST(Parser, GroundPrintable) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "abc");
    ASSERT_EQ(s.chars.size(), 3u);
    EXPECT_EQ(s.chars[0], U'a');
    EXPECT_EQ(s.chars[1], U'b');
    EXPECT_EQ(s.chars[2], U'c');
}

TEST(Parser, C0Execute) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\r\n\t\b");
    ASSERT_EQ(s.c0_bytes.size(), 4u);
    EXPECT_EQ(s.c0_bytes[0], 0x0D);
    EXPECT_EQ(s.c0_bytes[1], 0x0A);
    EXPECT_EQ(s.c0_bytes[2], 0x09);
    EXPECT_EQ(s.c0_bytes[3], 0x08);
}

TEST(Parser, CsiNoParams) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\x1b[H");
    ASSERT_EQ(s.csi_calls.size(), 1u);
    EXPECT_EQ(s.csi_calls[0].final_byte, 'H');
    EXPECT_TRUE(s.csi_calls[0].params.empty());
    EXPECT_FALSE(s.csi_calls[0].private_marker);
}

TEST(Parser, CsiWithParams) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\x1b[12;34H");
    ASSERT_EQ(s.csi_calls.size(), 1u);
    EXPECT_EQ(s.csi_calls[0].final_byte, 'H');
    ASSERT_EQ(s.csi_calls[0].params.size(), 2u);
    EXPECT_EQ(s.csi_calls[0].params[0], 12);
    EXPECT_EQ(s.csi_calls[0].params[1], 34);
}

TEST(Parser, CsiOmittedParam) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\x1b[;5H");
    ASSERT_EQ(s.csi_calls.size(), 1u);
    ASSERT_EQ(s.csi_calls[0].params.size(), 2u);
    EXPECT_EQ(s.csi_calls[0].params[0], 0);
    EXPECT_EQ(s.csi_calls[0].params[1], 5);
}

TEST(Parser, CsiPrivateMarker) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\x1b[?25h");
    ASSERT_EQ(s.csi_calls.size(), 1u);
    EXPECT_TRUE(s.csi_calls[0].private_marker);
    EXPECT_EQ(s.csi_calls[0].final_byte, 'h');
    ASSERT_EQ(s.csi_calls[0].params.size(), 1u);
    EXPECT_EQ(s.csi_calls[0].params[0], 25);
}

TEST(Parser, SgrChain) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\x1b[1;31;48;5;200m");
    ASSERT_EQ(s.csi_calls.size(), 1u);
    EXPECT_EQ(s.csi_calls[0].final_byte, 'm');
    ASSERT_EQ(s.csi_calls[0].params.size(), 5u);
    EXPECT_EQ(s.csi_calls[0].params[0], 1);
    EXPECT_EQ(s.csi_calls[0].params[1], 31);
    EXPECT_EQ(s.csi_calls[0].params[2], 48);
    EXPECT_EQ(s.csi_calls[0].params[3], 5);
    EXPECT_EQ(s.csi_calls[0].params[4], 200);
}

TEST(Parser, EscDispatch) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\x1b" "7\x1b" "8");
    ASSERT_EQ(s.esc_calls.size(), 2u);
    EXPECT_EQ(s.esc_calls[0].final_byte, '7');
    EXPECT_EQ(s.esc_calls[1].final_byte, '8');
}

TEST(Parser, Utf8TwoByte) {
    term::Parser p;
    RecordingSink s;
    // U+00E9 (é) = 0xC3 0xA9
    feed(p, s, "\xC3\xA9");
    ASSERT_EQ(s.chars.size(), 1u);
    EXPECT_EQ(s.chars[0], U'é');
}

TEST(Parser, Utf8ThreeByte) {
    term::Parser p;
    RecordingSink s;
    // U+3042 (あ) = 0xE3 0x81 0x82
    feed(p, s, "\xE3\x81\x82");
    ASSERT_EQ(s.chars.size(), 1u);
    EXPECT_EQ(s.chars[0], U'あ');
}

TEST(Parser, OscBelTerminated) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\x1b]0;hello\x07");
    ASSERT_EQ(s.osc_calls.size(), 1u);
    std::string got(s.osc_calls[0].begin(), s.osc_calls[0].end());
    EXPECT_EQ(got, "0;hello");
}

TEST(Parser, ResetClearsPartialEscape) {
    term::Parser p;
    RecordingSink s;
    feed(p, s, "\x1b[12;");
    p.reset();
    feed(p, s, "abc");
    ASSERT_EQ(s.chars.size(), 3u);
    EXPECT_TRUE(s.csi_calls.empty());
}
