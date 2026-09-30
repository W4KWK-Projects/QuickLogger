#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <ftxui/screen/screen.hpp>

namespace ftxui
{
    class ScreenInteractive;
}

namespace ql
{

    // Turns each frame FTXUI draws into only the terminal output needed to
    // change what's on screen into it.
    //
    // FTXUI v5 prints the whole screen on every frame -- 7-10 KB at 80x24 and
    // more on bigger terminals -- and its Menu draws a second, identical frame
    // after every selection change, so over SSH each keystroke cost about
    // 14 KB. This keeps the last frame it sent and writes only the runs of
    // cells that changed (cursor positioning plus colors and attributes),
    // and nothing at all for a frame identical to the last one.
    //
    // Every cell is still rewritten at least once a minute (the top bar's
    // clock draws a frame each minute), so anything else written to the
    // terminal -- the SSH listener's messages in a console session, say --
    // is painted over as it was when FTXUI drew whole frames.
    //
    // It needs FTXUI's frame-writer hook, which the build patches in
    // (cmake/patch_ftxui.cmake). Positions are absolute, so it's only for a
    // Fullscreen ScreenInteractive, whose frame starts at the terminal's
    // top-left corner.
    class FrameWriter
    {
    public:
        // The output for `screen`'s current frame. `full` repaints the whole
        // screen, for when the terminal's copy can't be trusted (FTXUI's
        // first frame, a resize, or the terminal having been handed to
        // another program); a change of size also repaints in full.
        std::string Write(const ftxui::Screen& screen, bool full);

    private:
        // The frame as the terminal has it now.
        std::vector<std::vector<ftxui::Pixel>> cells_;
        // The links cells_' hyperlink ids stand for. FTXUI numbers links
        // afresh each frame, so kept cells use this numbering instead.
        std::vector<std::string> links_;
        int width_ = 0;
        int height_ = 0;
        // Where the visible cursor was put last, and its shape (-1: not
        // known, so the next frame sets it).
        int cursor_x_ = -1;
        int cursor_y_ = -1;
        int cursor_shape_ = -1;
        // When every cell was last written.
        std::chrono::steady_clock::time_point last_refresh_;
    };

    // Makes `screen` draw through `writer` (which must outlive the screen's
    // loop), when FTXUI has the hook; otherwise FTXUI draws whole frames.
    void InstallFrameWriter(ftxui::ScreenInteractive* screen, FrameWriter* writer);

}  // namespace ql
