// The Session Notes editor: word wrapping and moving through wrapped text.

#include <string>
#include <vector>

#include <ftxui/component/event.hpp>

#include "../src/ui/notes_editor.hpp"
#include "test_framework.hpp"

namespace ql
{

    // The wrapped lines of `text` as strings.
    static std::vector<std::string> Lines(const std::string& text, int width)
    {
        std::vector<std::string> lines;
        for (const TextLineSpan& span : WrapText(text, width))
        {
            lines.push_back(text.substr(span.start, span.end - span.start));
        }
        return lines;
    }

    QL_TEST(NotesWrapAtTheLastSpaceThatFits)
    {
        std::vector<std::string> lines = Lines("the quick brown fox jumps", 10);
        REQUIRE(lines.size() == 3);
        CHECK_EQ(lines[0], std::string("the quick "));
        CHECK_EQ(lines[1], std::string("brown fox "));
        CHECK_EQ(lines[2], std::string("jumps"));
    }

    QL_TEST(NotesKeepTheirParagraphs)
    {
        std::vector<std::string> lines = Lines("one\n\ntwo\n", 10);
        REQUIRE(lines.size() == 4);
        CHECK_EQ(lines[0], std::string("one\n"));
        CHECK_EQ(lines[1], std::string("\n"));
        CHECK_EQ(lines[2], std::string("two\n"));
        CHECK_EQ(lines[3], std::string(""));
        CHECK_EQ(Lines("", 10).size(), std::size_t{1});
    }

    QL_TEST(ALongWordIsBrokenAndAccentsCountOnce)
    {
        std::vector<std::string> lines = Lines("abcdefghij", 4);
        REQUIRE(lines.size() == 3);
        CHECK_EQ(lines[0], std::string("abcd"));
        CHECK_EQ(lines[2], std::string("ij"));
        // Four characters, eight bytes: one line.
        CHECK_EQ(Lines("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 4).size(), std::size_t{1});
    }

    QL_TEST(TheNotesEditorMovesThroughWrappedLines)
    {
        std::string text = "the quick brown fox";
        int cursor = 0;
        bool read_only = false;
        int width = 11;  // Lines of 10, plus a column for the cursor.
        int height = 5;
        NotesEditor editor(&text, &cursor, &read_only, &width, &height);

        editor.OnEvent(ftxui::Event::ArrowDown);
        CHECK_EQ(cursor, 10);  // "brown fox" starts at 10.
        editor.OnEvent(ftxui::Event::End);
        CHECK_EQ(cursor, 19);  // The end of the text.
        editor.OnEvent(ftxui::Event::ArrowUp);
        CHECK_EQ(cursor, 9);  // Column 9 on "the quick ": its space.
        editor.OnEvent(ftxui::Event::Home);
        CHECK_EQ(cursor, 0);

        editor.OnEvent(ftxui::Event::Character("A"));
        editor.OnEvent(ftxui::Event::Return);
        CHECK_EQ(text, std::string("A\nthe quick brown fox"));
        CHECK_EQ(cursor, 2);
        editor.OnEvent(ftxui::Event::Backspace);
        editor.OnEvent(ftxui::Event::Delete);
        CHECK_EQ(text, std::string("Ahe quick brown fox"));

        // Read-only: moving works, editing doesn't.
        read_only = true;
        CHECK(!editor.OnEvent(ftxui::Event::Character("Z")));
        CHECK(editor.OnEvent(ftxui::Event::ArrowRight));
        CHECK_EQ(text, std::string("Ahe quick brown fox"));
    }

}  // namespace ql
