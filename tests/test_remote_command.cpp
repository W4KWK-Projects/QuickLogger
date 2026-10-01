// The SSH server's own commands (remote_command.hpp): splitting and parsing
// a command line, and import-session against a test database.

#include <cstdint>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/net_slice.hpp"
#include "../src/remote_command.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static std::vector<std::string> Split(const std::string& line)
    {
        std::vector<std::string> words;
        std::string error;
        if (!SplitCommandLine(line, &words, &error))
        {
            words.clear();
            words.emplace_back("(error)");
        }
        return words;
    }

    QL_TEST(CommandLinesSplitWithoutAShell)
    {
        std::vector<std::string> words = Split("  import-session\t--confirm-net \"TAG  Skywarn\" Net_1.qlsession ");
        REQUIRE(words.size() == 4);
        CHECK_EQ(words[0], std::string("import-session"));
        CHECK_EQ(words[2], std::string("TAG  Skywarn"));
        CHECK_EQ(words[3], std::string("Net_1.qlsession"));

        words = Split("a \"say \\\"hi\\\" \\\\ ; | $x\" \"\"");
        REQUIRE(words.size() == 3);
        CHECK_EQ(words[1], std::string("say \"hi\" \\ ; | $x"));
        CHECK_EQ(words[2], std::string(""));
    }

    QL_TEST(CommandLinesRefuseWhatAShellWouldActOn)
    {
        CHECK_EQ(Split("import-session x; ls")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session x|ls")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session $(ls)")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session `ls`")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session 'x'")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session x > y")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session \"unclosed")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session a\"b\"")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session \"a\"b")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session x\nls")[0], std::string("(error)"));
        CHECK_EQ(Split("import-session \"x\ny\"")[0], std::string("(error)"));
    }

    static std::string ParseError(const std::string& line)
    {
        RemoteCommand command;
        std::string error;
        if (ParseRemoteCommand(line, &command, &error))
        {
            return "(parsed)";
        }
        return error;
    }

    QL_TEST(RemoteCommandsParse)
    {
        RemoteCommand command;
        std::string error;
        REQUIRE(ParseRemoteCommand("version", &command, &error));
        CHECK(command.kind == RemoteCommandKind::kVersion);

        REQUIRE(ParseRemoteCommand("import-session Net.qlsession", &command, &error));
        CHECK(command.kind == RemoteCommandKind::kImportSession);
        CHECK_EQ(command.file, std::string("Net.qlsession"));
        CHECK(!command.has_confirm_net);

        REQUIRE(ParseRemoteCommand("import-session --confirm-net \"Sky Warn\" Net.qlsession", &command, &error));
        CHECK(command.has_confirm_net);
        CHECK_EQ(command.confirm_net, std::string("Sky Warn"));
        CHECK_EQ(command.file, std::string("Net.qlsession"));
    }

    QL_TEST(RemoteCommandsRefuseAnythingElse)
    {
        CHECK(ParseError("") != "(parsed)");
        CHECK(ParseError("sh") != "(parsed)");
        CHECK(ParseError("bash -c ls") != "(parsed)");
        CHECK(ParseError("version now") != "(parsed)");
        CHECK(ParseError("import-session") != "(parsed)");
        CHECK(ParseError("import-session a.qlsession b.qlsession") != "(parsed)");
        CHECK(ParseError("import-session --confirm-net") != "(parsed)");
        CHECK(ParseError("import-session --confirm-net Skywarn") != "(parsed)");
        CHECK(ParseError("import-session --force a.qlsession") != "(parsed)");
        CHECK(ParseError("import-session a.qlsession --confirm-net Skywarn") != "(parsed)");
        CHECK(ParseError("import-session x; ls") != "(parsed)");
    }

    QL_TEST(AParseErrorIsAResult)
    {
        RemoteCommandResult result = RemoteCommandParseError("Unknown command: sh.\nSecond line.");
        CHECK_EQ(result.exit_status, kRemoteExitError);
        CHECK_EQ(result.output, std::string("QUICKLOGGER-RESULT 1\nstatus: error\n"
                                            "message: Unknown command: sh. Second line.\n"));
    }

    // A server database, with an SSH user's /imports next to it.
    class ImportFixture
    {
    public:
        ImportFixture() : db_path_(dir_.File("quicklogger.db")), db_(db_path_)
        {
            EnsureDirectory(ImportsDir());
        }

        Database* db()
        {
            return &db_;
        }

        std::string ImportsDir() const
        {
            return SessionImportsDir(db_path_, "W4KWK");
        }

        // Writes `file` into /imports: one session of a net named `net_name`
        // (ad hoc or not), logged somewhere else.
        void WriteSession(const std::string& file, const std::string& net_name, bool ad_hoc = false,
                          std::int64_t started_at = 1789428600)
        {
            TempDir source_dir;
            Database source(source_dir.File("source.db"));
            Net net;
            net.name = net_name;
            net.is_ad_hoc = ad_hoc;
            std::int64_t net_id = source.CreateNet(net);
            std::int64_t instance = AddTestInstance(&source, net_id, "2026-09-14", started_at, "K4ABC");
            AddTestCheckIn(&source, instance, "K4ABC", 1);
            AddTestCheckIn(&source, instance, "W4XYZ", 2);
            source.CloseNetInstance(instance, started_at + 1800);
            std::string error;
            REQUIRE(WriteNetSliceFile(ImportsDir() + "/" + file, GatherSessionSlice(&source, instance), &error));
        }

        RemoteCommandResult Run(const std::string& line, bool view_only = false)
        {
            RemoteCommand command;
            std::string error;
            if (!ParseRemoteCommand(line, &command, &error))
            {
                return RemoteCommandParseError(error);
            }
            return RunRemoteCommand(command, &db_, db_path_, "W4KWK", view_only, 1800000000);
        }

        bool Uploaded(const std::string& file) const
        {
            return FileExists(ImportsDir() + "/" + file);
        }

    private:
        TempDir dir_;
        std::string db_path_;
        Database db_;
    };

    QL_TEST(ImportSessionImportsIntoTheNetOfTheSameName)
    {
        ImportFixture fixture;
        std::int64_t net_id = AddTestNet(fixture.db(), "TAG Skywarn");
        fixture.WriteSession("Sky.qlsession", "tag  SKYWARN");

        RemoteCommandResult result = fixture.Run("import-session Sky.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK_EQ(result.output,
                 "QUICKLOGGER-RESULT 1\nstatus: imported\nnet: TAG Skywarn\nnet-id: " + std::to_string(net_id) +
                     "\nsession: 2026-09-14 23:30 UTC\n"
                     "message: Imported the session into TAG Skywarn (logged as \"tag  SKYWARN\").\n");
        CHECK_EQ(fixture.db()->GetNetInstancesForNet(net_id).size(), std::size_t(1));
        CHECK(!fixture.Uploaded("Sky.qlsession"));

        // Pushed again: harmless.
        fixture.WriteSession("Sky.qlsession", "TAG Skywarn");
        result = fixture.Run("import-session /imports/Sky.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK(result.output.find("status: already-imported\n") != std::string::npos);
        CHECK_EQ(fixture.db()->GetNetInstancesForNet(net_id).size(), std::size_t(1));
        CHECK(!fixture.Uploaded("Sky.qlsession"));
    }

    QL_TEST(ImportSessionAsksAboutTheFirstLookAlike)
    {
        ImportFixture fixture;
        std::int64_t weekly = AddTestNet(fixture.db(), "Skywarn Weekly Net");
        std::int64_t sky_warn = AddTestNet(fixture.db(), "Sky Warn");
        std::int64_t ares = AddTestNet(fixture.db(), "Hamilton County ARES");
        fixture.WriteSession("Sky.qlsession", "TAG Skywarn");

        RemoteCommandResult result = fixture.Run("import-session Sky.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitNeedsConfirmation);
        CHECK(result.output.find("status: needs-confirmation\nnet: Sky Warn\nnet-id: " + std::to_string(sky_warn) +
                                 "\n") != std::string::npos);
        CHECK(result.output.find("1 other net looks alike too.") != std::string::npos);
        CHECK(fixture.Uploaded("Sky.qlsession"));
        CHECK(fixture.db()->GetNetInstancesForNet(sky_warn).empty());

        // Not a look-alike: refused, whatever the operator says.
        result = fixture.Run("import-session --confirm-net \"Hamilton County ARES\" Sky.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitRefused);
        CHECK(result.output.find("status: refused\n") != std::string::npos);
        CHECK(fixture.db()->GetNetInstancesForNet(ares).empty());
        CHECK(fixture.Uploaded("Sky.qlsession"));

        result = fixture.Run("import-session --confirm-net \"No Such Net\" Sky.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitNoMatch);
        CHECK(fixture.Uploaded("Sky.qlsession"));

        // Confirmed by its exact name (case and spacing aside), even the one
        // that wasn't offered first.
        result = fixture.Run("import-session --confirm-net \"skywarn  weekly net\" Sky.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK(result.output.find("status: imported\nnet: Skywarn Weekly Net\n") != std::string::npos);
        CHECK_EQ(fixture.db()->GetNetInstancesForNet(weekly).size(), std::size_t(1));
        CHECK(!fixture.Uploaded("Sky.qlsession"));
    }

    QL_TEST(ImportSessionFindsNoMatch)
    {
        ImportFixture fixture;
        AddTestNet(fixture.db(), "Hamilton County ARES");
        // A newline in a name never splits a line of the result.
        fixture.WriteSession("Dixie.qlsession", "Dixie\nTraders");

        RemoteCommandResult result = fixture.Run("import-session Dixie.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitNoMatch);
        CHECK_EQ(result.output, std::string("QUICKLOGGER-RESULT 1\nstatus: no-match\nsession: 2026-09-14 23:30 UTC\n"
                                            "message: No net here looks like \"Dixie Traders\".\n"));
        CHECK(fixture.Uploaded("Dixie.qlsession"));
    }

    QL_TEST(ImportSessionMakesAnAdHocNetOnce)
    {
        ImportFixture fixture;
        fixture.WriteSession("Storm.qlsession", "Storm Spotters", /*ad_hoc=*/true);

        RemoteCommandResult result = fixture.Run("import-session Storm.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK(result.output.find("status: imported\nnet: Storm Spotters\n") != std::string::npos);
        std::vector<NetInstance> sessions = fixture.db()->GetAdHocNetInstances();
        REQUIRE(sessions.size() == 1);
        CHECK(!fixture.Uploaded("Storm.qlsession"));

        fixture.WriteSession("Storm.qlsession", "Storm Spotters", /*ad_hoc=*/true);
        result = fixture.Run("import-session Storm.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK(result.output.find("status: already-imported\nnet: Storm Spotters\nnet-id: " +
                                 std::to_string(sessions[0].net_id) + "\n") != std::string::npos);
        CHECK_EQ(fixture.db()->GetAdHocNetInstances().size(), std::size_t(1));
        CHECK(!fixture.Uploaded("Storm.qlsession"));
    }

    QL_TEST(ImportSessionRefusesBadFilesAndViewOnlyUsers)
    {
        ImportFixture fixture;
        AddTestNet(fixture.db(), "TAG Skywarn");
        fixture.WriteSession("Sky.qlsession", "TAG Skywarn");
        fixture.WriteSession("Sky.qlnet", "TAG Skywarn");
        WriteTextFile(fixture.ImportsDir() + "/Junk.qlsession", "not a database");

        CHECK_EQ(fixture.Run("import-session Sky.qlsession", /*view_only=*/true).exit_status, kRemoteExitRefused);
        CHECK_EQ(fixture.Run("version", /*view_only=*/true).exit_status, kRemoteExitRefused);
        CHECK_EQ(fixture.Run("import-session Sky.qlnet").exit_status, kRemoteExitRefused);
        CHECK_EQ(fixture.Run("import-session ../Sky.qlsession").exit_status, kRemoteExitRefused);
        CHECK_EQ(fixture.Run("import-session /exports/Sky.qlsession").exit_status, kRemoteExitRefused);
        CHECK_EQ(fixture.Run("import-session .Sky.qlsession").exit_status, kRemoteExitRefused);
        CHECK_EQ(fixture.Run("import-session Missing.qlsession").exit_status, kRemoteExitRefused);
        CHECK_EQ(fixture.Run("import-session Junk.qlsession").exit_status, kRemoteExitRefused);
        CHECK(fixture.Uploaded("Sky.qlsession"));
        CHECK(fixture.Uploaded("Junk.qlsession"));
    }

    QL_TEST(VersionNamesTheInterface)
    {
        ImportFixture fixture;
        RemoteCommandResult result = fixture.Run("version");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK(result.output.rfind("QUICKLOGGER-RESULT 1\nstatus: ok\nversion: ", 0) == 0);
        CHECK(result.output.find("\ninterface: 1\n") != std::string::npos);
    }

    static std::int64_t QueryNumber(const std::string& db_path, const std::string& sql)
    {
        sqlite3* db = nullptr;
        sqlite3_open(db_path.c_str(), &db);
        sqlite3_stmt* statement = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr);
        std::int64_t value = -1;
        if (sqlite3_step(statement) == SQLITE_ROW)
        {
            value = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
        sqlite3_close(db);
        return value;
    }

    QL_TEST(PushedAtStaysInItsOwnDatabase)
    {
        TempDir dir;
        std::string db_path = dir.File("quicklogger.db");
        std::int64_t instance = 0;
        {
            Database db(db_path);
            std::int64_t net_id = AddTestNet(&db, "TAG Skywarn");
            instance = AddTestInstance(&db, net_id, "2026-09-14", 1789428600, "K4ABC");
            AddTestCheckIn(&db, instance, "K4ABC", 1);
        }
        CHECK_EQ(QueryNumber(db_path, "SELECT pushed_at FROM net_instances"), std::int64_t{0});
        CHECK_EQ(QueryNumber(db_path, "UPDATE net_instances SET pushed_at = 1800000000 RETURNING pushed_at"),
                 std::int64_t{1800000000});

        Database db(db_path);
        std::string file = dir.File("Sky.qlsession");
        std::string error;
        REQUIRE(WriteNetSliceFile(file, GatherSessionSlice(&db, instance), &error));
        CHECK_EQ(QueryNumber(file, "SELECT COUNT(*) FROM net_instances WHERE pushed_at != 0"), std::int64_t{0});
    }

}  // namespace ql
