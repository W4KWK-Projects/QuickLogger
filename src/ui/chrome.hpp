#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace ql
{

    // A single keyboard shortcut shown in a page's or modal's key legend, e.g.
    // {"F2", "Save"}. Since mouse use can't be assumed, every action a page or
    // modal offers should have an entry somewhere on screen.
    struct KeyHint
    {
        std::string key;
        std::string label;
    };

    // Renders `hints` as a row of colored "key badge + label" pairs. Doesn't
    // fill the available width -- meant for embedding inside a bordered modal
    // box, alongside PageChrome's full-width BottomBar for whole pages.
    ftxui::Element KeyHintRow(const std::vector<KeyHint>& hints);

    // A full-width colored title bar for the top of a page, naming the app and
    // the page currently shown.
    ftxui::Element TopBar(const std::string& page_title);

    // A full-width colored legend bar for the bottom of a page. Packs as many
    // hints as fit on one line given the real client terminal width
    // (ftxui::Terminal::Size(), not the page's own layout box), overflowing
    // onto additional lines only when the client's screen is too narrow to
    // fit them all -- a two-line bar is a fallback for a narrow terminal, not
    // a fixed choice made ahead of time.
    ftxui::Element BottomBar(const std::vector<KeyHint>& hints);

    // Same as BottomBar, but `rows` forces each inner vector onto its own
    // line regardless of width -- the primitive the width-aware overload
    // above is built on. Prefer the single-vector overload unless a page
    // genuinely needs a specific, fixed row grouping.
    ftxui::Element BottomBar(const std::vector<std::vector<KeyHint>>& rows);

    // Wraps `content` between a TopBar/BottomBar for `page_title`/`hints`,
    // giving `content` the full remaining vertical space in between so it
    // reaches the edges of the screen.
    ftxui::Element PageChrome(const std::string& page_title, ftxui::Element content,
                              const std::vector<KeyHint>& hints);

    // Same as PageChrome, but with a multi-row bottom bar -- see the
    // BottomBar overload above.
    ftxui::Element PageChrome(const std::string& page_title, ftxui::Element content,
                              const std::vector<std::vector<KeyHint>>& hint_rows);

}  // namespace ql
