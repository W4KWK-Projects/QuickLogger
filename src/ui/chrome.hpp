#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace ql
{

    class Database;

    // ---- The app's color palette ------------------------------------------
    //
    // Every color on screen comes from here, by role, so a given kind of
    // thing looks the same on every page. Light (bright) colors are used
    // for anything worth reading; plain white is avoided, since a real color
    // costs nothing more and carries meaning.
    //
    //   Top bar           blue background; app name light yellow, page title
    //                     light cyan, clock light green
    //   Key bar (bottom)  cyan background, black labels, keys as black on
    //                     light yellow badges -- a different color from the
    //                     top bar, so the two never blur together. The keys
    //                     aren't bold: many terminals draw bold black as
    //                     gray, which read poorly on the yellow.
    //   Headings          light cyan, bold         (Heading)
    //   Column headers    light cyan               (ColumnHeader)
    //   Field labels      light yellow             (FieldLabel in pages.cpp)
    //   Data              light green -- what's typed into fields, and the
    //                     operator's callsign / net name / clock up top
    //   List rows         white -- the long lists (check-ins, nets,
    //                     matches, history) would be a wall of green
    //                     otherwise; the colored header and frame around
    //                     them carry the color
    //   Hints / help      cyan                     (HintText, HintParagraph)
    //   Success messages  light green; errors bright red (256-color)
    //   Frames, lines     light blue               (Framed, Separator)
    //   Row numbers       bright red -- the numbers beside a list's rows while
    //                     picking one to edit or delete by number
    //   Dialog borders    bright blue (256-color) on every dialog, brighter
    //                     than the page frames, so it reads as raised above
    //                     the page behind it
    //   Dialog titles     light cyan (Heading) on every dialog except the
    //                     ones that delete something, which are bright red
    //                     (256-color) throughout, title and text alike
    constexpr ftxui::Color::Palette16 kColorHeading = ftxui::Color::CyanLight;
    constexpr ftxui::Color::Palette16 kColorLabel = ftxui::Color::YellowLight;
    constexpr ftxui::Color::Palette16 kColorData = ftxui::Color::GreenLight;
    constexpr ftxui::Color::Palette16 kColorListRow = ftxui::Color::White;
    constexpr ftxui::Color::Palette16 kColorHint = ftxui::Color::Cyan;
    constexpr ftxui::Color::Palette16 kColorSuccess = ftxui::Color::GreenLight;
    constexpr ftxui::Color::Palette16 kColorFrame = ftxui::Color::BlueLight;
    // A 256-color blue, brighter than the 16-color light blue the page
    // frames use (which many terminal themes draw as a mid blue), so a
    // dialog's edge stands out from the page under it. Terminals limited to
    // 16 colors get the nearest one instead.
    constexpr ftxui::Color::Palette256 kColorDialogBorder = ftxui::Color::DodgerBlue1;
    // A 256-color pure red for errors, pick-mode row numbers and dialogs
    // that delete something; the 16-color light red reads as a dark red in
    // many terminal themes.
    constexpr ftxui::Color::Palette256 kColorDanger = ftxui::Color::Red1;
    constexpr ftxui::Color::Palette256 kColorError = kColorDanger;
    constexpr ftxui::Color::Palette256 kColorPickNumber = kColorDanger;

    // A section heading ("Recurring Nets", "Saved Stations:").
    ftxui::Element Heading(const std::string& text);

    // The header line above a column-aligned list. Not bold: in some
    // terminals bold glyphs are wider, which would push the header out of
    // line with the rows below it.
    ftxui::Element ColumnHeader(const std::string& text);

    // A line of help or explanation ("Enter picks a station..."), or an
    // empty-list message; HintParagraph wraps to the available width.
    ftxui::Element HintText(const std::string& text);
    ftxui::Element HintParagraph(const std::string& text);

    // A horizontal rule between sections.
    ftxui::Element Separator();

    // A horizontal rule inside a dialog, in the dialog border's color.
    ftxui::Element DialogSeparator();

    // `content` inside a light-blue border.
    ftxui::Element Framed(ftxui::Element content);

    // Framed, for a box inside a dialog: its border in the dialog border's
    // color.
    ftxui::Element DialogFramed(ftxui::Element content);

    // A single keyboard shortcut shown in a page's or modal's key legend, e.g.
    // {"F2", "Save"}. Since mouse use can't be assumed, every action a page or
    // modal offers should have an entry somewhere on screen. Clicking one
    // presses its key (see mouse.hpp).
    struct KeyHint
    {
        std::string key;
        std::string label;
    };

    // Renders `hints` as a row of "key badge + label" pairs in the bottom
    // bar's colors (yellow badges, black labels on cyan), filling the width
    // it's given -- so a modal's key row looks just like the page's bottom
    // bar rather than taking on the modal's own colors. BottomBar is built
    // from these.
    ftxui::Element KeyHintRow(const std::vector<KeyHint>& hints);

    // Where TopBar reads the station-data notice from (see
    // DescribeStationDataNotice in uls_import.hpp). Set once per session,
    // before the first frame; until then, or with nullptr, no notice shows.
    // A process-wide setting because every page's TopBar needs it and each
    // process runs exactly one session.
    void SetTopBarNoticeDatabase(Database* db);

    // A full-width colored title bar for the top of a page, naming the app and
    // the page currently shown, with the local time (to the minute) at the
    // right end, the F1 Help key just left of it -- it's on every page, so it
    // lives here rather than taking room on each key bar -- and, while the
    // shared station data is loading or missing, a short notice saying so,
    // so people know lookups aren't fully working yet whatever page they're
    // on. A page can add a short `status` of its own (the active net's
    // check-in count), shown left of all that. The title is shortened if
    // need be so none of it is pushed off the edge.
    ftxui::Element TopBar(const std::string& page_title, const std::string& status = "");

    // A full-width colored legend bar for the bottom of a page. Packs as many
    // hints as fit on one line given the real client terminal width
    // (ftxui::Terminal::Size(), not the page's own layout box), overflowing
    // onto additional lines only when the client's screen is too narrow to
    // fit them all -- a two-line bar is a fallback for a narrow terminal, not
    // a fixed choice made ahead of time.
    ftxui::Element BottomBar(const std::vector<KeyHint>& hints);

    // `hints` followed by as many of `extras`, in order, as fit on this
    // terminal without the key bar needing more lines than it would anyway,
    // or than `lines` if that's more (placed before a closing Esc, which
    // stays last). For a page's seldom-used keys (see InfoWindow in
    // app_state.hpp), which work whether shown or not.
    std::vector<KeyHint> AddExtraKeysThatFit(const std::vector<KeyHint>& hints,
                                             const std::vector<KeyHint>& extras, int lines);

    // Same as BottomBar, but `rows` forces each inner vector onto its own
    // line regardless of width -- the primitive BottomBar is built on.
    // Prefer BottomBar unless a page genuinely needs a specific, fixed row
    // grouping.
    //
    // Deliberately a differently *named* function rather than an overload of
    // BottomBar: a braced list of key hints like {{"F2", "Save"}, {"Esc",
    // "Cancel"}} is also a valid std::vector<std::vector<KeyHint>> under
    // libstdc++ (each {"F2", "Save"} converts through vector's
    // iterator-pair constructor, the two const char* acting as iterators),
    // so overloading on the two vector types makes every such call
    // ambiguous on Linux/GCC even though libc++ (macOS/FreeBSD) resolves it.
    ftxui::Element BottomBarRows(const std::vector<std::vector<KeyHint>>& rows);

    // Wraps `content` between a TopBar/BottomBar for `page_title`/`hints`
    // (and the top bar's `top_status`), giving `content` the full remaining
    // vertical space in between so it reaches the edges of the screen.
    ftxui::Element PageChrome(const std::string& page_title, ftxui::Element content,
                              const std::vector<KeyHint>& hints,
                              const std::string& top_status = "");

    // Same as PageChrome, but with a multi-row bottom bar -- see
    // BottomBarRows above (also for why this isn't an overload).
    ftxui::Element PageChromeRows(const std::string& page_title, ftxui::Element content,
                                  const std::vector<std::vector<KeyHint>>& hint_rows);

}  // namespace ql
