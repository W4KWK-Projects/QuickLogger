#include "chrome.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <string>
#include <utility>

#include <ftxui/screen/terminal.hpp>

#include "../date_utils.hpp"
#include "../uls_import.hpp"
#include "../version.hpp"

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

    ftxui::Element DialogSeparator()
    {
        return ftxui::separator() | ftxui::color(kColorDialogBorder);
    }

    ftxui::Element Framed(ftxui::Element content)
    {
        return std::move(content) | ftxui::border | ftxui::color(kColorFrame);
    }

    ftxui::Element DialogFramed(ftxui::Element content)
    {
        return std::move(content) | ftxui::color(kColorFrame) |
               ftxui::borderStyled(kColorDialogBorder);
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
        pieces.push_back(ftxui::filler());
        return ftxui::hbox(pieces) | ftxui::bgcolor(ftxui::Color::Cyan) |
               ftxui::color(ftxui::Color::Black);
    }

    static Database* g_notice_db = nullptr;

    void SetTopBarNoticeDatabase(Database* db)
    {
        g_notice_db = db;
    }

    // The station-data notice's text (empty if there's none), and whether
    // it's a problem rather than just news.
    static std::string StationDataNoticeText(bool* is_problem)
    {
        *is_problem = false;
        if (g_notice_db == nullptr)
        {
            return "";
        }
        try
        {
            return DescribeStationDataNotice(
                g_notice_db, static_cast<std::int64_t>(std::time(nullptr)), is_problem);
        }
        catch (const std::exception&)
        {
            return "";
        }
    }

    // The station-data notice as a colored badge, or an empty element.
    static ftxui::Element NoticeBadge(const std::string& notice, bool is_problem)
    {
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

    ftxui::Element TopBar(const std::string& page_title, const std::string& status)
    {
        bool is_problem = false;
        std::string notice = StationDataNoticeText(&is_problem);
        // Local time, to the minute. It's computed each time the bar is
        // drawn; ScreenTicker (interactive_session.cpp) is what makes a
        // redraw happen when the minute changes.
        std::string clock = FormatLocalTimeOfDay(std::time(nullptr)) + " ";
        std::string version = std::string("v") + QuickLoggerVersion() + " ";

        // Shorten the title rather than push the right-hand side (status,
        // notice, F1 Help, clock) off the edge. Columns, not bytes: "— " is
        // two columns.
        const std::string help_key = " F1 ";
        const std::string help_label = " Help  ";
        const std::string status_gap = "   ";
        int left = 12 + static_cast<int>(version.size()) + 2;
        int right = (notice.empty() ? 0 : static_cast<int>(notice.size()) + 3) +
                    (status.empty() ? 0 : static_cast<int>(status.size() + status_gap.size())) +
                    static_cast<int>(help_key.size() + help_label.size() + clock.size());
        // A trailing space after the title, and a wider gap before a status.
        int room = ftxui::Terminal::Size().dimx - left - right - (status.empty() ? 1 : 3);
        std::string title = page_title;
        if (room < static_cast<int>(title.size()))
        {
            title = room > 3 ? title.substr(0, static_cast<std::size_t>(room - 3)) + "..."
                             : title.substr(0, static_cast<std::size_t>(std::max(room, 0)));
        }

        return ftxui::hbox({
                   ftxui::text("QuickLogger ") | ftxui::bold | ftxui::color(kColorLabel),
                   ftxui::text(version) | ftxui::color(kColorLabel),
                   ftxui::text("— ") | ftxui::color(kColorHeading),
                   ftxui::text(title + " ") | ftxui::bold | ftxui::color(kColorHeading),
                   ftxui::filler(),
                   status.empty() ? ftxui::text("")
                                  : ftxui::text(status + status_gap) | ftxui::color(kColorData),
                   NoticeBadge(notice, is_problem),
                   ftxui::text(help_key) | ftxui::bgcolor(ftxui::Color::YellowLight) |
                       ftxui::color(ftxui::Color::Black),
                   ftxui::text(help_label) | ftxui::color(kColorLabel),
                   ftxui::text(clock) | ftxui::color(kColorData),
               }) |
               ftxui::bgcolor(ftxui::Color::Blue);
    }

    ftxui::Element BottomBar(const std::vector<KeyHint>& hints)
    {
        int width = ftxui::Terminal::Size().dimx;
        return BottomBarRows(WrapKeyHints(hints, width));
    }

    std::vector<KeyHint> AddExtraKeysThatFit(const std::vector<KeyHint>& hints,
                                             const std::vector<KeyHint>& extras, int lines)
    {
        int width = ftxui::Terminal::Size().dimx;
        std::size_t most_lines =
            std::max(WrapKeyHints(hints, width).size(), static_cast<std::size_t>(lines));
        // Extras go before a closing Esc, which stays last.
        std::vector<KeyHint> all = hints;
        std::vector<KeyHint> closing;
        if (!all.empty() && all.back().key == "Esc")
        {
            closing.push_back(all.back());
            all.pop_back();
        }
        for (const KeyHint& extra : extras)
        {
            std::vector<KeyHint> candidate = all;
            candidate.push_back(extra);
            candidate.insert(candidate.end(), closing.begin(), closing.end());
            if (WrapKeyHints(candidate, width).size() > most_lines)
            {
                break;
            }
            all.push_back(extra);
        }
        all.insert(all.end(), closing.begin(), closing.end());
        return all;
    }

    ftxui::Element BottomBarRows(const std::vector<std::vector<KeyHint>>& rows)
    {
        ftxui::Elements lines;
        for (const std::vector<KeyHint>& row : rows)
        {
            lines.push_back(KeyHintRow(row));
        }
        return ftxui::vbox(lines);
    }

    ftxui::Element PageChrome(const std::string& page_title, ftxui::Element content,
                              const std::vector<KeyHint>& hints, const std::string& top_status)
    {
        return ftxui::vbox({
            TopBar(page_title, top_status),
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
