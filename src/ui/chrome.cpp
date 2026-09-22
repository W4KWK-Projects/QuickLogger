#include "chrome.hpp"

namespace ql
{

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
               }) |
               ftxui::bgcolor(ftxui::Color::Blue) | ftxui::color(ftxui::Color::White);
    }

    ftxui::Element BottomBar(const std::vector<KeyHint>& hints)
    {
        return ftxui::hbox({
                   KeyHintRow(hints),
                   ftxui::filler(),
               }) |
               ftxui::bgcolor(ftxui::Color::Blue) | ftxui::color(ftxui::Color::White);
    }

    ftxui::Element PageChrome(const std::string& page_title, ftxui::Element content,
                              const std::vector<KeyHint>& hints)
    {
        return ftxui::vbox({
            TopBar(page_title),
            content | ftxui::flex,
            BottomBar(hints),
        });
    }

}  // namespace ql
