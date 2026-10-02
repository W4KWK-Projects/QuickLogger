#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

namespace ql
{

    // One line of wrapped text: bytes [start, end) of the text. A line that
    // ends a paragraph includes its '\n'; a line broken at a space includes
    // that space. The lines of a text follow on from each other, from 0 to
    // its size, and there's always at least one.
    struct TextLineSpan
    {
        std::size_t start = 0;
        std::size_t end = 0;
    };

    // `text` word-wrapped to lines of at most `width` characters, not
    // counting the space a line is broken at: broken at the last space that
    // fits, or mid-word for a word longer than a line. Characters are UTF-8
    // code points, each taken as one column wide.
    std::vector<TextLineSpan> WrapText(const std::string& text, int width);

    // A multi-line, word-wrapped text editor for the Session Notes window
    // (F12): FTXUI 5's Input doesn't wrap, and notes are mostly long
    // paragraphs. Enter starts a new line; the arrows, Home/End and
    // PageUp/PageDown move through the text as it's wrapped on screen.
    // `*cursor` is a byte offset into `*text`. `*width` and `*height` are the
    // editing area's size, set by whoever draws the window before it
    // renders this. While `*read_only`, only moving around works.
    class NotesEditor : public ftxui::ComponentBase
    {
    public:
        NotesEditor(std::string* text, int* cursor, const bool* read_only, const int* width, const int* height);

        ftxui::Element Render() override;
        bool OnEvent(ftxui::Event event) override;
        bool Focusable() const override;

    private:
        // Moves the cursor `rows` wrapped lines up (negative) or down,
        // keeping its column where the line is long enough.
        void MoveLines(int rows);
        std::size_t Cursor() const;
        void SetCursor(std::size_t position);
        // The text wrapped to the width (see WrapText), worked out again
        // only when the text or width has changed since: not on every
        // frame, nor for every key.
        const std::vector<TextLineSpan>& Lines();

        std::string* text_;
        int* cursor_;
        const bool* read_only_;
        const int* width_;
        const int* height_;
        // What Lines() last wrapped, and the lines it made.
        std::string wrapped_text_;
        int wrapped_width_ = -1;
        std::vector<TextLineSpan> lines_;
    };

}  // namespace ql
