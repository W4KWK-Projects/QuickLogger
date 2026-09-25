#include "mouse.hpp"

#include <deque>
#include <iostream>
#include <utility>

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/box.hpp>

namespace ql
{

    // Two clicks on the same row at most this far apart are a double-click.
    static constexpr std::chrono::milliseconds kDoubleClickTime{500};

    // A clickable key, or with `row` 0 or more, a clickable list row.
    struct KeyTarget
    {
        ftxui::Event key;
        int row = -1;
        ftxui::Box box;
        int layer = 0;
    };

    // Last frame's clickable keys. A deque, so a box stays where it is
    // while more are added: ftxui::reflect writes into it when the frame
    // is laid out, after the element holding the reference was made. Each
    // process runs one session, like TopBar's notice database.
    static std::deque<KeyTarget> g_targets;
    static int g_layer = 0;
    static bool g_stop_movement_reports = true;
    // The last row clicked (see ClickedRow), to spot a double-click.
    static int g_last_row = -1;
    static std::chrono::steady_clock::time_point g_last_row_click;

    // The event for a key bar's key name, or false if it isn't one key.
    static bool KeyEvent(const std::string& name, ftxui::Event* event)
    {
        std::string key = name.substr(0, name.find('/'));
        static const ftxui::Event kFunctionKeys[] = {
            ftxui::Event::F1, ftxui::Event::F2,  ftxui::Event::F3,  ftxui::Event::F4,
            ftxui::Event::F5, ftxui::Event::F6,  ftxui::Event::F7,  ftxui::Event::F8,
            ftxui::Event::F9, ftxui::Event::F10, ftxui::Event::F11, ftxui::Event::F12,
        };
        for (int number = 1; number <= 12; ++number)
        {
            if (key == "F" + std::to_string(number))
            {
                *event = kFunctionKeys[number - 1];
                return true;
            }
        }
        if (key == "Esc")
        {
            *event = ftxui::Event::Escape;
            return true;
        }
        if (key == "Enter")
        {
            *event = ftxui::Event::Return;
            return true;
        }
        return false;
    }

    void BeginClickTargets()
    {
        g_targets.clear();
        g_layer = 0;
    }

    void BeginClickLayer()
    {
        ++g_layer;
    }

    // Registers `target` in the current layer and returns the decorator
    // that records where it's drawn.
    static ftxui::Decorator AddTarget(KeyTarget target)
    {
        // Empty until laid out, so a key that isn't drawn can't be hit.
        target.box.x_min = 0;
        target.box.x_max = -1;
        target.layer = g_layer;
        g_targets.push_back(target);
        return ftxui::reflect(g_targets.back().box);
    }

    ftxui::Decorator ClickTarget(const std::string& key)
    {
        KeyTarget target;
        if (!KeyEvent(key, &target.key))
        {
            return ftxui::nothing;
        }
        return AddTarget(target);
    }

    ftxui::Decorator ClickableRow(int index)
    {
        KeyTarget target;
        target.row = index;
        return AddTarget(target);
    }

    // The target under a left press in the topmost layer, or null.
    static const KeyTarget* TargetAt(const ftxui::Mouse& mouse)
    {
        if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Pressed)
        {
            return nullptr;
        }
        int top = g_layer;
        for (const KeyTarget& target : g_targets)
        {
            top = target.layer > top ? target.layer : top;
        }
        for (const KeyTarget& target : g_targets)
        {
            if (target.layer == top && target.box.Contain(mouse.x, mouse.y))
            {
                return &target;
            }
        }
        return nullptr;
    }

    bool ClickedKey(const ftxui::Mouse& mouse, ftxui::Event* key)
    {
        const KeyTarget* target = TargetAt(mouse);
        if (target == nullptr || target->row >= 0)
        {
            return false;
        }
        *key = target->key;
        return true;
    }

    bool ClickedRow(const ftxui::Mouse& mouse, int* row, bool* double_click)
    {
        const KeyTarget* target = TargetAt(mouse);
        if (target == nullptr || target->row < 0)
        {
            return false;
        }
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        *row = target->row;
        *double_click = target->row == g_last_row && now - g_last_row_click <= kDoubleClickTime;
        // A third click starts over rather than counting as another double.
        g_last_row = *double_click ? -1 : target->row;
        g_last_row_click = now;
        return true;
    }

    // Draws a window's contents as a new click layer.
    class ClickLayer : public ftxui::ComponentBase
    {
    public:
        explicit ClickLayer(ftxui::Component child)
        {
            Add(std::move(child));
        }

        ftxui::Element Render() override
        {
            BeginClickLayer();
            return ftxui::ComponentBase::Render();
        }
    };

    ftxui::Component LayeredModal(ftxui::Component main, ftxui::Component modal, const bool* show)
    {
        return ftxui::Modal(std::move(main), ftxui::Make<ClickLayer>(std::move(modal)), show);
    }

    DoubleClickToEnter::DoubleClickToEnter(ftxui::Component list, ftxui::ScreenInteractive* screen)
        : screen_(screen)
    {
        Add(std::move(list));
    }

    bool DoubleClickToEnter::OnEvent(ftxui::Event event)
    {
        // A list highlights the row when the button is let go (FTXUI's
        // Menu), so that's what's counted.
        if (!event.is_mouse() || event.mouse().button != ftxui::Mouse::Left ||
            event.mouse().motion != ftxui::Mouse::Released)
        {
            return ftxui::ComponentBase::OnEvent(event);
        }
        if (!ftxui::ComponentBase::OnEvent(event))
        {
            last_click_y_ = -1;
            return false;
        }
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (event.mouse().y == last_click_y_ && now - last_click_ <= kDoubleClickTime)
        {
            // A third click starts over rather than pressing Enter again.
            last_click_y_ = -1;
            if (screen_ != nullptr)
            {
                screen_->PostEvent(ftxui::Event::Return);
            }
            return true;
        }
        last_click_y_ = event.mouse().y;
        last_click_ = now;
        return true;
    }

    void RequestMouseMovementReportsOff()
    {
        g_stop_movement_reports = true;
    }

    void StopMouseMovementReports()
    {
        if (!g_stop_movement_reports)
        {
            return;
        }
        g_stop_movement_reports = false;
        // Terminals keep one mouse reporting mode, which each of 1000
        // (clicks), 1002 and 1003 (any movement) sets, and turning any of
        // them off turns reporting off altogether. So turn off FTXUI's 1003,
        // then turn clicks-only 1000 back on. 1006 (the report format) is a
        // separate setting and stays on.
        std::cout << "\x1B[?1003l\x1B[?1000h" << std::flush;
    }

}  // namespace ql
