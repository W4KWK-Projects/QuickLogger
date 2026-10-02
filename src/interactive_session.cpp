#include "interactive_session.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <exception>
#include <cstdio>
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
#include "update_check.hpp"
#include "version.hpp"
#include "ui/app_state.hpp"
#include "ui/chrome.hpp"
#include "ui/frame_writer.hpp"
#include "ui/handlers.hpp"
#include "ui/mouse.hpp"
#include "ui/pages.hpp"
#include "ui/push_runner.hpp"

namespace ql
{

    // Posted to the UI thread by ScreenTicker when the watched session's
    // check-ins have changed: see RefreshActiveCheckInsFromOthers.
    class RefreshCheckInsTask
    {
    public:
        RefreshCheckInsTask(AppState* state, std::int64_t instance_id) : state_(state), instance_id_(instance_id) {}

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
        SessionClosedTask(AppState* state, std::int64_t instance_id) : state_(state), instance_id_(instance_id) {}

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
            : screen_(screen), state_(state), db_path_(std::move(db_path)), thread_(&ScreenTicker::Run, this)
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
                return notice + "|" + DescribeStationDataStatus(db, now);
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
                std::chrono::system_clock::duration wait_time = next_minute - now + std::chrono::milliseconds(200);
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

                // This tick's reads under one snapshot and one lock (every
                // few seconds, for every session on a server). Never let a
                // failure end this thread: without it, the reads just run
                // separately.
                std::unique_ptr<Database::ReadTransaction> reads;
                if (db != nullptr)
                {
                    try
                    {
                        reads = std::make_unique<Database::ReadTransaction>(db.get());
                    }
                    catch (const std::exception&)
                    {
                        reads.reset();
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
                    next_status_check = std::chrono::system_clock::now() + (busy ? kStatusPollBusy : kStatusPollIdle);
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
            if (count != state_->shown_check_in_count || newest_id != state_->shown_newest_check_in_id)
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

    // At the console, checks GitHub for a newer release (see
    // update_check.hpp): shortly after starting, then every
    // kUpdateCheckInterval, sooner again after a failure. Never while
    // Settings' Update Check is off. A redraw shows what it finds.
    class UpdateChecker
    {
    public:
        explicit UpdateChecker(ftxui::ScreenInteractive* screen) : screen_(screen), thread_(&UpdateChecker::Run, this)
        {
        }

        ~UpdateChecker()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_ = true;
            }
            wake_.notify_all();
            thread_.join();
        }

        UpdateChecker(const UpdateChecker&) = delete;
        UpdateChecker& operator=(const UpdateChecker&) = delete;

    private:
        static constexpr std::chrono::seconds kFirstCheckDelay{20};
        static constexpr std::chrono::hours kUpdateCheckInterval{6};
        static constexpr std::chrono::hours kRetryAfterFailure{1};
        // While the check is off, how often to see whether it's been
        // turned on.
        static constexpr std::chrono::minutes kOffPoll{1};

        // Waits `how_long`; false if it's time to stop.
        bool Wait(std::chrono::system_clock::duration how_long)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, how_long, StopRequested(this));
            return !stop_;
        }

        class StopRequested
        {
        public:
            explicit StopRequested(const UpdateChecker* checker) : checker_(checker) {}
            bool operator()() const
            {
                return checker_->stop_;
            }

        private:
            const UpdateChecker* checker_;
        };

        void Run()
        {
            if (!Wait(kFirstCheckDelay))
            {
                return;
            }
            while (true)
            {
                std::chrono::system_clock::duration next = kOffPoll;
                if (UpdateCheckEnabled())
                {
                    std::string version;
                    std::string error;
                    if (FetchLatestReleaseVersion(kLatestReleaseUrl, &version, &error))
                    {
                        std::string found = IsNewerVersion(version, QuickLoggerVersion()) ? version : "";
                        if (found != AvailableUpdate())
                        {
                            SetAvailableUpdate(found);
                            screen_->PostEvent(ftxui::Event::Custom);
                        }
                        next = kUpdateCheckInterval;
                    }
                    else
                    {
                        next = kRetryAfterFailure;
                    }
                }
                if (!Wait(next))
                {
                    return;
                }
            }
        }

        ftxui::ScreenInteractive* screen_;
        std::mutex mutex_;
        std::condition_variable wake_;
        bool stop_ = false;
        std::thread thread_;
    };

    void RunInteractiveSession(const std::string& settings_path, bool is_console_session,
                               const std::string& ssh_username, bool over_mosh)
    {
        // Each frame goes to the terminal in one write. Standard output to a
        // terminal is otherwise line-buffered, and FTXUI ends every screen
        // line with a newline, so a frame went out as one write per line --
        // over SSH, as many small encrypted packets. FTXUI flushes after
        // every frame and every terminal mode change, so nothing waits.
        static char output_buffer[64 * 1024];
        std::fflush(stdout);
        std::setvbuf(stdout, output_buffer, _IOFBF, sizeof(output_buffer));

        // Sends only what changed from one frame to the next (see
        // frame_writer.hpp), rather than the whole screen every time.
        ql::FrameWriter frame_writer;
        ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
        ql::InstallFrameWriter(&screen, &frame_writer);
        ql::Database db("quicklogger.db");

        ql::AppState state;
        state.db = &db;
        state.db_path = "quicklogger.db";
        state.screen = &screen;
        state.is_console_session = is_console_session;
        state.over_mosh = over_mosh;
        // Decided once, at login: a change in Manage Users applies from
        // the user's next login.
        state.ssh_username = is_console_session ? std::string() : ssh_username;
        state.view_only_user = !is_console_session && !ssh_username.empty() && db.IsUserViewOnly(ssh_username);
        state.settings_path = settings_path;
        state.settings = ql::LoadSettings(state.settings_path);
        // An SSH user's call signs are set in Manage Users, not theirs to
        // change.
        if (!state.ssh_username.empty())
        {
            std::vector<ql::User> keys = db.GetUserKeys(state.ssh_username);
            if (!keys.empty())
            {
                state.callsign_editable = false;
                state.settings_focus = 1;
                state.settings.callsign = keys[0].amateur_callsign;
                state.settings.gmrs_callsign = keys[0].gmrs_callsign;
            }
        }
        ql::SetUse24HourClock(state.settings.use_24_hour_clock);
        // Only the console checks for updates: an SSH user can't install one.
        ql::SetUpdateCheckEnabled(is_console_session && state.settings.check_for_updates);

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
        ql::HighlightLastLoggedNet(&state);

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
        ftxui::Component with_confirm_prompt =
            ql::LayeredModal(tab, ql::BuildConfirmPrompt(&state), &state.show_confirm_prompt);
        ftxui::Component with_info_window =
            ql::LayeredModal(with_confirm_prompt, ql::BuildInfoWindow(&state), &state.show_info_window);
        ftxui::Component ui = ftxui::Make<ql::SafeAppEventDispatcher>(with_info_window, &state);

        // The top bar's station-data notice reads this session's database.
        SetTopBarNoticeDatabase(&db);

        // Declared after `screen` so it is stopped and joined before `screen`
        // is destroyed.
        ScreenTicker screen_ticker(&screen, &state, state.db_path);
        std::unique_ptr<UpdateChecker> update_checker;
        if (is_console_session)
        {
            update_checker = std::make_unique<UpdateChecker>(&screen);
        }
        // At the console: pushing sessions upstream, off this thread.
        std::unique_ptr<PushRunner> push_runner;
        if (is_console_session)
        {
            push_runner = std::make_unique<PushRunner>(&screen, &state);
            state.push_runner = push_runner.get();
        }

        screen.Loop(ui);
        // An SSH session's process ends with _exit, which doesn't flush.
        std::fflush(stdout);
    }

}  // namespace ql
