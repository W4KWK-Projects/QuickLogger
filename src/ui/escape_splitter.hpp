#pragma once

#include <string>
#include <vector>

#include <ftxui/component/event.hpp>

namespace ql
{

    // Undoes FTXUI's merging of Esc with whatever key arrives right after it.
    //
    // A terminal sends Esc as the same byte that starts every other key's
    // escape sequence (F4 is "Esc O S", Up is "Esc [ A"), so when a second
    // key arrives within about 50 ms of Esc -- a quick double-tap, or two
    // keys delivered in one packet over a laggy SSH link -- FTXUI reads the
    // bytes as a single "Alt + key" event. QuickLogger has no Alt keys, so
    // that event did nothing: both keys were lost, and when the second was
    // an F-key or arrow, its last byte was typed into the focused field as a
    // stray letter ("Esc F4" typed an "S").
    //
    // Feed every event through here and act on what comes back instead:
    // such a merged event comes back as Esc followed by the key(s) that were
    // really pressed. When the second key's sequence was cut short (its
    // remaining bytes arrive as ordinary characters), those characters are
    // held and joined to it rather than typed.
    //
    // THE NO-ALT-KEYS RULE: this treats every Esc-prefixed "Alt + key" as
    // Esc followed by that key. QuickLogger must never bind an Alt (Meta)
    // key combination; one would be split into Esc plus a keypress here.
    class EscapeSplitter
    {
    public:
        // The events `event` really stands for, in order: usually just
        // `event` itself; several for a merged Esc; none while the bytes of
        // a cut-short sequence are being collected.
        std::vector<ftxui::Event> Feed(const ftxui::Event& event);

    private:
        // The start of a key sequence whose remaining bytes haven't arrived
        // yet, e.g. "\x1b[" or "\x1bO". Empty when nothing is being held.
        std::string pending_;
    };

}  // namespace ql
