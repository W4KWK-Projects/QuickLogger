// What the built-in SSH server does with commands, end to end: the system's
// real ssh against a real QuickLogger server on this machine. It runs
// QuickLogger's own commands and nothing else: no shell, no programs. Run by
// tests/ssh_end_to_end.sh (the parsing alone is in test_remote_command.cpp);
// without QL_PUSH_E2E_DIR they do nothing.

#include <cstdlib>
#include <string>
#include <vector>

#include "../src/run_program.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // Runs `ssh -T user@127.0.0.1 <command words>`: no terminal, so a
    // shell is never asked for by a pty. An empty `command` asks for the
    // login shell.
    static ProgramResult SshCommand(const std::string& user, const std::string& command)
    {
        std::vector<std::string> arguments = {"-T", "-p", std::getenv("QL_PUSH_E2E_PORT"), user + "@127.0.0.1"};
        if (!command.empty())
        {
            arguments.push_back(command);
        }
        return RunProgram(FindProgramOnPath("ssh"), arguments, 60, nullptr);
    }

    static bool Contains(const ProgramResult& result, const std::string& text)
    {
        return result.output.find(text) != std::string::npos || result.errors.find(text) != std::string::npos;
    }

    QL_TEST(SshExecEndToEnd)
    {
        const char* dir_text = std::getenv("QL_PUSH_E2E_DIR");
        if (dir_text == nullptr)
        {
            return;
        }
        REQUIRE(std::getenv("QL_PUSH_E2E_PORT") != nullptr);
        REQUIRE(!FindProgramOnPath("ssh").empty());
        std::string dir = dir_text;

        // version: the interface's answer, exit status 0.
        ProgramResult result = SshCommand("W4KWK", "version");
        REQUIRE(result.started);
        CHECK(!result.stopped);
        CHECK_EQ(result.exit_status, 0);
        CHECK(result.output.rfind("QUICKLOGGER-RESULT 1\n", 0) == 0);
        CHECK(Contains(result, "status: ok\n"));
        CHECK(Contains(result, "version: "));

        // Anything else is refused as a result of the same form, with the
        // error exit status, and is never run: no listing of the server's
        // folder, no uid from id, no file made by touch.
        std::string made = dir + "/up/made-by-a-command";
        std::vector<std::string> refused = {"ls",
                                            "ls /",
                                            "id",
                                            "sh -c id",
                                            "bash",
                                            "cat /etc/passwd",
                                            "cat /etc/passwd; id",
                                            "version; touch " + made,
                                            "version && touch " + made,
                                            "version | cat",
                                            "touch " + made,
                                            "$(touch " + made + ")",
                                            "`touch " + made + "`",
                                            "version now",
                                            "import-session",
                                            "import-session --force x.qlsession",
                                            "import-session ../../secret.txt",
                                            "import-session /etc/passwd",
                                            "mosh-server"};
        for (const std::string& command : refused)
        {
            result = SshCommand("W4KWK", command);
            REQUIRE(result.started);
            CHECK(!result.stopped);
            CHECK(result.exit_status != 0);
            CHECK(result.exit_status != 255);
            CHECK(!Contains(result, "uid="));
            CHECK(!Contains(result, "root:"));
            CHECK(!Contains(result, "settings.txt"));
            CHECK(!FileExists(made));
            if (command.rfind("mosh-server", 0) != 0)
            {
                CHECK(result.output.rfind("QUICKLOGGER-RESULT 1\n", 0) == 0);
            }
        }

        // The refusal says what is run.
        result = SshCommand("W4KWK", "ls");
        CHECK_EQ(result.exit_status, 1);
        CHECK(Contains(result, "status: error\n"));
        CHECK(Contains(result, "Unknown command: ls"));

        // No command and no terminal: no shell, and no hang.
        result = SshCommand("W4KWK", "");
        REQUIRE(result.started);
        CHECK(!result.stopped);
        CHECK(result.exit_status != 0);
        CHECK(!Contains(result, "uid="));

        // A view-only user is refused every command, version too.
        result = SshCommand("K4VIEW", "version");
        CHECK(!result.stopped);
        CHECK_EQ(result.exit_status, 4);
        CHECK(Contains(result, "status: refused\n"));
        result = SshCommand("K4VIEW", "ls");
        CHECK_EQ(result.exit_status, 4);
        CHECK(!Contains(result, "settings.txt"));
        CHECK(!FileExists(made));
    }

}  // namespace ql
