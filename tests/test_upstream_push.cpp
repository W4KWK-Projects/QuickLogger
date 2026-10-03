// Pushing a session upstream (upstream_push.hpp), without running ssh: the
// arguments, reading import-session's reply, and what the operator is told.

#include <atomic>
#include <string>
#include <vector>

#include "../src/run_program.hpp"
#include "../src/settings.hpp"
#include "../src/upstream_push.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static Upstream TestUpstream()
    {
        Upstream upstream;
        upstream.host = "upstream.example.org";
        upstream.user = "W4KWK";
        upstream.port = 2222;
        return upstream;
    }

    static std::string Joined(const std::vector<std::string>& words)
    {
        std::string joined;
        for (const std::string& word : words)
        {
            joined += "[" + word + "]";
        }
        return joined;
    }

    QL_TEST(UpstreamNamesAreChecked)
    {
        CHECK(IsValidUpstreamHost("upstream.example.org"));
        CHECK(IsValidUpstreamHost("192.168.1.20"));
        CHECK(IsValidUpstreamHost("my_host-2"));
        CHECK(!IsValidUpstreamHost(""));
        CHECK(!IsValidUpstreamHost("-oProxyCommand=evil"));
        CHECK(!IsValidUpstreamHost("host name"));
        CHECK(!IsValidUpstreamHost("user@host"));
        CHECK(!IsValidUpstreamHost("host;ls"));
        CHECK(!IsValidUpstreamHost("host\n"));
        CHECK(IsValidUpstreamUser("W4KWK"));
        CHECK(!IsValidUpstreamUser("-l"));
        CHECK(!IsValidUpstreamUser("w4 kwk"));
        CHECK(!IsValidUpstreamUser("w4kwk@x"));
    }

    QL_TEST(UpstreamArgumentsNeedNoShell)
    {
        Upstream upstream = TestUpstream();
        std::vector<std::string> scp =
            UpstreamScpArguments(upstream, "./exports/Sky_2026-09-14.qlsession", "Sky_2026-09-14.qlsession");
        CHECK_EQ(Joined(scp), std::string("[-o][BatchMode=yes][-o][ConnectTimeout=15][-P][2222][--]"
                                          "[./exports/Sky_2026-09-14.qlsession]"
                                          "[W4KWK@upstream.example.org:/imports/Sky_2026-09-14.qlsession]"));
        CHECK_EQ(Joined(UpstreamImportArguments(upstream, "Sky.qlsession", "")),
                 std::string("[-o][BatchMode=yes][-o][ConnectTimeout=15][-p][2222][-l][W4KWK][--]"
                             "[upstream.example.org][import-session Sky.qlsession]"));
        CHECK_EQ(Joined(UpstreamImportArguments(upstream, "Sky.qlsession", "Sky \"Warn\" \\ Net\n")),
                 std::string("[-o][BatchMode=yes][-o][ConnectTimeout=15][-p][2222][-l][W4KWK][--]"
                             "[upstream.example.org]"
                             "[import-session --confirm-net \"Sky \\\"Warn\\\" \\\\ Net \" Sky.qlsession]"));
    }

    QL_TEST(ImportRepliesAreRead)
    {
        ImportReply reply = ParseImportReply(
            "QUICKLOGGER-RESULT 1\nstatus: imported\nnet: Sky Warn\nnet-id: 2\n"
            "session: 2026-09-14 23:30 UTC\nmessage: Imported.\n");
        CHECK(reply.status == ImportReplyStatus::kImported);
        CHECK_EQ(reply.net, std::string("Sky Warn"));
        CHECK_EQ(reply.net_id, std::int64_t{2});
        CHECK_EQ(reply.session, std::string("2026-09-14 23:30 UTC"));

        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\r\nstatus: already-imported\r\nnet: Sky\r\n").status ==
              ImportReplyStatus::kAlreadyImported);
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: needs-confirmation\nnet: Sky\n").status ==
              ImportReplyStatus::kNeedsConfirmation);
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: no-match\n").status == ImportReplyStatus::kNoMatch);
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: refused\n").status == ImportReplyStatus::kRefused);
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: error\n").status == ImportReplyStatus::kError);
        // A later key it doesn't know is fine.
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: no-match\nhint: x\n").status ==
              ImportReplyStatus::kNoMatch);
    }

    QL_TEST(ImportRepliesFromAnythingElseAreUnreadable)
    {
        CHECK(ParseImportReply("").status == ImportReplyStatus::kUnreadable);
        CHECK(ParseImportReply("bash: import-session: command not found\n").status == ImportReplyStatus::kUnreadable);
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 2\nstatus: imported\n").status == ImportReplyStatus::kUnreadable);
        CHECK(ParseImportReply(" QUICKLOGGER-RESULT 1\nstatus: imported\n").status == ImportReplyStatus::kUnreadable);
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\nmessage: no status\n").status == ImportReplyStatus::kUnreadable);
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: done\n").status == ImportReplyStatus::kUnreadable);
        // Nothing to confirm.
        CHECK(ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: needs-confirmation\n").status ==
              ImportReplyStatus::kUnreadable);
    }

    static ProgramResult Ran(int exit_status, const std::string& output, const std::string& errors)
    {
        ProgramResult result;
        result.started = true;
        result.exit_status = exit_status;
        result.output = output;
        result.errors = errors;
        return result;
    }

    // What the operator is told when the copy goes as `copy` and the import
    // as `import`.
    static PushResult Decide(const ProgramResult& copy, const ProgramResult& import)
    {
        return DecidePushResult(TestUpstream(), copy, import, "TAG Skywarn");
    }

    QL_TEST(PushesThatWorkSayWhere)
    {
        ProgramResult copied = Ran(0, "", "");
        PushResult result =
            Decide(copied, Ran(0, "QUICKLOGGER-RESULT 1\nstatus: imported\nnet: Sky Warn\nnet-id: 2\n", ""));
        CHECK(result.kind == PushResultKind::kPushed);
        CHECK_EQ(result.message, std::string("Pushed to Sky Warn on upstream.example.org."));

        result = Decide(copied, Ran(0, "QUICKLOGGER-RESULT 1\nstatus: already-imported\nnet: Sky Warn\n", ""));
        CHECK(result.kind == PushResultKind::kPushed);
        CHECK_EQ(result.message, std::string("upstream.example.org already had this session, in Sky Warn."));

        result = Decide(copied, Ran(2, "QUICKLOGGER-RESULT 1\nstatus: needs-confirmation\nnet: Sky Warn\n", ""));
        CHECK(result.kind == PushResultKind::kNeedsConfirmation);
        CHECK_EQ(result.upstream_net, std::string("Sky Warn"));
    }

    QL_TEST(APushedNetIsMergedOnceConfirmed)
    {
        ProgramResult copied = Ran(0, "", "");
        PushResult result =
            Decide(copied, Ran(2,
                               "QUICKLOGGER-RESULT 1\nstatus: needs-confirmation\nnet: TAG Skywarn\nnet-id: 4\n"
                               "new-sessions: 2\nnew-saved-stations: 1\ndiffering-sessions: 1\n",
                               ""));
        CHECK(result.kind == PushResultKind::kNeedsConfirmation);
        CHECK_EQ(result.upstream_net, std::string("TAG Skywarn"));
        CHECK_EQ(result.merge_summary,
                 std::string("adds 2 sessions and 1 saved station; 1 session differs and stays as it is there"));

        result = Decide(copied, Ran(0,
                                    "QUICKLOGGER-RESULT 1\nstatus: merged\nnet: TAG Skywarn\nnet-id: 4\n"
                                    "new-sessions: 1\nnew-saved-stations: 0\ndiffering-sessions: 0\n",
                                    ""));
        CHECK(result.kind == PushResultKind::kPushed);
        CHECK_EQ(result.message,
                 std::string("Merged into TAG Skywarn on upstream.example.org: it adds 1 session and 0 saved "
                             "stations."));

        // import-net, not import-session, for a net.
        CHECK_EQ(Joined(UpstreamImportArguments(TestUpstream(), "Sky.qlnet", "Sky Warn", true)),
                 std::string("[-o][BatchMode=yes][-o][ConnectTimeout=15][-p][2222][-l][W4KWK][--]"
                             "[upstream.example.org][import-net --confirm-net \"Sky Warn\" Sky.qlnet]"));
    }

    QL_TEST(PushesThatFailSayWhyInOneSentence)
    {
        ProgramResult copied = Ran(0, "", "");
        ProgramResult none;
        ProgramResult stopped;
        stopped.started = true;
        stopped.stopped = true;
        ProgramResult not_started;

        CHECK_EQ(Decide(not_started, none).message, std::string(kNoSshMessage));
        CHECK_EQ(Decide(stopped, none).message, std::string("Couldn't reach upstream.example.org."));
        CHECK_EQ(Decide(Ran(1, "", "ssh: connect to host upstream.example.org port 2222: Connection refused\n"), none)
                     .message,
                 std::string("Couldn't reach upstream.example.org."));
        CHECK_EQ(Decide(Ran(1, "", "ssh: connect to host x port 2222: Connection timed out\n"), none).message,
                 std::string("Couldn't reach upstream.example.org."));
        CHECK_EQ(Decide(Ran(255, "", "ssh: Could not resolve hostname x: Name or service not known\n"), none).message,
                 std::string("Couldn't find upstream.example.org."));
        CHECK_EQ(Decide(Ran(1, "", "W4KWK@x: Permission denied (publickey).\n"), none).message,
                 std::string("upstream.example.org refused your key."));
        CHECK_EQ(Decide(Ran(1, "", "Host key verification failed.\n"), none).message,
                 std::string("upstream.example.org's host key isn't known here or has changed; log in once with ssh "
                             "to check it."));
        CHECK_EQ(Decide(Ran(1, "", "scp: View-only users can't upload.\n"), none).message,
                 std::string("Your user on upstream.example.org is view-only."));
        // The same refusal from scp over SFTP (OpenSSH 9.0 and later).
        CHECK_EQ(Decide(Ran(1, "",
                            "scp: dest open \"/imports/Sky.qlsession\": Permission denied\n"
                            "scp: failed to upload file ./exports/x to /imports/Sky.qlsession\n"),
                        none)
                     .message,
                 std::string("Your user on upstream.example.org is view-only."));
        CHECK_EQ(Decide(Ran(255, "", "W4KWK@x: Permission denied (publickey,password).\n"), none).message,
                 std::string("upstream.example.org refused your key."));
        CHECK_EQ(Decide(Ran(1, "", "scp: Sky.qlsession: over the 25 MB limit.\n"), none).message,
                 std::string("The session is too big for upstream.example.org."));
        CHECK_EQ(Decide(Ran(1, "", "something odd\n"), none).message,
                 std::string("Couldn't copy the session to upstream.example.org."));

        // The copy worked; the import didn't.
        CHECK_EQ(Decide(copied, Ran(255, "", "Connection closed by 10.0.0.1 port 2222\n")).message,
                 std::string("Couldn't reach upstream.example.org."));
        CHECK_EQ(Decide(copied, stopped).message, std::string("Couldn't reach upstream.example.org."));
        CHECK_EQ(Decide(copied, Ran(3, "QUICKLOGGER-RESULT 1\nstatus: no-match\n", "")).message,
                 std::string("No net named like TAG Skywarn on upstream.example.org."));
        CHECK_EQ(Decide(copied, Ran(4,
                                    "QUICKLOGGER-RESULT 1\nstatus: refused\n"
                                    "message: View-only users can't run commands.\n",
                                    ""))
                     .message,
                 std::string("Your user on upstream.example.org is view-only."));
        CHECK_EQ(Decide(copied, Ran(4, "QUICKLOGGER-RESULT 1\nstatus: refused\nmessage: x\n", "")).message,
                 std::string("upstream.example.org refused the session."));
        CHECK_EQ(Decide(copied, Ran(1, "QUICKLOGGER-RESULT 1\nstatus: error\n", "")).message,
                 std::string("upstream.example.org couldn't import the session."));
        CHECK_EQ(Decide(copied, Ran(127, "", "sh: import-session: not found\n")).message,
                 std::string("The upstream isn't a QuickLogger, or is a different version."));
        CHECK(Decide(copied, Ran(127, "", "")).kind == PushResultKind::kFailed);
    }

    QL_TEST(DiscardingAnUploadRunsDiscardUploadOnTheSameConnection)
    {
        std::vector<std::string> discard = UpstreamDiscardArguments(TestUpstream(), "Sky.qlsession");
        std::vector<std::string> import = UpstreamImportArguments(TestUpstream(), "Sky.qlsession", "");
        REQUIRE(discard.size() == import.size());
        for (std::size_t i = 0; i + 1 < discard.size(); ++i)
        {
            CHECK_EQ(discard[i], import[i]);
        }
        CHECK_EQ(discard.back(), std::string("discard-upload Sky.qlsession"));
    }

    QL_TEST(WindowsCommandLinesQuoteEachArgument)
    {
        std::vector<std::string> arguments;
        arguments.emplace_back("-o");
        arguments.emplace_back("");
        arguments.emplace_back("two words");
        arguments.emplace_back("import-session --confirm-net \"Sky Warn\" a.qlsession");
        arguments.emplace_back("C:\\dir with space\\");
        arguments.emplace_back("a\\\\b");
        CHECK_EQ(WindowsCommandLine("C:\\Windows\\System32\\OpenSSH\\ssh.exe", arguments),
                 std::string("C:\\Windows\\System32\\OpenSSH\\ssh.exe -o \"\" \"two words\" "
                             "\"import-session --confirm-net \\\"Sky Warn\\\" a.qlsession\" "
                             "\"C:\\dir with space\\\\\" a\\\\b"));
    }

#if !defined(_WIN32)
    QL_TEST(ProgramsRunWithoutAShell)
    {
        std::string echo = FindProgramOnPath("echo");
        REQUIRE(!echo.empty());
        std::vector<std::string> arguments;
        arguments.emplace_back("a; ls");
        arguments.emplace_back("$HOME");
        ProgramResult result = RunProgram(echo, arguments, 10, nullptr);
        CHECK(result.started);
        CHECK_EQ(result.exit_status, 0);
        CHECK_EQ(result.output, std::string("a; ls $HOME\n"));

        std::string sleep = FindProgramOnPath("sleep");
        REQUIRE(!sleep.empty());
        std::vector<std::string> long_sleep;
        long_sleep.emplace_back("30");
        result = RunProgram(sleep, long_sleep, 1, nullptr);
        CHECK(result.stopped);
        CHECK_EQ(result.exit_status, -1);

        std::atomic<bool> cancel(true);
        result = RunProgram(sleep, long_sleep, 30, &cancel);
        CHECK(result.stopped);

        CHECK(!RunProgram("/nonexistent/program", arguments, 10, nullptr).started);
        CHECK(FindProgramOnPath("no-such-program-anywhere").empty());
    }
#endif

    QL_TEST(UpstreamSettingsAreKept)
    {
        TempDir dir;
        std::string path = dir.File("settings.txt");
        AppSettings settings;
        CHECK_EQ(settings.upstream_port, 22);
        settings.upstream_host = "upstream.example.org";
        settings.upstream_user = "W4KWK";
        settings.upstream_port = 2222;
        SaveSettings(path, settings);
        AppSettings loaded = LoadSettings(path);
        CHECK_EQ(loaded.upstream_host, std::string("upstream.example.org"));
        CHECK_EQ(loaded.upstream_user, std::string("W4KWK"));
        CHECK_EQ(loaded.upstream_port, 2222);

        WriteTextFile(path, "upstream_port=99999\n");
        CHECK_EQ(LoadSettings(path).upstream_port, 22);
        WriteTextFile(path, "upstream_port=-1\n");
        CHECK_EQ(LoadSettings(path).upstream_port, 22);
    }

    QL_TEST(OnlyAMissingUploadIsWorthUploadingAgain)
    {
        CHECK(ImportReplyMeansFileMissing(ParseImportReply(
            "QUICKLOGGER-RESULT 1\nstatus: refused\nmessage: There's no Sky.qlsession in your /imports.\n")));
        CHECK(!ImportReplyMeansFileMissing(
            ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: refused\nmessage: View-only users can't run commands.\n")));
        CHECK(!ImportReplyMeansFileMissing(ParseImportReply(
            "QUICKLOGGER-RESULT 1\nstatus: refused\nmessage: Sky.qlsession is over the 25 MB limit.\n")));
        CHECK(!ImportReplyMeansFileMissing(ParseImportReply("QUICKLOGGER-RESULT 1\nstatus: imported\nnet: Sky\n")));
        CHECK(!ImportReplyMeansFileMissing(ParseImportReply("not a reply")));
    }

}  // namespace ql
