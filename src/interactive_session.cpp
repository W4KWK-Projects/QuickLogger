#include "interactive_session.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "date_utils.hpp"
#include "db/database.hpp"
#include "settings.hpp"
#include "uls_import.hpp"
#include "ui/app_state.hpp"
#include "ui/chrome.hpp"
#include "ui/handlers.hpp"
#include "ui/pages.hpp"

namespace ql
{

    // Posted to the UI thread by ScreenTicker when the watched session's
    // check-ins have changed: see RefreshActiveCheckInsFromOthers.
    class RefreshCheckInsTask
    {
    public:
        RefreshCheckInsTask(AppState* state, std::int64_t instance_id)
            : state_(state), instance_id_(instance_id)
        {
        }

        void operator()() const
        {
            RefreshActiveCheckInsFromOthers(state_, instance_id_);
        }

    private:
        AppState* state_;
        std::int64_t instance_id_;
    };

    // Posted to the UI thread by ScreenTicker when which nets have a
    // session open has changed while the net list is showing.
    class RefreshNetListTask
    {
    public:
        explicit RefreshNetListTask(AppState* state) : state_(state) {}

        void operator()() const
        {
            if (!state_->showing_net_list)
            {
                return;  // Moved on since; it's reloaded when next needed.
            }
            RefreshNets(state_);
            if (state_->screen != nullptr)
            {
                state_->screen->PostEvent(ftxui::Event::Custom);
            }
        }

    private:
        AppState* state_;
    };

    // Posted to the UI thread by ScreenTicker when the watched session is no
    // longer open: someone else closed or deleted it.
    class SessionClosedTask
    {
    public:
        SessionClosedTask(AppState* state, std::int64_t instance_id)
            : state_(state), instance_id_(instance_id)
        {
        }

        void operator()() const
        {
            // Already gone from it (e.g. it was closed from here), or told.
            if (state_->watched_instance_id != instance_id_)
            {
                return;
            }
            if (!EnsureActiveSessionOpen(state_, "") && state_->screen != nullptr)
            {
                state_->screen->PostEvent(ftxui::Event::Custom);
            }
        }

    private:
        AppState* state_;
        std::int64_t instance_id_;
    };

    // Redraws the screen when -- and only when -- something it shows has
    // changed on its own, without a keypress: the top bar's clock ticking
    // over to a new minute, the shared station data's status (see
    // DescribeStationDataNotice / DescribeStationDataStatus) moving on, or
    // someone else logging (or deleting) a check-in in the session the
    // active net page is showing, or closing that session, or opening or
    // closing one while the net list is showing -- those checked every
    // kCheckInPoll. FTXUI only redraws in response to an event, and every
    // redraw costs an SSH session a screenful of bytes on the wire, so this
    // posts one only when the visible text would actually differ.
    //
    // It wakes every kCheckInPoll, or at the next minute boundary if that
    // comes first; the station data's status is checked every
    // kStatusPollIdle normally, every kStatusPollBusy while it's loading (so
    // its percentage keeps moving). Waking and checking costs nothing but a
    // local database read, and the short waits mean a stepped system clock
    // or a resume from suspend is noticed promptly. Construct it after the
    // screen and before screen.Loop(); it stops and joins on destruction.
    class ScreenTicker
    {
    public:
        ScreenTicker(ftxui::ScreenInteractive* screen, AppState* state, std::string db_path)
            : screen_(screen),
              state_(state),
              db_path_(std::move(db_path)),
              thread_(&ScreenTicker::Run, this)
        {
        }

        ~ScreenTicker()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_ = true;
            }
            wake_.notify_all();
            thread_.join();
        }

        ScreenTicker(const ScreenTicker&) = delete;
        ScreenTicker& operator=(const ScreenTicker&) = delete;

    private:
        static std::int64_t Now()
        {
            return static_cast<std::int64_t>(std::time(nullptr));
        }

        // Everything about the station data that's shown anywhere, as one
        // string to compare; empty if the database can't be read.
        static std::string StatusSignature(Database* db, bool* busy)
        {
            *busy = false;
            if (db == nullptr)
            {
                return "";
            }
            try
            {
                std::int64_t now = Now();
                bool is_problem = false;
                std::string notice = DescribeStationDataNotice(db, now, &is_problem);
                *busy = !notice.empty();
                return notice + "|" + DescribeStationDataStatus(db, now, true);
            }
            catch (const std::exception&)
            {
                return "";
            }
        }

        void Run()
        {
            // This thread's own connection: SQLite connections aren't shared
            // across threads here, and the UI thread's is busy with the UI.
            std::unique_ptr<Database> db;
            try
            {
                db = std::make_unique<Database>(db_path_);
            }
            // NOLINTNEXTLINE(bugprone-empty-catch): deliberately ignored, see below.
            catch (const std::exception&)
            {
                // No status watching, just the clock.
            }

            std::int64_t drawn_minute = Now() / 60;
            bool busy = false;
            std::string drawn_status = StatusSignature(db.get(), &busy);
            std::chrono::system_clock::time_point next_status_check =
                std::chrono::system_clock::now() + kStatusPollIdle;
            while (true)
            {
                std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
                std::chrono::system_clock::time_point next_minute =
                    std::chrono::floor<std::chrono::minutes>(now) + std::chrono::minutes(1);
                // The margin keeps a wake-up that lands a hair early from
                // missing the minute change.
                std::chrono::system_clock::duration wait_time =
                    next_minute - now + std::chrono::milliseconds(200);
                // Every kCheckInPoll whatever the page, so a session or list
                // just opened is watched from the start, not only once a
                // longer wait begun on another page is over.
                if (wait_time > kCheckInPoll)
                {
                    wait_time = kCheckInPoll;
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

                bool changed = false;
                std::int64_t minute = Now() / 60;
                if (minute != drawn_minute)
                {
                    drawn_minute = minute;
                    changed = true;
                }
                if (std::chrono::system_clock::now() >= next_status_check)
                {
                    std::string status = StatusSignature(db.get(), &busy);
                    if (status != drawn_status)
                    {
                        drawn_status = status;
                        changed = true;
                    }
                    next_status_check = std::chrono::system_clock::now() +
                                        (busy ? kStatusPollBusy : kStatusPollIdle);
                }
                if (changed)
                {
                    screen_->PostEvent(ftxui::Event::Custom);
                }
                CheckWatchedSession(db.get());
                CheckOpenNets(db.get());
            }
        }

        // While the net list is showing, asks the UI thread to reload it if
        // a net's session has been opened or closed (by anyone) since it
        // was last loaded, so its "session open" labels stay current.
        void CheckOpenNets(Database* db)
        {
            if (db == nullptr || !state_->showing_net_list)
            {
                return;
            }
            std::vector<std::int64_t> open_net_ids;
            try
            {
                open_net_ids = db->GetNetIdsWithOpenInstances();
            }
            catch (const std::exception&)
            {
                return;  // Busy right now; try again next time.
            }
            std::sort(open_net_ids.begin(), open_net_ids.end());
            if (open_net_ids != shown_open_net_ids_)
            {
                shown_open_net_ids_ = open_net_ids;
                screen_->Post(RefreshNetListTask(state_));
            }
        }

        // If the active net page's session has check-ins it isn't showing
        // yet (or is showing ones since deleted), asks the UI thread to
        // reload them; it redraws only then. If someone else has closed or
        // deleted the session, asks it to say so instead.
        void CheckWatchedSession(Database* db)
        {
            std::int64_t instance_id = state_->watched_instance_id;
            if (db == nullptr || instance_id == 0)
            {
                return;
            }
            std::int64_t count = 0;
            std::int64_t newest_id = 0;
            try
            {
                std::optional<NetInstance> session = db->GetNetInstanceById(instance_id);
                if (!session.has_value() || session->status != NetInstanceStatus::kOpen)
                {
                    screen_->Post(SessionClosedTask(state_, instance_id));
                    return;
                }
                db->GetCheckInSummary(instance_id, &count, &newest_id);
            }
            catch (const std::exception&)
            {
                return;  // Busy right now; try again next time.
            }
            if (count != state_->shown_check_in_count ||
                newest_id != state_->shown_newest_check_in_id)
            {
                screen_->Post(RefreshCheckInsTask(state_, instance_id));
            }
        }

        static constexpr std::chrono::seconds kStatusPollIdle{10};
        static constexpr std::chrono::seconds kStatusPollBusy{3};
        static constexpr std::chrono::seconds kCheckInPoll{3};

        ftxui::ScreenInteractive* screen_;
        AppState* state_;
        std::string db_path_;
        std::mutex mutex_;
        std::condition_variable wake_;
        bool stop_ = false;
        // The open nets the list was last reloaded for (sorted); the list
        // is loaded before the ticker starts, so it starts out as unknown.
        std::vector<std::int64_t> shown_open_net_ids_{-1};
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
        ql::SetUse24HourClock(state.settings.use_24_hour_clock);

        // First launch (or an upgrade from before the ZIP code became required):
        // force the operator through Settings before anything else. Mirrors
        // ShowSettingsPageHandler's own state setup since that handler isn't
        // reachable yet -- there's no net list to press F4 from until this is
        // done. CancelSettingsHandler refuses to leave kPageSettings while
        // SettingsAreComplete is still false, so Esc can't bypass this.
        if (!ql::SettingsAreComplete(state.settings))
        {
            ql::OpenSettingsForm(&state);
            state.page = ql::kPageSettings;
        }

        ql::RefreshNets(&state);

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
        // because another connection -- another session, or the station data
        // updater -- is mid-transaction) taking down the whole session.
        // Help and the seldom-used windows open over whichever page is up.
        ftxui::Component with_info_window =
            ftxui::Modal(tab, ql::BuildInfoWindow(&state), &state.show_info_window);
        ftxui::Component ui = ftxui::Make<ql::SafeAppEventDispatcher>(with_info_window, &state);

        // The top bar's station-data notice reads this session's database.
        SetTopBarNoticeDatabase(&db);

        // Declared after `screen` so it is stopped and joined before `screen`
        // is destroyed.
        ScreenTicker screen_ticker(&screen, &state, state.db_path);

        screen.Loop(ui);
    }

}  // namespace ql
