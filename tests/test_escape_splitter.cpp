// Esc merged with the key right after it (see EscapeSplitter).

#include <string>
#include <vector>

#include <ftxui/component/event.hpp>

#include "../src/ui/escape_splitter.hpp"
#include "test_framework.hpp"

namespace ql
{

    // Feeds `inputs` (as FTXUI would deliver them) and collects what comes out.
    static std::vector<ftxui::Event> FeedAll(EscapeSplitter* splitter,
                                             const std::vector<ftxui::Event>& inputs)
    {
        std::vector<ftxui::Event> out;
        for (const ftxui::Event& input : inputs)
        {
            for (const ftxui::Event& event : splitter->Feed(input))
            {
                out.push_back(event);
            }
        }
        return out;
    }

    QL_TEST(OrdinaryKeysPassStraightThrough)
    {
        EscapeSplitter splitter;
        std::vector<ftxui::Event> out =
            FeedAll(&splitter, {ftxui::Event::Escape, ftxui::Event::F4, ftxui::Event::ArrowUp,
                                ftxui::Event::Character('a'), ftxui::Event::F5});
        REQUIRE(out.size() == 5);
        CHECK(out[0] == ftxui::Event::Escape);
        CHECK(out[1] == ftxui::Event::F4);
        CHECK(out[2] == ftxui::Event::ArrowUp);
        CHECK(out[3] == ftxui::Event::Character('a'));
        CHECK(out[4] == ftxui::Event::F5);
    }

    QL_TEST(TwoQuickEscapesAreTwoEscapes)
    {
        EscapeSplitter splitter;
        std::vector<ftxui::Event> out = FeedAll(&splitter, {ftxui::Event::Special("\x1B\x1B")});
        REQUIRE(out.size() == 2);
        CHECK(out[0] == ftxui::Event::Escape);
        CHECK(out[1] == ftxui::Event::Escape);
        out = FeedAll(&splitter, {ftxui::Event::Special("\x1B\x1B\x1B")});
        CHECK_EQ(out.size(), std::size_t{3});
    }

    QL_TEST(EscapeThenAnFKeyTypesNothingStray)
    {
        // What FTXUI makes of "Esc" + F4 ("Esc O S") arriving together.
        EscapeSplitter splitter;
        std::vector<ftxui::Event> out =
            FeedAll(&splitter, {ftxui::Event::Special("\x1B\x1BO"), ftxui::Event::Character('S')});
        REQUIRE(out.size() == 2);
        CHECK(out[0] == ftxui::Event::Escape);
        CHECK(out[1] == ftxui::Event::F4);

        // "Esc" + F5 ("Esc [ 1 5 ~").
        out = FeedAll(&splitter, {ftxui::Event::Special("\x1B\x1B["), ftxui::Event::Character('1'),
                                  ftxui::Event::Custom, ftxui::Event::Character('5'),
                                  ftxui::Event::Character('~')});
        REQUIRE(out.size() == 3);
        CHECK(out[0] == ftxui::Event::Escape);
        CHECK(out[1] == ftxui::Event::Custom);  // A redraw in between still gets through.
        CHECK(out[2] == ftxui::Event::F5);

        // "Esc" + Up in the terminal's application cursor mode ("Esc O A").
        out =
            FeedAll(&splitter, {ftxui::Event::Special("\x1B\x1BO"), ftxui::Event::Character('A')});
        REQUIRE(out.size() == 2);
        CHECK(out[1] == ftxui::Event::ArrowUp);
    }

    QL_TEST(EscapeThenLettersAreEscapeAndTheLetters)
    {
        EscapeSplitter splitter;
        std::vector<ftxui::Event> out = FeedAll(&splitter, {ftxui::Event::Special("\x1Bk")});
        REQUIRE(out.size() == 2);
        CHECK(out[0] == ftxui::Event::Escape);
        CHECK(out[1] == ftxui::Event::Character('k'));

        out = FeedAll(&splitter, {ftxui::Event::Special("\x1B"
                                                        "ab")});
        REQUIRE(out.size() == 3);
        CHECK(out[1] == ftxui::Event::Character('a'));
        CHECK(out[2] == ftxui::Event::Character('b'));

        // "Esc" then Shift+O is not the start of an F-key.
        out = FeedAll(&splitter, {ftxui::Event::Special("\x1BO")});
        REQUIRE(out.size() == 2);
        CHECK(out[1] == ftxui::Event::Character('O'));

        out = FeedAll(&splitter, {ftxui::Event::Special("\x1B\x1Bq")});
        REQUIRE(out.size() == 3);
        CHECK(out[1] == ftxui::Event::Escape);
        CHECK(out[2] == ftxui::Event::Character('q'));
    }

    QL_TEST(AnUnfinishedSequenceIsDroppedByTheNextKey)
    {
        EscapeSplitter splitter;
        std::vector<ftxui::Event> out =
            FeedAll(&splitter, {ftxui::Event::Special("\x1B\x1B["), ftxui::Event::F2});
        REQUIRE(out.size() == 2);
        CHECK(out[0] == ftxui::Event::Escape);
        CHECK(out[1] == ftxui::Event::F2);
        // And nothing is left waiting.
        out = FeedAll(&splitter, {ftxui::Event::Character('x')});
        REQUIRE(out.size() == 1);
        CHECK(out[0] == ftxui::Event::Character('x'));
    }

}  // namespace ql
