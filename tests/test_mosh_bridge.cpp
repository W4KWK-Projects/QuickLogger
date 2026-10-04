// Mosh on the SSH server (mosh_bridge.hpp): reading a mosh client's request,
// the command run for it, the one-time login token, and no ZMODEM over Mosh.

#include <filesystem>
#include <string>
#include <vector>

#include "../src/mosh_bridge.hpp"
#include "../src/ui/app_state.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // What mosh 1.4 sends, quoted as it quotes it.
    static const char* const kClientLine =
        "mosh-server 'new' '-c' '256' '-s' '-l' 'LANG=en_US.UTF-8' '-l' 'LC_ALL=en_US.UTF-8'";

    QL_TEST(MoshClientsAreRecognized)
    {
        CHECK(IsMoshServerCommandLine(kClientLine));
        CHECK(IsMoshServerCommandLine("/usr/local/bin/mosh-server 'new'"));
        CHECK(
            IsMoshServerCommandLine("sh -c '[ -n \"$SSH_CONNECTION\" ] && printf \"\\nMOSH SSH_CONNECTION %s\\n\" "
                                    "\"$SSH_CONNECTION\"' ; mosh-server 'new' '-s'"));
        CHECK(!IsMoshServerCommandLine("version"));
        CHECK(!IsMoshServerCommandLine("scp -t /imports/"));
        CHECK(!IsMoshServerCommandLine("import-session Skywarn.qlsession"));
    }

    QL_TEST(AMoshRequestKeepsOnlyMoshServersOptions)
    {
        MoshServerRequest request;
        std::string error;
        REQUIRE(ParseMoshServerCommand(kClientLine, &request, &error));
        CHECK(!request.report_ssh_connection);
        std::vector<std::string> expected{"-c", "256", "-s", "-l", "LANG=en_US.UTF-8", "-l", "LC_ALL=en_US.UTF-8"};
        CHECK(request.options == expected);

        // The client's own command is never run: QuickLogger is.
        REQUIRE(ParseMoshServerCommand("mosh-server 'new' '-p' '60001:60010' '-i' '192.0.2.7' '--' '/bin/bash'",
                                       &request, &error));
        expected = {"-p", "60001:60010", "-i", "192.0.2.7"};
        CHECK(request.options == expected);
        std::vector<std::string> argv = MoshServerArgv("/usr/bin/mosh-server", request, "/srv/ql/QuickLogger");
        std::vector<std::string> run{
            "/usr/bin/mosh-server", "new",           "-p", "60001:60010", "-i", "192.0.2.7", "--",
            "/srv/ql/QuickLogger",  "--mosh-session"};
        CHECK(argv == run);
    }

    QL_TEST(TheSshConnectionProbeIsAnsweredNotRun)
    {
        MoshServerRequest request;
        std::string error;
        REQUIRE(
            ParseMoshServerCommand("sh -c '[ -n \"$SSH_CONNECTION\" ] && printf \"\\nMOSH SSH_CONNECTION %s\\n\" "
                                   "\"$SSH_CONNECTION\"' ; mosh-server 'new' '-s'",
                                   &request, &error));
        CHECK(request.report_ssh_connection);
        CHECK(request.options == std::vector<std::string>{"-s"});
    }

    QL_TEST(TermiusBareLocaleOptionGetsItsDefaultLocale)
    {
        // What Termius sends with its mosh field left empty.
        MoshServerRequest request;
        std::string error;
        REQUIRE(ParseMoshServerCommand("mosh-server new -s -l -p 60000:61000", &request, &error));
        std::vector<std::string> bare_l{"-s", "-l", "LANG=en_US.UTF-8", "-p", "60000:61000"};
        CHECK(request.options == bare_l);
        // Termius's documented default string still parses as written.
        REQUIRE(ParseMoshServerCommand("mosh-server new -s -c 256 -l LANG=en_US.UTF-8", &request, &error));
        std::vector<std::string> documented{"-s", "-c", "256", "-l", "LANG=en_US.UTF-8"};
        CHECK(request.options == documented);
        // And it ends the line with a newline.
        REQUIRE(ParseMoshServerCommand("mosh-server new -s -l -p 60000:61000\n", &request, &error));
        CHECK(request.options == bare_l);
        // A trailing bare -l is the same.
        REQUIRE(ParseMoshServerCommand("mosh-server new -s -l", &request, &error));
        std::vector<std::string> trailing{"-s", "-l", "LANG=en_US.UTF-8"};
        CHECK(request.options == trailing);
    }

    QL_TEST(AnythingElseInAMoshRequestIsRefused)
    {
        MoshServerRequest request;
        std::string error;
        CHECK(!ParseMoshServerCommand("mosh-server new; touch /tmp/x", &request, &error));
        CHECK_EQ(error, std::string("The command has a ';' in it, which isn't allowed."));
        CHECK(!ParseMoshServerCommand("mosh-server new $(id)", &request, &error));
        CHECK(!ParseMoshServerCommand("mosh-server 'new' '-x'", &request, &error));
        CHECK_EQ(error, std::string("mosh-server's option -x isn't allowed."));
        // -l is for locale variables only.
        CHECK(!ParseMoshServerCommand("mosh-server 'new' '-l' 'LD_PRELOAD=/tmp/evil.so'", &request, &error));
        CHECK(!ParseMoshServerCommand("mosh-server 'new' '-c' '256; id'", &request, &error));
        CHECK(!ParseMoshServerCommand("mosh-server 'old'", &request, &error));
        CHECK(!ParseMoshServerCommand("mosh-server 'new' '-c'", &request, &error));
        CHECK(!ParseMoshServerCommand("bash 'new'", &request, &error));
        CHECK(!ParseMoshServerCommand("mosh-server 'new", &request, &error));
    }

    QL_TEST(AMoshTokenLogsInOnceAndSoon)
    {
        TempDir dir;
        std::string error;
        std::string token = CreateMoshToken(dir.path(), "W4KWK", 1000, &error);
        REQUIRE(token.size() == 32);
        std::string username;
        CHECK(ConsumeMoshToken(dir.path(), token, 1005, &username));
        CHECK_EQ(username, std::string("W4KWK"));
        CHECK(!ConsumeMoshToken(dir.path(), token, 1006, &username));

        std::string late = CreateMoshToken(dir.path(), "W4KWK", 1000, &error);
        CHECK(!ConsumeMoshToken(dir.path(), late, 1000 + kMoshTokenSeconds + 1, &username));
        CHECK(!std::filesystem::exists(dir.File("mosh/" + late)));

        CHECK(!ConsumeMoshToken(dir.path(), "../quicklogger.db", 1000, &username));
        CHECK(!ConsumeMoshToken(dir.path(), "", 1000, &username));
    }

    QL_TEST(UnusedMoshTokensAreClearedAway)
    {
        TempDir dir;
        std::string error;
        std::string old_token = CreateMoshToken(dir.path(), "W4KWK", 1000, &error);
        std::string new_token = CreateMoshToken(dir.path(), "K4AAA", 1000 + kMoshTokenSeconds + 5, &error);
        CHECK(!std::filesystem::exists(dir.File("mosh/" + old_token)));
        CHECK(std::filesystem::exists(dir.File("mosh/" + new_token)));
    }

    QL_TEST(OverMoshFilesAreCopiedWithScpNotZmodem)
    {
        AppState state;
        state.is_console_session = false;
        state.ssh_username = "W4KWK";
        state.over_mosh = true;
        OfferZmodemSendFiles(&state, {"./exports/ssh-users/W4KWK/Skywarn.qlnet"});
        CHECK(!state.show_zmodem_confirm_modal);
        CHECK_EQ(state.status_message,
                 std::string("Saved to ./exports/ssh-users/W4KWK/Skywarn.qlnet. Over Mosh, copy files with scp or "
                             "sftp."));
        StartZmodemReceive(&state);
        CHECK(!state.show_zmodem_confirm_modal);
    }

}  // namespace ql
