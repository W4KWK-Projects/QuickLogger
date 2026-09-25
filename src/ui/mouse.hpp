#pragma once

#include <chrono>
#include <string>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

namespace ftxui
{
    class ScreenInteractive;
}

namespace ql
{

    // The mouse is optional: everything works from the keyboard, and a
    // click only ever does what a key already does. Clicking a key in a
    // key bar (or F1 Help in the top bar) presses that key, and in a list a
    // click highlights a row and a double-click presses Enter on it.
    //
    // Only clicks are reported, not movement. FTXUI asks the terminal for
    // every mouse movement too (mode 1003), which over SSH meant a stream of
    // reports, and a redraw for each, just from moving the mouse across the
    // window -- and a list's highlight followed the pointer, so a stray
    // nudge could change the selection just before Enter. See
    // StopMouseMovementReports.

    // Call at the start of each frame, before anything is drawn: forgets
    // the last frame's clickable keys.
    void BeginClickTargets();

    // Marks everything drawn from here on this frame as a window over what
    // was drawn before. While a window is up, only its own keys can be
    // clicked, not those of the page underneath. See LayeredModal.
    void BeginClickLayer();

    // Makes the element it decorates press `key` when clicked: a key
    // bar's key name such as "F2", "Esc" or "F2/Enter" (the first of the
    // two). Names that aren't a single key ("Up/Down", "0-9") aren't
    // clickable, and the element is left as it is.
    ftxui::Decorator ClickTarget(const std::string& key);

    // If `mouse` is a left click on a clickable key in the topmost layer
    // drawn last frame, sets `*key` to that key's event and returns true.
    bool ClickedKey(const ftxui::Mouse& mouse, ftxui::Event* key);

    // Makes a row of a list QuickLogger draws itself (not an FTXUI Menu,
    // e.g. a list numbered for picking) clickable as row `index`.
    ftxui::Decorator ClickableRow(int index);

    // If `mouse` is a left click on a ClickableRow in the topmost layer
    // drawn last frame, sets `*row` to its index and `*double_click` to
    // whether it's the second click on that row in quick succession, and
    // returns true.
    bool ClickedRow(const ftxui::Mouse& mouse, int* row, bool* double_click);

    // ftxui::Modal, with `modal`'s keys drawn in a layer of their own (see
    // BeginClickLayer). Use it for every window.
    ftxui::Component LayeredModal(ftxui::Component main, ftxui::Component modal, const bool* show);

    // Wraps a list so a double-click on one of its rows presses Enter, as
    // if the row had been highlighted and Enter pressed. `screen` may be
    // null (tests), when a double-click just highlights.
    class DoubleClickToEnter : public ftxui::ComponentBase
    {
    public:
        DoubleClickToEnter(ftxui::Component list, ftxui::ScreenInteractive* screen);

        bool OnEvent(ftxui::Event event) override;

    private:
        ftxui::ScreenInteractive* screen_;
        std::chrono::steady_clock::time_point last_click_;
        int last_click_y_ = -1;
    };

    // Turns off FTXUI's movement reports (see above), keeping click
    // reports. FTXUI turns them on each time it takes over the terminal:
    // when the session starts and again after a ZMODEM transfer. Call
    // RequestMouseMovementReportsOff at those times and
    // StopMouseMovementReports at the start of each frame; it only writes
    // anything when requested. FTXUI's own cleanup turns click reports
    // off too when the session ends.
    void RequestMouseMovementReportsOff();
    void StopMouseMovementReports();

}  // namespace ql
