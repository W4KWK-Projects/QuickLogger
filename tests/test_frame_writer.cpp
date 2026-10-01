// Sending only the changed parts of each frame (see FrameWriter).

#include <cstdint>
#include <string>
#include <vector>

#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include "../src/ui/frame_writer.hpp"
#include "test_framework.hpp"

namespace ql
{

    // One cell of TestTerminal: its text and style as SGR parameters.
    struct TerminalCell
    {
        std::string character = " ";
        // Covered by the wide character to its left.
        bool covered = false;
        std::string foreground = "39";
        std::string background = "49";
        bool bold = false;
        bool dim = false;
        bool inverted = false;
        bool underlined = false;
    };

    // Just enough of a terminal to play back what FrameWriter writes: cursor
    // moves, clearing, SGR styles and wide characters. Anything it doesn't
    // understand, or a character written past the right edge (FrameWriter
    // must never rely on wrapping), sets `error`.
    class TestTerminal
    {
    public:
        TestTerminal(int width, int height)
            : width_(width), height_(height), cells_(static_cast<std::size_t>(width * height))
        {
        }

        void Play(const std::string& output)
        {
            std::size_t i = 0;
            while (i < output.size())
            {
                if (output[i] != '\x1B')
                {
                    std::size_t length = GlyphLength(output, i);
                    Put(output.substr(i, length));
                    i += length;
                    continue;
                }
                if (output.compare(i, 2, "\x1B]") == 0)
                {
                    // OSC (hyperlinks), ended by ESC backslash.
                    std::size_t end = output.find("\x1B\\", i);
                    if (end == std::string::npos)
                    {
                        error = "unterminated OSC";
                        return;
                    }
                    i = end + 2;
                    continue;
                }
                if (output.compare(i, 2, "\x1B[") != 0)
                {
                    error = "unknown escape";
                    return;
                }
                std::size_t end = i + 2;
                while (end < output.size() && !((output[end] >= 'A' && output[end] <= 'Z') ||
                                                (output[end] >= 'a' && output[end] <= 'z')))
                {
                    ++end;
                }
                if (end == output.size())
                {
                    error = "unterminated CSI";
                    return;
                }
                Csi(output.substr(i + 2, end - i - 2), output[end]);
                i = end + 1;
            }
        }

        const TerminalCell& At(int x, int y) const
        {
            return cells_[static_cast<std::size_t>(y * width_ + x)];
        }

        std::string error;
        bool cursor_visible = true;
        int x = 0;
        int y = 0;

    private:
        static std::size_t GlyphLength(const std::string& text, std::size_t i)
        {
            unsigned char lead = static_cast<unsigned char>(text[i]);
            std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            return length;
        }

        TerminalCell& Cell(int cx, int cy)
        {
            return cells_[static_cast<std::size_t>(cy * width_ + cx)];
        }

        // Blanks the whole of any wide character covering (cx, cy).
        void BreakWide(int cx, int cy)
        {
            if (Cell(cx, cy).covered && cx > 0)
            {
                Cell(cx - 1, cy).character = " ";
                Cell(cx, cy).covered = false;
            }
            if (ftxui::string_width(Cell(cx, cy).character) == 2 && cx + 1 < width_)
            {
                Cell(cx + 1, cy).covered = false;
                Cell(cx + 1, cy).character = " ";
            }
        }

        void Put(const std::string& glyph)
        {
            int glyph_width = ftxui::string_width(glyph);
            if (x < 0 || y < 0 || y >= height_ || x + glyph_width > width_)
            {
                error = "character written at " + std::to_string(x) + "," + std::to_string(y);
                return;
            }
            BreakWide(x, y);
            if (glyph_width == 2)
            {
                BreakWide(x + 1, y);
            }
            TerminalCell& cell = Cell(x, y);
            cell.character = glyph;
            cell.covered = false;
            cell.foreground = pen_.foreground;
            cell.background = pen_.background;
            cell.bold = pen_.bold;
            cell.dim = pen_.dim;
            cell.inverted = pen_.inverted;
            cell.underlined = pen_.underlined;
            if (glyph_width == 2)
            {
                TerminalCell& covered = Cell(x + 1, y);
                covered = cell;
                covered.character = "";
                covered.covered = true;
            }
            // Past the last column the position is left undefined here.
            x += glyph_width;
            if (x >= width_)
            {
                x = -1;
            }
        }

        void Csi(const std::string& parameters, char command)
        {
            if (command == 'H')
            {
                std::size_t semicolon = parameters.find(';');
                if (parameters.empty())
                {
                    x = 0;
                    y = 0;
                }
                else if (semicolon == std::string::npos)
                {
                    y = std::stoi(parameters) - 1;
                    x = 0;
                }
                else
                {
                    y = std::stoi(parameters.substr(0, semicolon)) - 1;
                    x = std::stoi(parameters.substr(semicolon + 1)) - 1;
                }
            }
            else if (command == 'C')
            {
                if (x < 0)
                {
                    error = "relative move from an unknown position";
                    return;
                }
                x += std::stoi(parameters);
            }
            else if (command == 'J' && parameters == "2")
            {
                for (TerminalCell& cell : cells_)
                {
                    cell = TerminalCell();
                    cell.background = pen_.background;
                }
            }
            else if (command == 'l' && parameters == "?25")
            {
                cursor_visible = false;
            }
            else if (command == 'h' && parameters == "?25")
            {
                cursor_visible = true;
            }
            else if (command == 'q')
            {
                // Cursor shape.
            }
            else if (command == 'm')
            {
                Sgr(parameters);
            }
            else
            {
                error = std::string("unknown CSI ") + command;
            }
        }

        void Sgr(const std::string& parameters)
        {
            if (parameters == "0" || parameters.empty())
            {
                pen_ = TerminalCell();
            }
            else if (parameters == "1")
            {
                pen_.bold = true;
            }
            else if (parameters == "2")
            {
                pen_.dim = true;
            }
            else if (parameters == "22")
            {
                pen_.bold = false;
                pen_.dim = false;
            }
            else if (parameters == "4")
            {
                pen_.underlined = true;
            }
            else if (parameters == "24")
            {
                pen_.underlined = false;
            }
            else if (parameters == "7")
            {
                pen_.inverted = true;
            }
            else if (parameters == "27")
            {
                pen_.inverted = false;
            }
            else
            {
                int first = std::stoi(parameters);
                if (first == 38 || first == 39 || (first >= 30 && first <= 37) ||
                    (first >= 90 && first <= 97))
                {
                    pen_.foreground = parameters;
                }
                else if (first == 48 || first == 49 || (first >= 40 && first <= 47) ||
                         (first >= 100 && first <= 107))
                {
                    pen_.background = parameters;
                }
                else
                {
                    error = "unknown SGR " + parameters;
                }
            }
        }

        int width_;
        int height_;
        std::vector<TerminalCell> cells_;
        TerminalCell pen_;
    };

    // Whether `terminal` shows exactly what `screen` holds, the way
    // Screen::ToString would draw it; describes the first difference.
    static std::string CompareToScreen(const TestTerminal& terminal, const ftxui::Screen& screen)
    {
        for (int y = 0; y < screen.dimy(); ++y)
        {
            for (int x = 0; x < screen.dimx(); ++x)
            {
                const TerminalCell& cell = terminal.At(x, y);
                std::string where = std::to_string(x) + "," + std::to_string(y);
                if (x > 0 && ftxui::string_width(screen.PixelAt(x - 1, y).character) == 2)
                {
                    if (!cell.covered)
                    {
                        return where + " should be covered by a wide character";
                    }
                    continue;
                }
                const ftxui::Pixel& pixel = screen.PixelAt(x, y);
                if (cell.character != pixel.character)
                {
                    return where + " has \"" + cell.character + "\", not \"" + pixel.character +
                           "\"";
                }
                if (cell.foreground != pixel.foreground_color.Print(false) ||
                    cell.background != pixel.background_color.Print(true) ||
                    cell.bold != pixel.bold || cell.dim != pixel.dim ||
                    cell.inverted != pixel.inverted || cell.underlined != pixel.underlined)
                {
                    return where + " has the wrong style";
                }
            }
        }
        return std::string();
    }

    static ftxui::Screen MakeScreen(int width, int height)
    {
        ftxui::Screen screen(width, height);
        ftxui::Screen::Cursor cursor;
        cursor.shape = ftxui::Screen::Cursor::Hidden;
        screen.SetCursor(cursor);
        return screen;
    }

    // A small, repeatable pseudo-random sequence (no <random>: the same
    // numbers on every platform).
    class TestRandom
    {
    public:
        int Next(int below)
        {
            state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<int>((state_ >> 33) % static_cast<std::uint64_t>(below));
        }

    private:
        std::uint64_t state_ = 12345;
    };

    static void RandomizeCell(ftxui::Screen* screen, TestRandom* random, int x, int y)
    {
        static const char* const kGlyphs[] = {"a", "b", " ", "é", "─", "Z", "日"};
        static const ftxui::Color kColors[] = {ftxui::Color::Default, ftxui::Color::Blue,
                                               ftxui::Color::Yellow, ftxui::Color::GrayDark,
                                               ftxui::Color::RGB(10, 20, 30)};
        ftxui::Pixel& pixel = screen->PixelAt(x, y);
        pixel = ftxui::Pixel();
        std::string glyph = kGlyphs[random->Next(7)];
        if (glyph == "日" && x + 1 >= screen->dimx())
        {
            glyph = "x";
        }
        pixel.character = glyph;
        pixel.foreground_color = kColors[random->Next(5)];
        pixel.background_color = kColors[random->Next(5)];
        pixel.bold = random->Next(4) == 0;
        pixel.dim = random->Next(8) == 0;
        pixel.inverted = random->Next(6) == 0;
        pixel.underlined = random->Next(8) == 0;
        if (glyph == "日")
        {
            // FTXUI leaves the covered cell empty.
            screen->PixelAt(x + 1, y) = pixel;
            screen->PixelAt(x + 1, y).character = "";
        }
    }

    QL_TEST(FrameWriterSendsNothingForAnIdenticalFrame)
    {
        FrameWriter writer;
        ftxui::Screen screen = MakeScreen(20, 5);
        screen.PixelAt(3, 1).character = "Q";
        screen.PixelAt(3, 1).foreground_color = ftxui::Color::Red;
        CHECK(!writer.Write(screen, true).empty());
        CHECK_EQ(writer.Write(screen, false), std::string());
    }

    QL_TEST(FrameWriterSendsOnlyAChangedCell)
    {
        FrameWriter writer;
        ftxui::Screen screen = MakeScreen(80, 24);
        for (int x = 0; x < 80; ++x)
        {
            screen.PixelAt(x, 0).character = "=";
        }
        writer.Write(screen, true);
        screen.PixelAt(4, 2).character = "X";
        CHECK_EQ(writer.Write(screen, false), std::string("\x1B[3;5HX"));
    }

    QL_TEST(FrameWriterAlwaysGivesRowAndColumn)
    {
        // Termius (iOS) misreads "ESC[3H", so even the left edge is "3;1".
        FrameWriter writer;
        ftxui::Screen screen = MakeScreen(80, 24);
        writer.Write(screen, true);
        screen.PixelAt(0, 2).character = "X";
        CHECK_EQ(writer.Write(screen, false), std::string("\x1B[3;1HX"));
    }

    QL_TEST(FrameWriterRepaintsWhenAskedAndOnResize)
    {
        FrameWriter writer;
        ftxui::Screen small = MakeScreen(10, 3);
        small.PixelAt(0, 0).character = "A";
        writer.Write(small, true);
        std::string again = writer.Write(small, true);
        CHECK(again.find("\x1B[2J") != std::string::npos);
        CHECK(again.find('A') != std::string::npos);

        ftxui::Screen large = MakeScreen(12, 3);
        large.PixelAt(0, 0).character = "A";
        std::string resized = writer.Write(large, false);
        CHECK(resized.find("\x1B[2J") != std::string::npos);
        CHECK(resized.find('A') != std::string::npos);
    }

    QL_TEST(FrameWriterPutsTheVisibleCursorBack)
    {
        FrameWriter writer;
        ftxui::Screen screen = MakeScreen(20, 5);
        ftxui::Screen::Cursor cursor;
        cursor.x = 7;
        cursor.y = 2;
        cursor.shape = ftxui::Screen::Cursor::Bar;
        screen.SetCursor(cursor);
        TestTerminal terminal(20, 5);
        terminal.Play(writer.Write(screen, true));
        CHECK(terminal.cursor_visible);
        CHECK(terminal.x == 7 && terminal.y == 2);

        // A change elsewhere moves the cursor away to write it, then back.
        screen.PixelAt(15, 4).character = "k";
        terminal.Play(writer.Write(screen, false));
        CHECK(terminal.x == 7 && terminal.y == 2);
        CHECK_EQ(writer.Write(screen, false), std::string());

        cursor.shape = ftxui::Screen::Cursor::Hidden;
        screen.SetCursor(cursor);
        terminal.Play(writer.Write(screen, false));
        CHECK(!terminal.cursor_visible);
        CHECK_EQ(terminal.error, std::string());
    }

    // Plays hundreds of frames -- small edits, wide characters appearing and
    // disappearing, colors, whole-screen changes -- and checks after each
    // that the terminal shows exactly the frame.
    QL_TEST(FrameWriterKeepsTheTerminalInStepWithRandomFrames)
    {
        const int width = 23;
        const int height = 7;
        FrameWriter writer;
        TestTerminal terminal(width, height);
        ftxui::Screen screen = MakeScreen(width, height);
        TestRandom random;
        for (int frame = 0; frame < 400; ++frame)
        {
            int edits = random.Next(10) == 0 ? width * height : random.Next(12);
            for (int edit = 0; edit < edits; ++edit)
            {
                RandomizeCell(&screen, &random, random.Next(width), random.Next(height));
            }
            // Tidy each row into what FTXUI draws: the cell a wide character
            // covers is empty, and no other cell is.
            for (int y = 0; y < height; ++y)
            {
                bool covered = false;
                for (int x = 0; x < width; ++x)
                {
                    ftxui::Pixel& pixel = screen.PixelAt(x, y);
                    if (covered)
                    {
                        pixel.character = "";
                        covered = false;
                        continue;
                    }
                    if (pixel.character.empty())
                    {
                        pixel.character = "-";
                    }
                    covered = ftxui::string_width(pixel.character) == 2;
                }
            }
            terminal.Play(writer.Write(screen, frame == 0));
            REQUIRE(terminal.error.empty());
            std::string difference = CompareToScreen(terminal, screen);
            if (!difference.empty())
            {
                CHECK_EQ("frame " + std::to_string(frame) + ": " + difference, std::string());
                return;
            }
        }
    }

}  // namespace ql
