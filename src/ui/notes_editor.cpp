#include "notes_editor.hpp"

#include <algorithm>

#include <ftxui/dom/elements.hpp>

#include "chrome.hpp"

namespace ql
{

    static bool IsContinuationByte(char c)
    {
        return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
    }

    // The byte offset of the code point after (or before) the one at
    // `position`.
    static std::size_t NextCodePoint(const std::string& text, std::size_t position)
    {
        if (position >= text.size())
        {
            return text.size();
        }
        ++position;
        while (position < text.size() && IsContinuationByte(text[position]))
        {
            ++position;
        }
        return position;
    }

    static std::size_t PreviousCodePoint(const std::string& text, std::size_t position)
    {
        if (position == 0)
        {
            return 0;
        }
        --position;
        while (position > 0 && IsContinuationByte(text[position]))
        {
            --position;
        }
        return position;
    }

    static int CountCodePoints(const std::string& text, std::size_t start, std::size_t end)
    {
        int count = 0;
        for (std::size_t i = start; i < end; ++i)
        {
            if (!IsContinuationByte(text[i]))
            {
                ++count;
            }
        }
        return count;
    }

    std::vector<TextLineSpan> WrapText(const std::string& text, int width)
    {
        width = std::max(width, 1);
        std::vector<TextLineSpan> lines;
        std::size_t start = 0;
        while (true)
        {
            std::size_t paragraph_end = text.find('\n', start);
            if (paragraph_end == std::string::npos)
            {
                paragraph_end = text.size();
            }
            std::size_t line_start = start;
            while (true)
            {
                std::size_t position = line_start;
                std::size_t last_space = std::string::npos;
                int count = 0;
                while (position < paragraph_end && count < width)
                {
                    if (text[position] == ' ')
                    {
                        last_space = position;
                    }
                    position = NextCodePoint(text, position);
                    ++count;
                }
                if (position >= paragraph_end)
                {
                    break;  // The rest of the paragraph fits.
                }
                std::size_t end = position;
                if (text[position] == ' ')
                {
                    end = position + 1;
                }
                else if (last_space != std::string::npos && last_space > line_start)
                {
                    end = last_space + 1;
                }
                lines.push_back({line_start, end});
                line_start = end;
            }
            if (paragraph_end == text.size())
            {
                lines.push_back({line_start, paragraph_end});
                return lines;
            }
            lines.push_back({line_start, paragraph_end + 1});
            start = paragraph_end + 1;
        }
    }

    // The line `position` is on.
    static std::size_t LineOf(const std::vector<TextLineSpan>& lines, std::size_t position)
    {
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (position < lines[i].end)
            {
                return i;
            }
        }
        return lines.size() - 1;
    }

    // The furthest the cursor can go on line `index`: onto the last
    // character (the space or '\n' it ends with), or past the end of the
    // text on the last line.
    static std::size_t LastCursorPosition(const std::string& text,
                                          const std::vector<TextLineSpan>& lines, std::size_t index)
    {
        if (index + 1 == lines.size())
        {
            return lines[index].end;
        }
        return std::max(lines[index].start, PreviousCodePoint(text, lines[index].end));
    }

    NotesEditor::NotesEditor(std::string* text, int* cursor, const bool* read_only,
                             const int* width, const int* height)
        : text_(text), cursor_(cursor), read_only_(read_only), width_(width), height_(height)
    {
    }

    bool NotesEditor::Focusable() const
    {
        return true;
    }

    std::size_t NotesEditor::Cursor() const
    {
        return std::min(static_cast<std::size_t>(std::max(*cursor_, 0)), text_->size());
    }

    void NotesEditor::SetCursor(std::size_t position)
    {
        *cursor_ = static_cast<int>(std::min(position, text_->size()));
    }

    ftxui::Element NotesEditor::Render()
    {
        // One column is kept for the cursor after a full line.
        std::vector<TextLineSpan> lines = WrapText(*text_, *width_ - 1);
        std::size_t cursor = Cursor();
        std::size_t cursor_line = LineOf(lines, cursor);
        ftxui::Elements rows;
        rows.reserve(lines.size());
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            std::size_t end = lines[i].end;
            if (end > lines[i].start && (*text_)[end - 1] == '\n')
            {
                --end;
            }
            if (i != cursor_line)
            {
                rows.push_back(ftxui::text(text_->substr(lines[i].start, end - lines[i].start)));
                continue;
            }
            // The character under the cursor, or a space at the end of a
            // line, shown inverted.
            std::size_t after = cursor < end ? NextCodePoint(*text_, cursor) : cursor;
            std::string under = cursor < end ? text_->substr(cursor, after - cursor) : " ";
            ftxui::Element cursor_cell = ftxui::text(under);
            if (!*read_only_ && Focused())
            {
                cursor_cell = cursor_cell | ftxui::inverted;
            }
            rows.push_back(ftxui::hbox({
                               ftxui::text(text_->substr(lines[i].start, cursor - lines[i].start)),
                               cursor_cell,
                               ftxui::text(text_->substr(after, end - after)),
                           }) |
                           ftxui::focus);
        }
        return ftxui::vbox(std::move(rows)) | ftxui::color(kColorData);
    }

    void NotesEditor::MoveLines(int rows)
    {
        std::vector<TextLineSpan> lines = WrapText(*text_, *width_ - 1);
        std::size_t cursor = Cursor();
        std::size_t line = LineOf(lines, cursor);
        int column = CountCodePoints(*text_, lines[line].start, cursor);
        int target =
            std::clamp(static_cast<int>(line) + rows, 0, static_cast<int>(lines.size()) - 1);
        std::size_t target_line = static_cast<std::size_t>(target);
        std::size_t last = LastCursorPosition(*text_, lines, target_line);
        std::size_t position = lines[target_line].start;
        for (int i = 0; i < column && position < last; ++i)
        {
            position = NextCodePoint(*text_, position);
        }
        SetCursor(position);
    }

    bool NotesEditor::OnEvent(ftxui::Event event)
    {
        if (event.is_mouse())
        {
            return false;
        }
        std::size_t cursor = Cursor();
        if (event == ftxui::Event::ArrowLeft)
        {
            SetCursor(PreviousCodePoint(*text_, cursor));
            return true;
        }
        if (event == ftxui::Event::ArrowRight)
        {
            SetCursor(NextCodePoint(*text_, cursor));
            return true;
        }
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)
        {
            MoveLines(event == ftxui::Event::ArrowUp ? -1 : 1);
            return true;
        }
        if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
        {
            int page = std::max(*height_ - 1, 1);
            MoveLines(event == ftxui::Event::PageUp ? -page : page);
            return true;
        }
        if (event == ftxui::Event::Home || event == ftxui::Event::End)
        {
            std::vector<TextLineSpan> lines = WrapText(*text_, *width_ - 1);
            std::size_t line = LineOf(lines, cursor);
            SetCursor(event == ftxui::Event::Home ? lines[line].start
                                                  : LastCursorPosition(*text_, lines, line));
            return true;
        }
        if (*read_only_)
        {
            return false;
        }
        if (event == ftxui::Event::Return)
        {
            text_->insert(cursor, "\n");
            SetCursor(cursor + 1);
            return true;
        }
        if (event == ftxui::Event::Backspace)
        {
            std::size_t previous = PreviousCodePoint(*text_, cursor);
            text_->erase(previous, cursor - previous);
            SetCursor(previous);
            return true;
        }
        if (event == ftxui::Event::Delete)
        {
            text_->erase(cursor, NextCodePoint(*text_, cursor) - cursor);
            return true;
        }
        if (event.is_character())
        {
            const std::string& typed = event.character();
            // Control characters (a pasted tab, say) aren't kept.
            if (typed.size() == 1 && static_cast<unsigned char>(typed[0]) < 0x20)
            {
                return true;
            }
            text_->insert(cursor, typed);
            SetCursor(cursor + typed.size());
            return true;
        }
        return false;
    }

}  // namespace ql
