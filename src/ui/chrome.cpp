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
#include "../update_check.hpp"
#include "../version.hpp"
#include "list_columns.hpp"
#include "mouse.hpp"

namespace ql
{

    // Matches KeyHintRow's own rendering exactly: " "+key+" " is
    // key.size()+2 columns, " "+label+"  " is label.size()+3.
    static int KeyHintWidth(const KeyHint& hint)
    {
        return static_cast<int>(hint.key.size() + hint.label.size()) + 5;
    }

    // Where each bar row starts when hints of these `widths` are wrapped:
    // each row is filled up to `max_width` before the next is started, so a
    // wide terminal gets as much of row one as possible and a narrow one
    // only spills onto extra rows as far as it has to. Works on widths alone,
    // so wrapping copies no hints.
    static std::size_t CountKeyHintRows(const int* widths, std::size_t count, int max_width)
    {
        std::size_t rows = 0;
        int row_width = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (rows == 0 || (row_width > 0 && row_width + widths[i] > max_width))
            {
                ++rows;
                row_width = 0;
            }
            row_width += widths[i];
        }
        return rows;
    }

    // One bar row's element: `hints[first]` up to (not including) `end`.
    static ftxui::Element KeyHintRowOf(const std::vector<KeyHint>& hints, std::size_t first,
                                       std::size_t end);

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

    int KeyHintRowWidth(const std::vector<KeyHint>& hints)
    {
        int width = 0;
        for (const KeyHint& hint : hints)
        {
            width += KeyHintWidth(hint);
        }
        return width;
    }

    static ftxui::Element KeyHintRowOf(const std::vector<KeyHint>& hints, std::size_t first,
                                       std::size_t end)
    {
        ftxui::Elements pieces;
        pieces.reserve(end - first + 1);
        for (std::size_t i = first; i < end; ++i)
        {
            const KeyHint& hint = hints[i];
            // " F2 " and " Save  ", each made in one allocation.
            std::string key_text;
            key_text.reserve(hint.key.size() + 2);
            key_text += ' ';
            key_text += hint.key;
            key_text += ' ';
            std::string label_text;
            label_text.reserve(hint.label.size() + 3);
            label_text += ' ';
            label_text += hint.label;
            label_text += "  ";
            // Clicking the key or its label presses the key (see mouse.hpp).
            pieces.push_back(
                ftxui::hbox({
                    ftxui::text(std::move(key_text)) | ftxui::bgcolor(ftxui::Color::YellowLight) |
                        ftxui::color(ftxui::Color::Black),
                    ftxui::text(std::move(label_text)),
                }) |
                ClickTarget(hint.key));
        }
        pieces.push_back(ftxui::filler());
        return ftxui::hbox(std::move(pieces)) | ftxui::bgcolor(ftxui::Color::Cyan) |
               ftxui::color(ftxui::Color::Black);
    }

    ftxui::Element KeyHintRow(const std::vector<KeyHint>& hints)
    {
        return KeyHintRowOf(hints, 0, hints.size());
    }

    static Database* g_notice_db = nullptr;

    void SetTopBarNoticeDatabase(Database* db)
    {
        g_notice_db = db;
    }

    // The station-data notice's text (empty if there's none), and whether
    // it's a problem rather than just news. Read from the database at most
    // once a second, not on every frame: it changes only every few seconds
    // (ScreenTicker redraws when it does), and a read takes a lock.
    static const std::string& StationDataNoticeText(bool* is_problem, std::int64_t now)
    {
        static std::int64_t read_at = -1;
        static std::string text;
        static bool problem = false;
        if (now != read_at)
        {
            read_at = now;
            problem = false;
            text.clear();
            if (g_notice_db != nullptr)
            {
                try
                {
                    text = DescribeStationDataNotice(g_notice_db, now, &problem);
                }
                catch (const std::exception&)
                {
                    text.clear();
                    problem = false;
                }
            }
        }
        *is_problem = problem;
        return text;
    }

    // The newer release found, read at most once a second, not on every
    // frame: it changes every few hours at most, and a read takes a lock
    // and a copy.
    static const std::string& CachedUpdate(std::int64_t now)
    {
        static std::int64_t read_at = -1;
        static std::string version;
        if (now != read_at)
        {
            read_at = now;
            version = AvailableUpdate();
        }
        return version;
    }

    const std::string& AvailableUpdateForDisplay()
    {
        return CachedUpdate(static_cast<std::int64_t>(std::time(nullptr)));
    }

    // The update notice's text ("v1.8.0 available"), or "" if there's none;
    // remade only when the version found changes.
    // `*short_text` is the same notice shortened ("New v1.8.0"), for when
    // the full one would cut the page title.
    static const std::string& UpdateNoticeText(std::int64_t now, const std::string** short_text)
    {
        static std::string shown_version;
        static std::string text;
        static std::string short_form;
        const std::string& version = CachedUpdate(now);
        if (version != shown_version)
        {
            shown_version = version;
            text.clear();
            short_form.clear();
            if (!version.empty())
            {
                text.reserve(version.size() + 11);
                text.push_back('v');
                text.append(version);
                text.append(" available");
                short_form.reserve(version.size() + 5);
                short_form.append("New v");
                short_form.append(version);
            }
        }
        *short_text = &short_form;
        return text;
    }

    // The clock's text, "03:42 PM " or "15:42 ": remade only when the minute
    // (or the 12/24-hour setting) changes, not on every frame.
    static const std::string& ClockText(std::int64_t now)
    {
        static std::int64_t shown_minute = -1;
        static bool shown_24_hour = false;
        static std::string text;
        std::int64_t minute = now / 60;
        if (minute != shown_minute || Use24HourClock() != shown_24_hour)
        {
            shown_minute = minute;
            shown_24_hour = Use24HourClock();
            text = FormatLocalTimeOfDay(now);
            text += ' ';
        }
        return text;
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
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        bool is_problem = false;
        const std::string& notice = StationDataNoticeText(&is_problem, now);
        // A newer release, found at the console (see update_check.hpp).
        const std::string* update_short = nullptr;
        const std::string* update = &UpdateNoticeText(now, &update_short);
        // Local time, to the minute. ScreenTicker (interactive_session.cpp)
        // is what makes a redraw happen when the minute changes.
        const std::string& clock = ClockText(now);
        static const std::string version = std::string("v") + QuickLoggerVersion() + " ";

        // Shorten the title rather than push the right-hand side (status,
        // notice, F1 Help, clock) off the edge. Columns, not bytes: "— " is
        // two columns.
        static const std::string help_key = " F1 ";
        static const std::string help_label = " Help  ";
        static const std::string status_gap = "   ";
        int left = 12 + static_cast<int>(version.size()) + 2;
        int right = (notice.empty() ? 0 : TextWidth(notice) + 3) +
                    (update->empty() ? 0 : TextWidth(*update) + 3) +
                    (status.empty() ? 0 : TextWidth(status) + static_cast<int>(status_gap.size())) +
                    static_cast<int>(help_key.size() + help_label.size() + clock.size());
        // A trailing space after the title, and a wider gap before a status.
        int room = FrameTerminalSize().dimx - left - right - (status.empty() ? 1 : 3);
        // The update notice in short, rather than cut the title.
        if (!update->empty() && room < TextWidth(page_title))
        {
            room += TextWidth(*update) - TextWidth(*update_short);
            update = update_short;
        }
        std::string title = page_title;
        if (room < TextWidth(title))
        {
            title = room > 3 ? CutToWidth(title, room - 3) + "..."
                             : CutToWidth(title, std::max(room, 0));
        }

        return ftxui::hbox({
                   ftxui::text("QuickLogger ") | ftxui::color(kColorData),
                   ftxui::text(version) | ftxui::color(kColorLabel),
                   ftxui::text("— ") | ftxui::color(kColorHeading),
                   ftxui::text(title + " ") | ftxui::color(kColorHeading),
                   ftxui::filler(),
                   status.empty() ? ftxui::text("")
                                  : ftxui::text(status + status_gap) | ftxui::color(kColorData),
                   NoticeBadge(*update, false) | ClickTargetEvent(OpenUpdatePageEvent()),
                   NoticeBadge(notice, is_problem),
                   ftxui::hbox({
                       ftxui::text(help_key) | ftxui::bgcolor(ftxui::Color::YellowLight) |
                           ftxui::color(ftxui::Color::Black),
                       ftxui::text(help_label) | ftxui::color(kColorLabel),
                   }) | ClickTarget("F1"),
                   ftxui::text(clock) | ftxui::color(kColorData),
               }) |
               ftxui::bgcolor(ftxui::Color::Blue);
    }

    static ftxui::Dimensions g_frame_terminal_size{0, 0};

    void SetFrameTerminalSize(const ftxui::Dimensions& size)
    {
        g_frame_terminal_size = size;
    }

    ftxui::Dimensions FrameTerminalSize()
    {
        if (g_frame_terminal_size.dimx > 0)
        {
            return g_frame_terminal_size;
        }
        return ftxui::Terminal::Size();
    }

    ftxui::Element BottomBar(const std::vector<KeyHint>& hints)
    {
        int max_width = FrameTerminalSize().dimx;
        // Wrapped as CountKeyHintRows does, each row built straight from
        // `hints` without copying them into rows first.
        ftxui::Elements lines;
        std::size_t row_start = 0;
        int row_width = 0;
        for (std::size_t i = 0; i < hints.size(); ++i)
        {
            int width = KeyHintWidth(hints[i]);
            if (row_width > 0 && row_width + width > max_width)
            {
                lines.push_back(KeyHintRowOf(hints, row_start, i));
                row_start = i;
                row_width = 0;
            }
            row_width += width;
        }
        if (row_start < hints.size())
        {
            lines.push_back(KeyHintRowOf(hints, row_start, hints.size()));
        }
        return ftxui::vbox(std::move(lines));
    }

    std::vector<KeyHint> AddExtraKeysThatFit(const std::vector<KeyHint>& hints,
                                             const std::vector<KeyHint>& extras, int lines)
    {
        int max_width = FrameTerminalSize().dimx;
        // Extras go before a closing Esc, which stays last.
        bool has_closing = !hints.empty() && hints.back().key == "Esc";
        std::size_t kept = has_closing ? hints.size() - 1 : hints.size();

        // Tried by width alone: the hints' widths, then each extra's in turn
        // (with the closing Esc's after it), counting the rows they'd need.
        std::vector<int> widths;
        widths.reserve(hints.size() + extras.size());
        for (std::size_t i = 0; i < kept; ++i)
        {
            widths.push_back(KeyHintWidth(hints[i]));
        }
        int closing_width = has_closing ? KeyHintWidth(hints.back()) : 0;
        widths.push_back(closing_width);
        std::size_t with_closing = has_closing ? widths.size() : widths.size() - 1;
        std::size_t most_lines = std::max(CountKeyHintRows(widths.data(), with_closing, max_width),
                                          static_cast<std::size_t>(lines));
        std::size_t extras_that_fit = 0;
        for (const KeyHint& extra : extras)
        {
            // The closing Esc's slot takes the extra, and the Esc moves after it.
            widths.back() = KeyHintWidth(extra);
            widths.push_back(closing_width);
            std::size_t count = has_closing ? widths.size() : widths.size() - 1;
            if (CountKeyHintRows(widths.data(), count, max_width) > most_lines)
            {
                break;
            }
            ++extras_that_fit;
        }

        std::vector<KeyHint> all;
        all.reserve(hints.size() + extras_that_fit);
        all.insert(all.end(), hints.begin(), hints.begin() + static_cast<std::ptrdiff_t>(kept));
        all.insert(all.end(), extras.begin(),
                   extras.begin() + static_cast<std::ptrdiff_t>(extras_that_fit));
        if (has_closing)
        {
            all.push_back(hints.back());
        }
        return all;
    }

    ftxui::Element BottomBarRows(const std::vector<std::vector<KeyHint>>& rows)
    {
        ftxui::Elements lines;
        lines.reserve(rows.size());
        for (const std::vector<KeyHint>& row : rows)
        {
            lines.push_back(KeyHintRow(row));
        }
        return ftxui::vbox(std::move(lines));
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
