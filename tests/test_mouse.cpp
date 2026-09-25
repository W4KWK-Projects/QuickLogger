// Clicking keys in a key bar, and double-clicking list rows (see mouse.hpp).

#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "../src/ui/chrome.hpp"
#include "../src/ui/mouse.hpp"
#include "test_framework.hpp"

namespace ql
{

    static ftxui::Mouse Click(int x, int y, ftxui::Mouse::Motion motion = ftxui::Mouse::Pressed)
    {
        ftxui::Mouse mouse;
        mouse.button = ftxui::Mouse::Left;
        mouse.motion = motion;
        mouse.x = x;
        mouse.y = y;
        return mouse;
    }

    // Draws `element` on a one-line screen, as a frame would.
    static void Draw(ftxui::Element element)
    {
        ftxui::Screen screen(80, 1);
        ftxui::Render(screen, element);
    }

    QL_TEST(ClickingAKeyInTheKeyBarPressesIt)
    {
        BeginClickTargets();
        // " F2 " + " Save  " is 0-10, " Up/Down " + " Move  " 11-26, then Esc.
        Draw(KeyHintRow({{"F2/Enter", "Save"}, {"Up/Down", "Move"}, {"Esc", "Cancel"}}));
        ftxui::Event key;
        REQUIRE(ClickedKey(Click(1, 0), &key));
        CHECK(key == ftxui::Event::F2);
        REQUIRE(ClickedKey(Click(18, 0), &key) == false);  // Not a single key.
        REQUIRE(ClickedKey(Click(36, 0), &key));           // Its label.
        CHECK(key == ftxui::Event::Escape);
        CHECK(!ClickedKey(Click(70, 0), &key));
        // Only a press counts, so a release doesn't press it again.
        CHECK(!ClickedKey(Click(1, 0, ftxui::Mouse::Released), &key));

        // With a window up, the page's keys underneath can't be clicked.
        BeginClickLayer();
        Draw(ftxui::hbox({ftxui::text("   "), KeyHintRow({{"F4", "View"}})}));
        CHECK(!ClickedKey(Click(1, 0), &key));
        REQUIRE(ClickedKey(Click(4, 0), &key));
        CHECK(key == ftxui::Event::F4);

        // The next frame starts over.
        BeginClickTargets();
        CHECK(!ClickedKey(Click(4, 0), &key));
    }

    QL_TEST(ClickingANumberedRowPicksIt)
    {
        BeginClickTargets();
        ftxui::Screen screen(20, 3);
        ftxui::Render(screen, ftxui::vbox({ftxui::text("1 one") | ClickableRow(0),
                                           ftxui::text("2 two") | ClickableRow(1)}));
        int row = -1;
        bool double_click = true;
        ftxui::Event key;
        CHECK(!ClickedKey(Click(1, 1), &key));  // A row isn't a key.
        REQUIRE(ClickedRow(Click(1, 1), &row, &double_click));
        CHECK_EQ(row, 1);
        CHECK(!double_click);
        REQUIRE(ClickedRow(Click(3, 1), &row, &double_click));
        CHECK(double_click);
        // A third click starts over.
        REQUIRE(ClickedRow(Click(3, 1), &row, &double_click));
        CHECK(!double_click);
        REQUIRE(ClickedRow(Click(1, 0), &row, &double_click));
        CHECK_EQ(row, 0);
        CHECK(!double_click);
        CHECK(!ClickedRow(Click(1, 2), &row, &double_click));
    }

    QL_TEST(ClickingARowHighlightsIt)
    {
        std::vector<std::string> rows = {"one", "two", "three"};
        int selected = 0;
        ftxui::Component list =
            ftxui::Make<DoubleClickToEnter>(ftxui::Menu(&rows, &selected), nullptr);
        ftxui::Screen screen(20, 3);
        ftxui::Render(screen, list->Render());
        CHECK(list->OnEvent(ftxui::Event::Mouse("", Click(1, 2))) == false);
        CHECK(list->OnEvent(ftxui::Event::Mouse("", Click(1, 2, ftxui::Mouse::Released))));
        CHECK_EQ(selected, 2);
        // A second click, a double-click, would press Enter; with no screen
        // to send it to, it just keeps the highlight.
        CHECK(list->OnEvent(ftxui::Event::Mouse("", Click(1, 2, ftxui::Mouse::Released))));
        CHECK_EQ(selected, 2);
    }

}  // namespace ql
