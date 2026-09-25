#include "escape_splitter.hpp"

#include <cstddef>

namespace ql
{

    static const char kEscapeByte = '\x1B';

    // Longest key sequence worth waiting for; anything longer isn't a key.
    static const std::size_t kMaxSequenceLength = 8;

    // A key's alternative spellings and the one FTXUI uses internally.
    struct KeySpelling
    {
        const char* from;
        const char* to;
    };

    // FTXUI rewrites these alternative spellings as it reads them (see
    // g_uniformize in its terminal_input_parser.cpp); a sequence put back
    // together here gets the same treatment, so it still matches
    // ftxui::Event::ArrowUp, Event::F1 and so on.
    static const KeySpelling kKeySpellings[] = {
        // Arrows, Home and End in the terminal's "application" cursor mode.
        {"\x1BOA", "\x1B[A"},
        {"\x1BOB", "\x1B[B"},
        {"\x1BOC", "\x1B[C"},
        {"\x1BOD", "\x1B[D"},
        {"\x1BOH", "\x1B[H"},
        {"\x1BOF", "\x1B[F"},
        // The Linux console's F1-F5.
        {"\x1B[[A", "\x1BOP"},
        {"\x1B[[B", "\x1BOQ"},
        {"\x1B[[C", "\x1BOR"},
        {"\x1B[[D", "\x1BOS"},
        {"\x1B[[E", "\x1B[15~"},
        // xterm-r5/r6 and rxvt F1-F4.
        {"\x1B[11~", "\x1BOP"},
        {"\x1B[12~", "\x1BOQ"},
        {"\x1B[13~", "\x1BOR"},
        {"\x1B[14~", "\x1BOS"},
        // vt100 F5-F10.
        {"\x1BOt", "\x1B[15~"},
        {"\x1BOu", "\x1B[17~"},
        {"\x1BOv", "\x1B[18~"},
        {"\x1BOl", "\x1B[19~"},
        {"\x1BOw", "\x1B[20~"},
        {"\x1BOx", "\x1B[21~"},
    };

    static std::string UniformSpelling(const std::string& sequence)
    {
        for (const KeySpelling& spelling : kKeySpellings)
        {
            if (sequence == spelling.from)
            {
                return spelling.to;
            }
        }
        return sequence;
    }

    // True once `sequence` -- which starts "Esc [" or "Esc O" -- holds a
    // whole key: one byte after "Esc O"; after "Esc [", parameters then a
    // final byte from '@' to '~' (the Linux console's F1-F5 put a second '['
    // in front of theirs).
    static bool IsWholeSequence(const std::string& sequence)
    {
        if (sequence.size() < 3)
        {
            return false;
        }
        if (sequence[1] == 'O')
        {
            return true;
        }
        if (sequence == "\x1B[[")
        {
            return false;
        }
        unsigned char last = static_cast<unsigned char>(sequence.back());
        return last >= 0x40 && last <= 0x7E;
    }

    // `text` as typed characters: one event per ASCII byte, or the whole of
    // it as one character if it isn't plain ASCII (a single UTF-8 letter).
    static void AppendCharacters(const std::string& text, std::vector<ftxui::Event>* events)
    {
        bool ascii = true;
        for (char c : text)
        {
            if (static_cast<unsigned char>(c) >= 0x80)
            {
                ascii = false;
            }
        }
        if (!ascii)
        {
            events->push_back(ftxui::Event::Character(text));
            return;
        }
        for (char c : text)
        {
            events->push_back(ftxui::Event::Character(c));
        }
    }

    std::vector<ftxui::Event> EscapeSplitter::Feed(const ftxui::Event& event)
    {
        std::vector<ftxui::Event> events;

        if (!pending_.empty())
        {
            if (event.is_character())
            {
                pending_ += event.input();
                if (IsWholeSequence(pending_))
                {
                    events.push_back(ftxui::Event::Special(UniformSpelling(pending_)));
                    pending_.clear();
                }
                else if (pending_.size() > kMaxSequenceLength)
                {
                    pending_.clear();  // Not a key after all.
                }
                return events;
            }
            if (event == ftxui::Event::Custom)
            {
                // A redraw request, not a key: keep collecting.
                events.push_back(event);
                return events;
            }
            pending_.clear();
        }

        // Only FTXUI's "Alt + key" events -- Esc and one or two more bytes
        // that aren't the start of a longer sequence -- need splitting.
        const std::string& input = event.input();
        bool merged = !event.is_character() && input.size() >= 2 && input[0] == kEscapeByte &&
                      input[1] != '[' && input[1] != 'P' && input[1] != ']';
        // "Esc O" and one more byte is a whole key in itself (F1-F4).
        if (!merged || (input.size() == 3 && input[1] == 'O'))
        {
            events.push_back(event);
            return events;
        }

        events.push_back(ftxui::Event::Escape);
        std::string rest = input.substr(1);
        if (rest[0] != kEscapeByte)
        {
            AppendCharacters(rest, &events);
        }
        else if (rest.size() == 1)
        {
            events.push_back(ftxui::Event::Escape);
        }
        else if (rest[1] == '[' || rest[1] == 'O')
        {
            // The start of another key (an F-key, an arrow...), cut short:
            // its remaining bytes arrive next, as characters.
            pending_ = rest;
        }
        else
        {
            events.push_back(ftxui::Event::Escape);
            if (rest[1] == kEscapeByte)
            {
                events.push_back(ftxui::Event::Escape);
            }
            else
            {
                AppendCharacters(rest.substr(1), &events);
            }
        }
        return events;
    }

}  // namespace ql
