// Pulling nets and sessions from the upstream (upstream_pull.hpp), without
// running ssh: the arguments, reading list-nets and the exports' replies, and
// what the operator is told.

#include <string>
#include <vector>

#include "../src/run_program.hpp"
#include "../src/upstream_pull.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static Upstream PullUpstream()
    {
        Upstream upstream;
        upstream.host = "upstream.example.org";
        upstream.user = "W4KWK";
        upstream.port = 2222;
        return upstream;
    }

    static ProgramResult Answered(int exit_status, const std::string& output, const std::string& errors = "")
    {
        ProgramResult result;
        result.started = true;
        result.exit_status = exit_status;
        result.output = output;
        result.errors = errors;
        return result;
    }

    static std::string JoinedWords(const std::vector<std::string>& words)
    {
        std::string joined;
        for (const std::string& word : words)
        {
            joined += "[" + word + "]";
        }
        return joined;
    }

    QL_TEST(PullArgumentsNeedNoShell)
    {
        Upstream upstream = PullUpstream();
        CHECK_EQ(JoinedWords(UpstreamListNetsArguments(upstream)),
                 std::string("[-o][BatchMode=yes][-o][ConnectTimeout=15][-p][2222][-l][W4KWK][--]"
                             "[upstream.example.org][list-nets]"));
        CHECK_EQ(JoinedWords(UpstreamExportArguments(upstream, "TAG \"Sky\" Warn", false)),
                 std::string("[-o][BatchMode=yes][-o][ConnectTimeout=15][-p][2222][-l][W4KWK][--]"
                             "[upstream.example.org][export-net \"TAG \\\"Sky\\\" Warn\"]"));
        CHECK_EQ(JoinedWords(UpstreamExportArguments(upstream, "Skywarn", true)),
                 std::string("[-o][BatchMode=yes][-o][ConnectTimeout=15][-p][2222][-l][W4KWK][--]"
                             "[upstream.example.org][export-sessions \"Skywarn\"]"));
        CHECK_EQ(JoinedWords(UpstreamFetchArguments(upstream, {"A_2026-09-14.qlsession", "A_2026-09-07.qlsession"},
                                                    "/home/w4kwk/imports/.pull")),
                 std::string("[-o][BatchMode=yes][-o][ConnectTimeout=15][-P][2222][--]"
                             "[W4KWK@upstream.example.org:/exports/A_2026-09-14.qlsession]"
                             "[W4KWK@upstream.example.org:/exports/A_2026-09-07.qlsession]"
                             "[/home/w4kwk/imports/.pull]"));
    }

    QL_TEST(TheListOfNetsIsRead)
    {
        PullResult result =
            DecideListNetsResult(PullUpstream(), Answered(0,
                                                          "QUICKLOGGER-RESULT 1\nstatus: ok\n"
                                                          "net: Hamilton County ARES\nservice: amateur\nsessions: 0\n"
                                                          "net: TAG Skywarn\nservice: amateur\nsessions: 12\n"
                                                          "net: GMRS Chat\nservice: gmrs\nsessions: 3\n"
                                                          "message: 3 nets.\n"));
        REQUIRE(result.kind == PullResultKind::kNets);
        REQUIRE(result.nets.size() == 3);
        CHECK_EQ(result.nets[0].name, std::string("Hamilton County ARES"));
        CHECK_EQ(result.nets[0].sessions, 0);
        CHECK_EQ(result.nets[1].name, std::string("TAG Skywarn"));
        CHECK_EQ(result.nets[1].sessions, 12);
        CHECK_EQ(result.nets[2].service, std::string("gmrs"));

        // An upstream with no nets is still an answer.
        result =
            DecideListNetsResult(PullUpstream(), Answered(0, "QUICKLOGGER-RESULT 1\nstatus: ok\nmessage: 0 nets.\n"));
        CHECK(result.kind == PullResultKind::kNets);
        CHECK(result.nets.empty());
    }

    QL_TEST(ALackOfAListOfNetsSaysWhy)
    {
        Upstream upstream = PullUpstream();
        ProgramResult not_started;
        CHECK_EQ(DecideListNetsResult(upstream, not_started).message, std::string(kNoSshMessage));
        CHECK_EQ(
            DecideListNetsResult(upstream, Answered(255, "", "ssh: connect to host x port 2222: Connection refused\n"))
                .message,
            std::string("Couldn't reach upstream.example.org."));
        CHECK_EQ(DecideListNetsResult(upstream, Answered(255, "", "W4KWK@x: Permission denied (publickey).\n")).message,
                 std::string("upstream.example.org refused your key."));
        CHECK_EQ(DecideListNetsResult(upstream, Answered(127, "", "sh: list-nets: not found\n")).message,
                 std::string("The upstream isn't a QuickLogger, or is a different version."));
        // An older QuickLogger, which knows only the push commands.
        CHECK_EQ(DecideListNetsResult(upstream, Answered(1,
                                                         "QUICKLOGGER-RESULT 1\nstatus: error\n"
                                                         "message: Unknown command: list-nets. QuickLogger runs only "
                                                         "import-session, discard-upload and version.\n"))
                     .message,
                 std::string("upstream.example.org is a QuickLogger that can't list its nets (it needs a newer "
                             "version)."));
        CHECK_EQ(DecideListNetsResult(upstream, Answered(4, "QUICKLOGGER-RESULT 1\nstatus: refused\nmessage: Nope.\n"))
                     .message,
                 std::string("upstream.example.org refused: Nope."));
        // A bare "Permission denied" is a push's view-only user; a pull has no such thing.
        CHECK_EQ(DecideListNetsResult(upstream, Answered(255, "", "scp: x: Permission denied\n")).message,
                 std::string("Couldn't ask upstream.example.org."));
    }

    QL_TEST(AnExportNamesTheFilesToFetch)
    {
        PullResult result = DecideExportResult(
            PullUpstream(),
            Answered(0,
                     "QUICKLOGGER-RESULT 1\nstatus: ok\nnet: TAG Skywarn\nnet-id: 4\nsessions: 2\nnot-sent: 1\n"
                     "file: TAG_Skywarn_2026-09-14_233000.qlsession\nfile: TAG_Skywarn_2026-09-07_233000.qlsession\n"
                     "message: Exported 2 of TAG Skywarn's sessions.\n"),
            "tag skywarn", ".qlsession");
        REQUIRE(result.kind == PullResultKind::kFiles);
        CHECK_EQ(result.net, std::string("TAG Skywarn"));
        CHECK_EQ(result.not_sent, 1);
        REQUIRE(result.files.size() == 2);
        CHECK_EQ(result.files[0], std::string("TAG_Skywarn_2026-09-14_233000.qlsession"));

        result = DecideExportResult(PullUpstream(),
                                    Answered(0,
                                             "QUICKLOGGER-RESULT 1\nstatus: ok\nnet: TAG Skywarn\nnet-id: 4\n"
                                             "file: TAG_Skywarn.qlnet\n"),
                                    "TAG Skywarn", ".qlnet");
        CHECK(result.kind == PullResultKind::kFiles);
        CHECK_EQ(result.files.size(), std::size_t(1));
    }

    QL_TEST(AnExportCannotNameAFileOutsideWhatQuickLoggerExports)
    {
        Upstream upstream = PullUpstream();
        // Each of these would have scp fetch something else, or somewhere else.
        const char* const bad_names[] = {"../../.ssh/authorized_keys",
                                         "/etc/passwd",
                                         "-oProxyCommand=x.qlnet",
                                         ".hidden.qlnet",
                                         "Skywarn.txt",
                                         "Sky warn.qlnet",
                                         "a:b.qlnet",
                                         "Skywarn.qlsession"};
        for (const char* name : bad_names)
        {
            PullResult result = DecideExportResult(
                upstream, Answered(0, std::string("QUICKLOGGER-RESULT 1\nstatus: ok\nfile: ") + name + "\n"), "Skywarn",
                ".qlnet");
            CHECK(result.kind == PullResultKind::kFailed);
            CHECK_EQ(result.message,
                     std::string("upstream.example.org named a file that isn't one QuickLogger exports."));
        }
    }

    QL_TEST(AnExportThatFailedSaysWhy)
    {
        Upstream upstream = PullUpstream();
        CHECK_EQ(DecideExportResult(upstream, Answered(3, "QUICKLOGGER-RESULT 1\nstatus: no-match\n"), "Sky", ".qlnet")
                     .message,
                 std::string("upstream.example.org has no net named Sky any more."));
        CHECK_EQ(DecideExportResult(upstream,
                                    Answered(1, "QUICKLOGGER-RESULT 1\nstatus: error\nmessage: Couldn't write x.\n"),
                                    "Sky", ".qlnet")
                     .message,
                 std::string("upstream.example.org couldn't export that net."));
        ProgramResult stopped;
        stopped.started = true;
        stopped.stopped = true;
        CHECK_EQ(DecideExportResult(upstream, stopped, "Sky", ".qlnet").message,
                 std::string("Couldn't reach upstream.example.org."));
        CHECK_EQ(DecideExportResult(upstream, Answered(0, "hello\n"), "Sky", ".qlnet").message,
                 std::string("The upstream isn't a QuickLogger, or is a different version."));
    }

}  // namespace ql
