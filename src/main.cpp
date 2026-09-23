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
    // Must happen before any thread (a background ULS import, or the SSH
    // listener's own accept-loop thread and every connection it forks)
    // could call into libcurl.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    CliOptions options = ParseArgs(argc, argv);

    if (options.ssh_enabled)
    {
        ql::StartSshServer(kDbPath, options.ssh_port);
    }

    if (options.headless)
    {
        // No local console session -- just keep the process (and the
        // detached SSH accept-loop thread it started above) alive forever.
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }

    ql::RunInteractiveSession("settings.txt", /*is_console_session=*/true);
    return 0;
}
