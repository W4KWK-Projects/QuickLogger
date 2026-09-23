#include "interactive_session.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "db/database.hpp"
#include "settings.hpp"
#include "uls_import.hpp"
#include "ui/app_state.hpp"
#include "ui/handlers.hpp"
#include "ui/pages.hpp"

namespace ql
{

    // Makes the top bar's clock advance, using as little terminal traffic as
    // possible. FTXUI only redraws in response to an event, so on an idle
    // screen the clock would freeze at whatever minute the last keypress
    // happened in. This thread posts one redraw event when the minute
    // changes -- one screen update per minute, and nothing in between (an
    // SSH session pays for every redraw in bytes on the wire).
    //
    // It sleeps until the next minute boundary, but never for more than
    // kMaxSleep at a stretch, and only posts if the minute really did change
    // when it wakes. The cap costs no traffic (waking up sends nothing); it
    // means a system clock that was stepped or a machine that was suspended
    // -- when a long sleep can overrun the boundary -- is noticed within
    // kMaxSleep instead of up to a minute later. Construct it after the
    // screen and before screen.Loop(); it stops and joins on destruction.
    class ClockTicker
    {
    public:
        explicit ClockTicker(ftxui::ScreenInteractive* screen)
            : screen_(screen), thread_(&ClockTicker::Run, this)
        {
        }

        ~ClockTicker()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_ = true;
            }
            wake_.notify_all();
            thread_.join();
        }

        ClockTicker(const ClockTicker&) = delete;
        ClockTicker& operator=(const ClockTicker&) = delete;

    private:
        static std::int64_t CurrentMinute()
        {
            return static_cast<std::int64_t>(std::time(nullptr)) / 60;
        }

        void Run()
        {
            std::int64_t drawn_minute = CurrentMinute();
            while (true)
            {
                // Until just past the next minute boundary (the margin keeps
                // a wake-up that lands a hair early from spinning), or the
                // cap, whichever comes first.
                std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
                std::chrono::system_clock::time_point next_minute =
                    std::chrono::floor<std::chrono::minutes>(now) + std::chrono::minutes(1);
                std::chrono::system_clock::duration wait_time =
                    next_minute - now + std::chrono::milliseconds(200);
                if (wait_time > kMaxSleep)
                {
                    wait_time = kMaxSleep;
                }
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (stop_)
                    {
                        return;
                    }
                    wake_.wait_for(lock, wait_time);
                    if (stop_)
                    {
                        return;
                    }
                }

                std::int64_t minute = CurrentMinute();
                if (minute != drawn_minute)
                {
                    drawn_minute = minute;
                    screen_->PostEvent(ftxui::Event::Custom);
                }
            }
        }

        static constexpr std::chrono::seconds kMaxSleep{30};

        ftxui::ScreenInteractive* screen_;
        std::mutex mutex_;
        std::condition_variable wake_;
        bool stop_ = false;
        std::thread thread_;
    };

    void RunInteractiveSession(const std::string& settings_path, bool is_console_session)
    {
        ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
        ql::Database db("quicklogger.db");

        ql::AppState state;
        state.db = &db;
        state.db_path = "quicklogger.db";
        state.screen = &screen;
        state.is_console_session = is_console_session;
        state.settings_path = settings_path;
        state.settings = ql::LoadSettings(state.settings_path);

        // First launch (or an upgrade from before the ZIP code became required):
        // force the operator through Settings before anything else. Mirrors
        // ShowSettingsPageHandler's own state setup since that handler isn't
        // reachable yet -- there's no net list to press F4 from until this is
        // done. CancelSettingsHandler refuses to leave kPageSettings while
        // SettingsAreComplete is still false, so Esc can't bypass this.
        if (!ql::SettingsAreComplete(state.settings))
        {
            state.settings_form = state.settings;
            state.page = ql::kPageSettings;
        }

        ql::RefreshNets(&state);

        // Kick off a fresh FCC ULS import automatically if one has never run,
        // previously failed, is stuck "running" (only possible if a prior run
        // crashed, since a clean quit is blocked while one is active), the last
        // completed run is more than a week old, or either auxiliary step (the
        // ZIP-centroid geocode or the ZIP-to-county lookup) specifically failed
        // last time (which a fresh, still-recent `uls` row would otherwise mask
        // for up to a week -- see ShouldRetryAuxiliaryImport). Every session
        // shares the same import_runs bookkeeping, so only one of however many
        // concurrent sessions are open at a given moment actually ends up
        // starting a redundant import -- see Database::TryClaimImportRun.
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (ql::ShouldAutoStartUlsImport(db.GetImportRunStatus("uls"), now) ||
            ql::ShouldRetryAuxiliaryImport(db.GetImportRunStatus("zip_centroids")) ||
            ql::ShouldRetryAuxiliaryImport(db.GetImportRunStatus("zip_counties")))
        {
            ql::StartUlsImport(state.db_path, &state.uls_import_progress, &screen);
        }

        ftxui::Component net_list_page = ql::BuildNetListPage(&state);
        ftxui::Component create_net_page = ql::BuildCreateNetPage(&state);
        ftxui::Component select_role_page = ql::BuildSelectRolePage(&state);
        ftxui::Component enter_callsign_page = ql::BuildEnterCallsignPage(&state);
        ftxui::Component active_net_page = ql::BuildActiveNetPage(&state);
        ftxui::Component settings_page = ql::BuildSettingsPage(&state);
        ftxui::Component ad_hoc_net_page = ql::BuildAdHocNetPage(&state);
        ftxui::Component net_history_page = ql::BuildNetHistoryPage(&state);
        ftxui::Component edit_net_page = ql::BuildEditNetPage(&state);
        ftxui::Component import_net_page = ql::BuildImportNetPage(&state);
        ftxui::Component manage_users_page = ql::BuildManageUsersPage(&state);

        ftxui::Component tab = ftxui::Container::Tab(
            {
                net_list_page,
                create_net_page,
                select_role_page,
                enter_callsign_page,
                active_net_page,
                settings_page,
                ad_hoc_net_page,
                net_history_page,
                edit_net_page,
                import_net_page,
                manage_users_page,
            },
            &state.page);

        // Wrapping the whole Tab (rather than each page individually) matters:
        // Container::Tab only forwards keyboard events to its active child when
        // that child's subtree reports itself focusable, which fails whenever a
        // page's only widget is an empty list (e.g. no recurring nets yet). See
        // ql::AppKeyHandler for the full explanation. SafeAppEventDispatcher
        // wraps AppKeyHandler the same way ftxui::CatchEvent would, but also
        // guards against a Database exception (e.g. a write that times out
        // because another connection -- another session, or this one's own
        // background ULS import thread -- is mid-transaction) taking down the
        // whole session.
        ftxui::Component ui = ftxui::Make<ql::SafeAppEventDispatcher>(tab, &state);

        // Declared after `screen` so it is stopped and joined before `screen`
        // is destroyed.
        ClockTicker clock_ticker(&screen);

        screen.Loop(ui);
    }

}  // namespace ql
