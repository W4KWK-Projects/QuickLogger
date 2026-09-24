#include "chrome.hpp"

#include <cstdint>
#include <ctime>
#include <exception>
#include <string>
#include <utility>

#include <ftxui/screen/terminal.hpp>

#include "../date_utils.hpp"
#include "../uls_import.hpp"

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
                rows.push_back(std::move(current_row));
                current_row.clear();
                current_width = 0;
            }
            current_row.push_back(hint);
            current_width += width;
        }
        if (!current_row.empty())
        {
            rows.push_back(std::move(current_row));
        }
        return rows;
    }

    ftxui::Element Heading(const std::string& text)
    {
        return ftxui::text(text) | ftxui::bold | ftxui::color(kColorHeading);
    }

    ftxui::Element ColumnHeader(const std::string& text)
    {
        return ftxui::text(text) | ftxui::color(kColorHeading);
    }

    ftxui::Element HintText(const std::string& text)
    {
        return ftxui::text(text) | ftxui::color(kColorHint);
    }

    ftxui::Element HintParagraph(const std::string& text)
    {
        return ftxui::paragraph(text) | ftxui::color(kColorHint);
    }

    ftxui::Element Separator()
    {
        return ftxui::separator() | ftxui::color(kColorFrame);
    }

    ftxui::Element Framed(ftxui::Element content)
    {
        return std::move(content) | ftxui::border | ftxui::color(kColorFrame);
    }

    ftxui::Element KeyHintRow(const std::vector<KeyHint>& hints)
    {
        ftxui::Elements pieces;
        for (const KeyHint& hint : hints)
        {
            pieces.push_back(ftxui::text(" " + hint.key + " ") |
                             ftxui::bgcolor(ftxui::Color::YellowLight) |
                             ftxui::color(ftxui::Color::Black));
            pieces.push_back(ftxui::text(" " + hint.label + "  "));
        }
        return ftxui::hbox(pieces);
    }

    static Database* g_notice_db = nullptr;

    void SetTopBarNoticeDatabase(Database* db)
    {
        g_notice_db = db;
    }

    // The station-data notice as a colored badge, or an empty element.
    static ftxui::Element StationDataNotice()
    {
        if (g_notice_db == nullptr)
        {
            return ftxui::text("");
        }
        bool is_problem = false;
        std::string notice;
        try
        {
            notice = DescribeStationDataNotice(
                g_notice_db, static_cast<std::int64_t>(std::time(nullptr)), &is_problem);
        }
        catch (const std::exception&)
        {
            return ftxui::text("");
        }
        if (notice.empty())
        {
            return ftxui::text("");
        }
        ftxui::Element badge = ftxui::text(" " + notice + " ");
        if (is_problem)
        {
            return ftxui::hbox({badge | ftxui::bgcolor(ftxui::Color::Red) |
                                    ftxui::color(ftxui::Color::White) | ftxui::bold,
                                ftxui::text(" ")});
        }
        return ftxui::hbox(
            {badge | ftxui::bgcolor(ftxui::Color::YellowLight) | ftxui::color(ftxui::Color::Black),
             ftxui::text(" ")});
    }

    ftxui::Element TopBar(const std::string& page_title)
    {
        return ftxui::hbox({
                   ftxui::text(" QuickLogger ") | ftxui::bold | ftxui::color(kColorLabel),
                   ftxui::text("— ") | ftxui::color(kColorHeading),
                   ftxui::text(page_title + " ") | ftxui::bold | ftxui::color(kColorHeading),
                   ftxui::filler(),
                   StationDataNotice(),
                   // Local time, to the minute. It's computed each time the bar is
                   // drawn; ScreenTicker (interactive_session.cpp) is what makes a
                   // redraw happen when the minute changes.
                   ftxui::text(FormatLocalTimeOfDay(std::time(nullptr)) + " ") |
                       ftxui::color(kColorData),
               }) |
               ftxui::bgcolor(ftxui::Color::Blue);
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
                            ftxui::bgcolor(ftxui::Color::Cyan) | ftxui::color(ftxui::Color::Black));
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
