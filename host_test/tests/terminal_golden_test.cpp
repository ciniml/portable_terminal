#include "term_core/terminal.hpp"

#include <gtest/gtest.h>

#include "ascii_renderer.hpp"
#include "fake_display.hpp"

TEST(Terminal, FixedBootStringSnapshot) {
    term::testing::FakeDisplay display(40, 4);
    term::Terminal t(40, 4, display);

    constexpr const char* kBoot =
        "\x1b[2J\x1b[H"
        "\x1b[1;32mTab5 Terminal Phase 1\x1b[0m\r\n"
        "\x1b[33mhello, world\x1b[0m\r\n";

    ASSERT_TRUE(t.feed(std::string_view(kBoot)));
    ASSERT_TRUE(t.render_dirty());

    EXPECT_GT(display.draw_calls(), 0);
    EXPECT_EQ(display.flush_calls(), 1);

    std::string dump = term::testing::ascii_dump(t.screen());
    const std::string expected =
        "Tab5 Terminal Phase 1                   \n"
        "hello, world                            \n"
        "                                        \n"
        "                                        \n";
    EXPECT_EQ(dump, expected);

    // Verify SGR carried into cells.
    EXPECT_TRUE(t.screen().at(0, 0).attrs.bold);
    EXPECT_EQ(t.screen().at(0, 0).fg.kind, term::Color::Kind::Index);
    EXPECT_EQ(t.screen().at(0, 0).fg.value, 2u);  // green
    EXPECT_FALSE(t.screen().at(1, 0).attrs.bold);
    EXPECT_EQ(t.screen().at(1, 0).fg.value, 3u);  // yellow
}

TEST(Terminal, RenderDirtyIsIdempotentWhenClean) {
    term::testing::FakeDisplay display(10, 2);
    term::Terminal t(10, 2, display);

    ASSERT_TRUE(t.feed(std::string_view("hi")));
    ASSERT_TRUE(t.render_dirty());
    int draws = display.draw_calls();
    int flushes = display.flush_calls();

    ASSERT_TRUE(t.render_dirty());
    EXPECT_EQ(display.draw_calls(), draws);
    EXPECT_EQ(display.flush_calls(), flushes);
}
