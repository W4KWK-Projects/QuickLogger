// What the built-in SSH server does with commands, end to end: the system's
// real ssh against a real QuickLogger server on this machine. It runs
// QuickLogger's own commands and nothing else: no shell, no programs. Run by
// tests/ssh_end_to_end.sh (the parsing alone is in test_remote_command.cpp);
// without QL_PUSH_E2E_DIR they do nothing.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/public_key.hpp"
#include "../src/run_program.hpp"
#include "../src/ui/app_state.hpp"
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

        // A view-only user may read (version, list-nets) but not import, and
        // gets no shell either.
        result = SshCommand("K4VIEW", "version");
        CHECK(!result.stopped);
        CHECK_EQ(result.exit_status, 0);
        CHECK(Contains(result, "status: ok\n"));
        result = SshCommand("K4VIEW", "list-nets");
        CHECK_EQ(result.exit_status, 0);
        result = SshCommand("K4VIEW", "import-session Sky.qlsession");
        CHECK_EQ(result.exit_status, 4);
        CHECK(Contains(result, "status: refused\n"));
        result = SshCommand("K4VIEW", "ls");
        CHECK_EQ(result.exit_status, 1);
        CHECK(!Contains(result, "settings.txt"));
        CHECK(!FileExists(made));
    }

    // The real ssh with one key and nothing else: no config, no agent, no
    // other identity that could log in in its place.
    static ProgramResult SshWithKey(const std::string& user, const std::string& key_file, const std::string& command)
    {
        std::string dir = std::getenv("QL_PUSH_E2E_DIR");
        std::vector<std::string> arguments = {"-F",
                                              "/dev/null",
                                              "-T",
                                              "-o",
                                              "BatchMode=yes",
                                              "-o",
                                              "IdentitiesOnly=yes",
                                              "-o",
                                              "IdentityAgent=none",
                                              "-o",
                                              "IdentityFile=" + key_file,
                                              "-o",
                                              "UserKnownHostsFile=" + dir + "/ssh/known_hosts",
                                              "-o",
                                              "GlobalKnownHostsFile=/dev/null",
                                              "-o",
                                              "StrictHostKeyChecking=yes",
                                              "-p",
                                              std::getenv("QL_PUSH_E2E_PORT"),
                                              user + "@127.0.0.1",
                                              command};
        return RunProgram("/usr/bin/ssh", arguments, 60, nullptr);
    }

    // A key made here, with its public line as the .pub file has it.
    static std::string MakeKey(const std::string& file, const std::string& comment)
    {
        ProgramResult made = RunProgram(FindProgramOnPath("ssh-keygen"),
                                        {"-q", "-t", "ed25519", "-N", "", "-C", comment, "-f", file}, 60, nullptr);
        REQUIRE(made.started);
        REQUIRE(made.exit_status == 0);
        return ReadTextFile(file + ".pub");
    }

    // A key added in My Keys is a key the real server lets in; one never
    // added, or removed again, isn't. Goes through the same AddMyKey and
    // RemoveMyKey the screen uses, on the server's own database.
    QL_TEST(KeysAddedInMyKeysWorkOverSsh)
    {
        const char* dir_text = std::getenv("QL_PUSH_E2E_DIR");
        if (dir_text == nullptr)
        {
            return;
        }
        REQUIRE(std::getenv("QL_PUSH_E2E_PORT") != nullptr);
        REQUIRE(!FindProgramOnPath("ssh-keygen").empty());
        REQUIRE(FileExists("/usr/bin/ssh"));
        std::string dir = dir_text;
        std::string key_dir = dir + "/added-keys";
        std::filesystem::create_directories(key_dir);
        std::string added_file = key_dir + "/added";
        std::string other_file = key_dir + "/never-added";
        std::string added_line = MakeKey(added_file, "laptop key");
        MakeKey(other_file, "stranger");

        Database db(dir + "/up/quicklogger.db");
        db.SetServerOption(kOptionSelfServiceKeys, true);
        AppState state;
        state.db = &db;
        state.db_path = dir + "/up/quicklogger.db";
        state.is_console_session = false;
        state.ssh_username = "W4KWK";
        state.ssh_key_id = db.GetUserKeys("W4KWK")[0].id;
        OpenMyKeys(&state);
        REQUIRE(state.my_keys_self_service);

        // Neither key is in yet.
        ProgramResult result = SshWithKey("W4KWK", added_file, "version");
        CHECK_EQ(result.exit_status, 255);

        // Pasted the way a terminal may hand it over: blank space around it.
        state.my_key_new_text = "  \n" + added_line + "  \r\n";
        AddMyKey(&state);
        CHECK(state.form_error.empty());
        CHECK_EQ(db.GetUserKeys("W4KWK").size(), std::size_t{2});

        // The new key logs in, as its owner and nobody else.
        result = SshWithKey("W4KWK", added_file, "version");
        REQUIRE(result.started);
        CHECK_EQ(result.exit_status, 0);
        CHECK(Contains(result, "status: ok\n"));
        CHECK_EQ(SshWithKey("K4VIEW", added_file, "version").exit_status, 255);
        // A key that wasn't added still doesn't.
        CHECK_EQ(SshWithKey("W4KWK", other_file, "version").exit_status, 255);

        // Removed with F3 twice, it stops working; the first key still does.
        for (std::size_t i = 0; i < state.my_keys.size(); ++i)
        {
            if (SamePublicKey(state.my_keys[i].public_key, added_line))
            {
                state.selected_my_key_index = static_cast<int>(i);
            }
        }
        RemoveMyKey(&state);
        RemoveMyKey(&state);
        CHECK(state.form_error.empty());
        CHECK_EQ(db.GetUserKeys("W4KWK").size(), std::size_t{1});
        CHECK_EQ(SshWithKey("W4KWK", added_file, "version").exit_status, 255);
        result = SshCommand("W4KWK", "version");
        CHECK_EQ(result.exit_status, 0);

        db.SetServerOption(kOptionSelfServiceKeys, false);
    }

}  // namespace ql
