#include "data_updater.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <memory>
#include <utility>
#else
#include <csignal>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "db/database.hpp"
#include "models.hpp"
#include "uls_import.hpp"

#if !defined(_WIN32)
namespace ql
{
    static void RequestUpdaterStop();
}

// Signal handlers need C linkage (clang-tidy bugprone-signal-handler), which a
// function inside a namespace can't have -- so this one lives out here and
// only forwards.
extern "C"
{
    static void QuickLoggerUpdaterStopSignal(int /*signal_number*/)
    {
        ql::RequestUpdaterStop();
    }
}
#endif

namespace ql
{

    // How often the updater looks at the database to see whether anything
    // is due (or has been requested). A local SQLite read -- no network
    // traffic, and nothing sent to any session.
    static constexpr int kPollSeconds = 5;

    // Set by StopDataUpdater (Windows) or the SIGTERM/SIGINT handler
    // (elsewhere) to make the updater wind down. A lock-free atomic, so it's
    // safe to set from a signal handler.
    static std::atomic<bool> g_stop_requested{false};

#if !defined(_WIN32)
    // The process that started the updater; when it's gone (getppid() stops
    // returning it), the app has quit and the updater should too.
    static pid_t g_parent_pid = -1;
    // The updater process, as seen from the process that started it.
    static pid_t g_updater_pid = -1;
#else
    static std::unique_ptr<std::thread> g_updater_thread;
#endif

    static bool ShouldStop()
    {
#if !defined(_WIN32)
        if (g_parent_pid > 0 && ::getppid() != g_parent_pid)
        {
            g_stop_requested.store(true);
        }
#endif
        return g_stop_requested.load();
    }

    static std::int64_t Now()
    {
        return static_cast<std::int64_t>(std::time(nullptr));
    }

    // Sleeps up to `seconds`, waking early if a stop is requested.
    static void SleepUnlessStopped(int seconds)
    {
        for (int i = 0; i < seconds * 4 && !ShouldStop(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    // Claims and runs a refresh if one is due. Returns whether one ran.
    static bool RunRefreshIfDue(Database* db, const std::string& db_path)
    {
        // Planned before claiming: claiming stamps the job's started_at,
        // which is what marks a hand-made request as taken care of, so a
        // plan made afterwards would no longer see the request.
        DataRefreshPlan plan = PlanDataRefresh(db, Now());
        if (!DataRefreshPlanHasWork(plan))
        {
            return false;
        }
        if (!db->TryClaimImportRun(kDataRefreshJob, Now(), kJobStaleAfterSeconds))
        {
            // Another QuickLogger instance on this database is on it.
            return false;
        }

        ImportRunStatus job;
        job.source = kDataRefreshJob;
        job.started_at = Now();
        try
        {
            job.status = RunDataRefresh(db, db_path, plan, ShouldStop, DefaultDataSources());
        }
        catch (const std::exception& e)
        {
            job.status = "failed";
            job.last_error = e.what();
        }
        job.completed_at = Now();
        job.percent = 100;
        db->UpsertImportRunStatus(job);
        return true;
    }

#if !defined(_WIN32)

    // Whether a refresh is due, from a database connection that's closed
    // again before this returns -- the caller may fork next (THE FORK RULE,
    // ssh_server.hpp).
    static bool IsRefreshDue(const std::string& db_path)
    {
        Database db(db_path);
        return DataRefreshPlanHasWork(PlanDataRefresh(&db, Now()));
    }

    // Runs one refresh in a child process, and waits for it. A refresh needs
    // a few hundred MB for a minute (the whole FCC license list in memory);
    // done in a process that then exits, every byte goes back to the system
    // at once, and the updater itself stays at a couple of MB for the week
    // until the next one. (Freeing it in-process isn't enough: the C
    // library keeps freed memory reserved for reuse -- measured on macOS at
    // ~250 MB still held after a refresh.) A stop request is passed on to
    // the child, which winds down and records the run as interrupted.
    static void RunRefreshInChildProcess(const std::string& db_path)
    {
        pid_t updater_pid = ::getpid();
        pid_t pid = fork();
        if (pid < 0)
        {
            std::fprintf(stderr, "Station data updater: could not start a refresh.\n");
            return;
        }
        if (pid == 0)
        {
            // Stop if the updater goes away, just as the updater stops if
            // the app does.
            g_parent_pid = updater_pid;
            int exit_code = 0;
            try
            {
                Database db(db_path);
                RunRefreshIfDue(&db, db_path);
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "Station data updater: %s\n", e.what());
                exit_code = 1;
            }
            _exit(exit_code);
        }

        bool stop_sent = false;
        while (::waitpid(pid, nullptr, WNOHANG) == 0)
        {
            if (ShouldStop() && !stop_sent)
            {
                ::kill(pid, SIGTERM);
                stop_sent = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    static void RunUpdaterLoop(const std::string& db_path)
    {
        while (!ShouldStop())
        {
            try
            {
                if (IsRefreshDue(db_path))
                {
                    RunRefreshInChildProcess(db_path);
                }
                SleepUnlessStopped(kPollSeconds);
            }
            catch (const std::exception& e)
            {
                // Most likely the database was briefly unavailable; the run
                // (if any) is left for the next attempt to pick up.
                std::fprintf(stderr, "Station data updater: %s\n", e.what());
                SleepUnlessStopped(60);
            }
        }
    }

#else  // _WIN32

    // No fork() on Windows: the refresh runs right here in the updater
    // thread, and the memory it used stays with the process afterward.
    static void RunUpdaterLoop(const std::string& db_path)
    {
        while (!ShouldStop())
        {
            try
            {
                Database db(db_path);
                while (!ShouldStop())
                {
                    RunRefreshIfDue(&db, db_path);
                    SleepUnlessStopped(kPollSeconds);
                }
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "Station data updater: %s\n", e.what());
                SleepUnlessStopped(60);
            }
        }
    }

#endif

#if !defined(_WIN32)

    static void RequestUpdaterStop()
    {
        g_stop_requested.store(true);
    }

    void StartDataUpdater(const std::string& db_path)
    {
        if (g_updater_pid > 0)
        {
            return;
        }
        pid_t parent_pid = ::getpid();
        pid_t pid = fork();
        if (pid < 0)
        {
            std::fprintf(stderr, "Could not start the station data updater.\n");
            return;
        }
        if (pid == 0)
        {
            g_parent_pid = parent_pid;
            std::signal(SIGTERM, QuickLoggerUpdaterStopSignal);
            // Ctrl-C at the console reaches every process in the terminal's
            // process group, this one included; wind down cleanly too.
            std::signal(SIGINT, QuickLoggerUpdaterStopSignal);
            RunUpdaterLoop(db_path);
            _exit(0);
        }
        g_updater_pid = pid;
    }

    void StopDataUpdater()
    {
        if (g_updater_pid <= 0)
        {
            return;
        }
        ::kill(g_updater_pid, SIGTERM);
        // Give an interrupted refresh a few seconds to record itself, then
        // stop waiting.
        for (int i = 0; i < 40; ++i)
        {
            if (::waitpid(g_updater_pid, nullptr, WNOHANG) != 0)
            {
                g_updater_pid = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        ::kill(g_updater_pid, SIGKILL);
        ::waitpid(g_updater_pid, nullptr, 0);
        g_updater_pid = -1;
    }

#else  // _WIN32

    // std::thread needs a callable; a named class rather than a lambda, per
    // this codebase's convention.
    class UpdaterThreadMain
    {
    public:
        explicit UpdaterThreadMain(std::string db_path) : db_path_(std::move(db_path)) {}

        void operator()() const
        {
            RunUpdaterLoop(db_path_);
        }

    private:
        std::string db_path_;
    };

    void StartDataUpdater(const std::string& db_path)
    {
        if (g_updater_thread)
        {
            return;
        }
        g_stop_requested.store(false);
        g_updater_thread = std::make_unique<std::thread>(UpdaterThreadMain(db_path));
    }

    void StopDataUpdater()
    {
        if (!g_updater_thread)
        {
            return;
        }
        g_stop_requested.store(true);
        g_updater_thread->join();
        g_updater_thread.reset();
    }

#endif

}  // namespace ql
