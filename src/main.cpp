#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "interactive_session.hpp"
#include "ssh_server.hpp"

namespace
{

    constexpr int kDefaultSshPort = 2222;
    constexpr const char* kDbPath = "quicklogger.db";

    // Parsed once at startup; see the --ssh-port/--no-ssh/--headless doc
    // comments in README.md for what each one means to an operator.
    struct CliOptions
    {
        bool ssh_enabled = true;
        int ssh_port = kDefaultSshPort;
        bool headless = false;
    };

    CliOptions ParseArgs(int argc, char** argv)
    {
        CliOptions options;
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--no-ssh")
            {
                options.ssh_enabled = false;
            }
            else if (arg == "--headless")
            {
                options.headless = true;
            }
            else if (arg.rfind("--ssh-port=", 0) == 0)
            {
                options.ssh_port = std::atoi(arg.c_str() + std::strlen("--ssh-port="));
            }
        }
        return options;
    }

}  // namespace

int main(int argc, char** argv)
{
    // Must happen before any thread (a background ULS import, or a forked
    // SSH session's own threads) could call into libcurl. Done before the
    // SSH listener below forks, so every process it leads to inherits an
    // already-initialized libcurl.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    CliOptions options = ParseArgs(argc, argv);

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

    ql::RunInteractiveSession("settings.txt", /*is_console_session=*/true);

    ql::StopSshServerProcess(ssh_listener_pid);
    return 0;
}
