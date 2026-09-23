#include "chrome.hpp"

#include <ctime>
#include <utility>

#include <ftxui/screen/terminal.hpp>

#include "../date_utils.hpp"

namespace ql
{

    // Matches KeyHintRow's own rendering exactly: " "+key+" " is
    // key.size()+2 columns, " "+label+"  " is label.size()+3.
    static int KeyHintWidth(const KeyHint& hint)
    {
        return static_cast<int>(hint.key.size() + hint.label.size()) + 5;
    }

    // Greedily fills each row up to `max_width` before starting a new
    // one, so a wide terminal gets as much of row one as possible and a
    // narrow one only spills onto extra rows as far as it has to.
    static std::vector<std::vector<KeyHint>> WrapKeyHints(const std::vector<KeyHint>& hints,
                                                          int max_width)
    {
        std::vector<std::vector<KeyHint>> rows;
        std::vector<KeyHint> current_row;
        int current_width = 0;
        for (const KeyHint& hint : hints)
        {
            int width = KeyHintWidth(hint);
            if (!current_row.empty() && current_width + width > max_width)
            {
                rows.push_back(current_row);
                current_row.clear();
                current_width = 0;
            }
            current_row.push_back(hint);
            current_width += width;
        }
        if (!current_row.empty())
        {
            rows.push_back(current_row);
        }
        return rows;
    }

    ftxui::Element KeyHintRow(const std::vector<KeyHint>& hints)
    {
        ftxui::Elements pieces;
        for (const KeyHint& hint : hints)
        {
            pieces.push_back(ftxui::text(" " + hint.key + " ") | ftxui::bold |
                             ftxui::bgcolor(ftxui::Color::YellowLight) |
                             ftxui::color(ftxui::Color::Black));
            pieces.push_back(ftxui::text(" " + hint.label + "  "));
        }
        return ftxui::hbox(pieces);
    }

    ftxui::Element TopBar(const std::string& page_title)
    {
        return ftxui::hbox({
                   ftxui::text(" QuickLogger ") | ftxui::bold,
                   ftxui::text("— " + page_title + " "),
                   ftxui::filler(),
                   // Local time, to the minute. It's computed each time the bar is
                   // drawn; ClockTicker (interactive_session.cpp) is what makes a
                   // redraw happen when the minute changes.
                   ftxui::text(FormatLocalTimeOfDay(std::time(nullptr)) + " "),
               }) |
               ftxui::bgcolor(ftxui::Color::Blue) | ftxui::color(ftxui::Color::White);
    }

    ftxui::Element BottomBar(const std::vector<KeyHint>& hints)
    {
        int width = ftxui::Terminal::Size().dimx;
        return BottomBarRows(WrapKeyHints(hints, width));
    }

    ftxui::Element BottomBarRows(const std::vector<std::vector<KeyHint>>& rows)
    {
        ftxui::Elements lines;
        for (const std::vector<KeyHint>& row : rows)
        {
            lines.push_back(ftxui::hbox({
                                KeyHintRow(row),
                                ftxui::filler(),
                            }) |
                            ftxui::bgcolor(ftxui::Color::Blue) | ftxui::color(ftxui::Color::White));
        }
        return ftxui::vbox(lines);
    }

    ftxui::Element PageChrome(const std::string& page_title, ftxui::Element content,
                              const std::vector<KeyHint>& hints)
    {
        return ftxui::vbox({
            TopBar(page_title),
            std::move(content) | ftxui::flex,
            BottomBar(hints),
        });
    }

    ftxui::Element PageChromeRows(const std::string& page_title, ftxui::Element content,
                                  const std::vector<std::vector<KeyHint>>& hint_rows)
    {
        return ftxui::vbox({
            TopBar(page_title),
            std::move(content) | ftxui::flex,
            BottomBarRows(hint_rows),
        });
    }

}  // namespace ql
