#include "frame_writer.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/string.hpp>

namespace ql
{

    // Unchanged cells between two changed ones are rewritten, rather than
    // moving the cursor past them, when there are no more than this many:
    // a cursor move costs about as many bytes.
    static const int kMaxRewrittenGap = 4;

    // The cursor's position isn't known (after a clear, or a character
    // written into the last column, where terminals differ).
    static const int kUnknownPosition = -1;

    // Every cell is rewritten, changed or not, in the first frame at least
    // this long after the last time. Under the clock's once-a-minute frame,
    // so an idle screen is rewritten every minute.
    static const std::chrono::seconds kRefreshInterval(30);

    static bool IsWide(const ftxui::Pixel& pixel)
    {
        return ftxui::string_width(pixel.character) == 2;
    }

    static bool SameStyle(const ftxui::Pixel& a, const ftxui::Pixel& b)
    {
        return a.bold == b.bold && a.dim == b.dim && a.underlined == b.underlined &&
               a.underlined_double == b.underlined_double && a.blink == b.blink && a.inverted == b.inverted &&
               a.strikethrough == b.strikethrough && a.foreground_color == b.foreground_color &&
               a.background_color == b.background_color;
    }

    // The escape sequences that change the terminal's style from `from` to
    // `to` -- the same ones FTXUI's Screen::ToString uses. Links are passed
    // separately, since a Pixel's hyperlink id only means something within
    // its own frame.
    static void AppendStyleChange(std::string* out, const ftxui::Pixel& from, const std::string& from_link,
                                  const ftxui::Pixel& to, const std::string& to_link)
    {
        if (from_link != to_link)
        {
            *out += "\x1B]8;;" + to_link + "\x1B\\";
        }
        if (from.bold != to.bold || from.dim != to.dim)
        {
            if ((from.bold && !to.bold) || (from.dim && !to.dim))
            {
                *out += "\x1B[22m";
            }
            if (to.bold)
            {
                *out += "\x1B[1m";
            }
            if (to.dim)
            {
                *out += "\x1B[2m";
            }
        }
        if (from.underlined != to.underlined || from.underlined_double != to.underlined_double)
        {
            *out += to.underlined ? "\x1B[4m" : to.underlined_double ? "\x1B[21m" : "\x1B[24m";
        }
        if (from.blink != to.blink)
        {
            *out += to.blink ? "\x1B[5m" : "\x1B[25m";
        }
        if (from.inverted != to.inverted)
        {
            *out += to.inverted ? "\x1B[7m" : "\x1B[27m";
        }
        if (from.strikethrough != to.strikethrough)
        {
            *out += to.strikethrough ? "\x1B[9m" : "\x1B[29m";
        }
        if (from.foreground_color != to.foreground_color || from.background_color != to.background_color)
        {
            // Appended piece by piece: no temporary strings per change.
            out->append("\x1B[");
            out->append(to.foreground_color.Print(false));
            out->append("m\x1B[");
            out->append(to.background_color.Print(true));
            out->push_back('m');
        }
    }

    // The terminal's cursor and current style while a frame is written.
    struct Pen
    {
        int x = kUnknownPosition;
        int y = kUnknownPosition;
        ftxui::Pixel style;
        std::string link;
    };

    // `number` in decimal, appended to `out` straight from a buffer on the
    // stack (no temporary string, as std::to_string would make).
    static void AppendNumber(std::string* out, int number)
    {
        char digits[12];
        std::to_chars_result result = std::to_chars(digits, digits + sizeof(digits), number);
        out->append(digits, static_cast<std::size_t>(result.ptr - digits));
    }

    static void MoveTo(std::string* out, Pen* pen, int x, int y)
    {
        if (pen->y == y && pen->x == x)
        {
            return;
        }
        out->append("\x1B[");
        if (pen->y == y && pen->x != kUnknownPosition && x > pen->x)
        {
            AppendNumber(out, x - pen->x);
            out->push_back('C');
        }
        else
        {
            // Always row AND column, even for column 1: "ESC[nH" alone
            // (column left to default) is standard, but Termius on iOS
            // misreads it, putting every line that starts at the left
            // edge on the top row.
            AppendNumber(out, y + 1);
            out->push_back(';');
            AppendNumber(out, x + 1);
            out->push_back('H');
        }
        pen->x = x;
        pen->y = y;
    }

    static std::uint8_t LinkId(std::vector<std::string>* links, const std::string& link)
    {
        for (std::size_t i = 0; i < links->size(); ++i)
        {
            if ((*links)[i] == link)
            {
                return static_cast<std::uint8_t>(i);
            }
        }
        // Hyperlink ids are 8 bits; past that, cells lose their link here
        // and are rewritten whenever compared (harmless: QuickLogger draws
        // no links).
        if (links->size() > 255)
        {
            return 0;
        }
        links->push_back(link);
        return static_cast<std::uint8_t>(links->size() - 1);
    }

    std::string FrameWriter::Write(const ftxui::Screen& screen, bool full)
    {
        const int width = screen.dimx();
        const int height = screen.dimy();
        std::string out;
        Pen pen;

        const std::chrono::steady_clock::time_point clock_now = std::chrono::steady_clock::now();
        full = full || width != width_ || height != height_;
        const bool refresh = full || clock_now - last_refresh_ >= kRefreshInterval;
        if (refresh)
        {
            last_refresh_ = clock_now;
            cursor_shape_ = -1;
        }

        if (full)
        {
            // Clear, then compare against blank cells: blanks needn't be sent.
            out += "\x1B[0m\x1B]8;;\x1B\\\x1B[2J";
            width_ = width;
            height_ = height;
            cells_.assign(static_cast<std::size_t>(height), std::vector<ftxui::Pixel>(static_cast<std::size_t>(width)));
            links_.assign(1, std::string());
        }
        else if (refresh)
        {
            // Something else may have left a style set.
            out += "\x1B[0m";
        }

        std::vector<char> dirty(static_cast<std::size_t>(width));
        for (int y = 0; y < height; ++y)
        {
            std::vector<ftxui::Pixel>& row = cells_[static_cast<std::size_t>(y)];
            bool any_dirty = false;
            for (int x = 0; x < width; ++x)
            {
                const ftxui::Pixel& now = screen.PixelAt(x, y);
                const ftxui::Pixel& was = row[static_cast<std::size_t>(x)];
                const bool changed = (refresh && !full) || now.character != was.character || !SameStyle(now, was) ||
                                     screen.Hyperlink(now.hyperlink) != links_[was.hyperlink];
                dirty[static_cast<std::size_t>(x)] = changed;
                any_dirty = any_dirty || changed;
            }
            if (!any_dirty)
            {
                continue;
            }

            // A wide character and the cell it covers are written together:
            // terminals blank all of a wide character when half of it is
            // overwritten. (FTXUI leaves the covered cell empty, so a change
            // to either usually changes both anyway.)
            for (int x = 0; x + 1 < width; ++x)
            {
                if (IsWide(screen.PixelAt(x, y)) || IsWide(row[static_cast<std::size_t>(x)]))
                {
                    const bool either = dirty[x] || dirty[x + 1];
                    dirty[x] = either;
                    dirty[x + 1] = either;
                }
            }

            int x = 0;
            while (x < width)
            {
                if (!dirty[x])
                {
                    ++x;
                    continue;
                }
                // The run of cells to write: changed ones, joined across
                // short gaps of unchanged ones.
                int end = x + 1;
                while (end < width)
                {
                    int next = end;
                    while (next < width && !dirty[next])
                    {
                        ++next;
                    }
                    if (next == width || next - end > kMaxRewrittenGap)
                    {
                        break;
                    }
                    end = next + 1;
                }

                for (int i = x; i < end; ++i)
                {
                    const ftxui::Pixel& now = screen.PixelAt(i, y);
                    const std::string& link = screen.Hyperlink(now.hyperlink);
                    ftxui::Pixel& kept = row[static_cast<std::size_t>(i)];
                    kept = now;
                    kept.hyperlink = LinkId(&links_, link);

                    // As in Screen::ToString: the cell after a wide
                    // character is covered by it, not written.
                    if (i > 0 && IsWide(screen.PixelAt(i - 1, y)))
                    {
                        continue;
                    }
                    MoveTo(&out, &pen, i, y);
                    AppendStyleChange(&out, pen.style, pen.link, now, link);
                    pen.style = now;
                    pen.link = link;
                    out += now.character;

                    const int advance = ftxui::string_width(now.character);
                    if ((advance == 1 || advance == 2) && i + advance < width)
                    {
                        pen.x = i + advance;
                    }
                    else
                    {
                        pen.x = kUnknownPosition;
                        pen.y = kUnknownPosition;
                    }
                }
                x = end;
            }
        }

        // Leave the terminal in its default style between frames.
        AppendStyleChange(&out, pen.style, pen.link, ftxui::Pixel(), std::string());

        ftxui::Screen::Cursor cursor = screen.cursor();
        int shape = static_cast<int>(cursor.shape);
        if (shape < ftxui::Screen::Cursor::Hidden || shape > ftxui::Screen::Cursor::Bar)
        {
            shape = ftxui::Screen::Cursor::Hidden;
        }
        if (shape == ftxui::Screen::Cursor::Hidden)
        {
            if (cursor_shape_ != shape)
            {
                out += "\x1B[?25l";
            }
        }
        else
        {
            const int cursor_x = cursor.x < 0 ? 0 : (cursor.x >= width ? width - 1 : cursor.x);
            const int cursor_y = cursor.y < 0 ? 0 : (cursor.y >= height ? height - 1 : cursor.y);
            // Where the cursor was left isn't relied on from one frame to
            // the next, in case something else wrote to the terminal.
            if (!out.empty() || cursor_x != cursor_x_ || cursor_y != cursor_y_)
            {
                MoveTo(&out, &pen, cursor_x, cursor_y);
            }
            cursor_x_ = cursor_x;
            cursor_y_ = cursor_y;
            if (cursor_shape_ != shape)
            {
                out += "\x1B[?25h\x1B[" + std::to_string(shape) + " q";
            }
        }
        cursor_shape_ = shape;
        return out;
    }

    // Hands FTXUI's frames to a FrameWriter.
    class FrameWriterCall
    {
    public:
        explicit FrameWriterCall(FrameWriter* writer) : writer_(writer) {}

        std::string operator()(const ftxui::Screen& screen, bool full) const
        {
            return writer_->Write(screen, full);
        }

    private:
        FrameWriter* writer_;
    };

    void InstallFrameWriter(ftxui::ScreenInteractive* screen, FrameWriter* writer)
    {
#ifdef QUICKLOGGER_FTXUI_FRAME_WRITER
        screen->SetFrameWriter(FrameWriterCall(writer));
#else
        (void)screen;
        (void)writer;
#endif
    }

}  // namespace ql
