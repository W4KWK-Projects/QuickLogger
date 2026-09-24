#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>

#include "data_updater.hpp"
#include "interactive_session.hpp"
#include "version.hpp"

// The built-in SSH server is an optional part of the build (CMake option
// QUICKLOGGER_ENABLE_SSH, off by default on Windows, where it can't work: it
// is built on fork() and ptys).
#ifdef QUICKLOGGER_WITH_SSH
#include "ssh_server.hpp"
#endif

constexpr int kDefaultSshPort = 2222;
constexpr std::string_view kSshPortFlag = "--ssh-port=";
constexpr const char* kDbPath = "quicklogger.db";

#ifdef QUICKLOGGER_WITH_SSH
constexpr bool kSshCompiledIn = true;
#else
constexpr bool kSshCompiledIn = false;
#endif

// Parsed once at startup; see the --ssh-port/--no-ssh/--headless doc
// comments in README.md for what each one means to an operator.
struct CliOptions
{
    bool ssh_enabled = kSshCompiledIn;
    int ssh_port = kDefaultSshPort;
    bool headless = false;
};

static CliOptions ParseArgs(int argc, char** argv)
{
    CliOptions options;
    for (int i = 1; i < argc; ++i)
    {
        // A view, not a std::string: nothing here needs a copy of argv.
        std::string_view arg = argv[i];
        if (arg == "--no-ssh")
        {
            options.ssh_enabled = false;
        }
        else if (arg == "--headless")
        {
            options.headless = true;
        }
        else if (arg.substr(0, kSshPortFlag.size()) == kSshPortFlag)
        {
            // argv strings are NUL-terminated, so this is too.
            const char* value = argv[i] + kSshPortFlag.size();
            char* end = nullptr;
            long port = std::strtol(value, &end, 10);
            if (end == value || *end != '\0' || port < 1 || port > 65535)
            {
                std::fprintf(stderr, "Ignoring invalid --ssh-port value \"%s\".\n", value);
            }
            else
            {
                options.ssh_port = static_cast<int>(port);
            }
        }
    }
    return options;
}

static int RunQuickLogger(int argc, char** argv)
{
    // Must happen before any thread (a background ULS import, or a forked
    // SSH session's own threads) could call into libcurl. Done before the
    // SSH listener below forks, so every process it leads to inherits an
    // already-initialized libcurl.
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--version")
        {
            std::printf("QuickLogger %s\n", ql::QuickLoggerVersion());
            return 0;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    CliOptions options = ParseArgs(argc, argv);

    // Keeps the shared station data loaded and current for every session
    // (see data_updater.hpp). Like the SSH listener below, started before
    // this process opens the database or starts any thread.
    ql::StartDataUpdater(kDbPath);

#ifdef QUICKLOGGER_WITH_SSH
    if (options.headless)
    {
        // No local console session, so nothing in this process ever opens
        // the database -- the accept loop can safely run right here and
        // fork its connections directly.
        if (options.ssh_enabled)
        {
            ql::RunSshServer(kDbPath, options.ssh_port);
        }
        // Reached only with SSH disabled (nothing to serve) or if the
        // listener couldn't start (already logged) -- keep the process alive
        // rather than exiting, as before.
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }

    // The listener must be its own process, forked here -- before this
    // process opens the database or starts any thread -- so the SSH
    // connections it forks never inherit this process's SQLite state (see
    // THE FORK RULE in ssh_server.hpp).
    pid_t ssh_listener_pid = -1;
    if (options.ssh_enabled)
    {
        ssh_listener_pid = ql::StartSshServerProcess(kDbPath, options.ssh_port);
    }
#else
    if (options.headless)
    {
        std::fprintf(stderr,
                     "--headless needs the built-in SSH server, which this build of "
                     "QuickLogger doesn't include.\n");
        return 2;
    }
#endif

    // Anything thrown this far up (e.g. the database file can't be opened
    // or created) ends the session; report it in plain words rather than
    // letting std::terminate abort with a core dump.
    int exit_code = 0;
    try
    {
        ql::RunInteractiveSession("settings.txt", /*is_console_session=*/true);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "QuickLogger stopped: %s\n", e.what());
        exit_code = 1;
    }

#ifdef QUICKLOGGER_WITH_SSH
    ql::StopSshServerProcess(ssh_listener_pid);
#endif
    ql::StopDataUpdater();
    return exit_code;
}

int main(int argc, char** argv)
{
    // Last-resort guard so nothing escapes main() (which would end in
    // std::terminate and an abort instead of a readable message).
    try
    {
        return RunQuickLogger(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "QuickLogger stopped: %s\n", e.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "QuickLogger stopped because of an unexpected error.\n");
    }
    return 1;
}
