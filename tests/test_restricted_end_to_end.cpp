// Restricted-nets mode end to end: the system's real ssh and scp against a
// real QuickLogger server that has Restricted on. Run by
// tests/ssh_end_to_end.sh, which starts that server (its own, on
// QL_RESTRICTED_E2E_PORT, in $QL_PUSH_E2E_DIR/restricted):
// RestrictedSeed makes its database before it starts; RestrictedExecEndToEnd
// pushes sessions and nets as a full user, a Net Admin and, when an Admin
// changes things behind its back, checks the very next push sees it.
// tests/restricted_screens.py then drives the screens. Without
// QL_RESTRICTED_E2E_PORT these do nothing.
//
// Users (all with the key tests/ssh_end_to_end.sh made): N4FUL a full user
// given Tuesday Net, N4NET a Net Admin given Tuesday Net, N4ADM an Admin.

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/net_slice.hpp"
#include "../src/remote_command.hpp"
#include "../src/run_program.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static std::string RestrictedDir()
    {
        const char* dir = std::getenv("QL_PUSH_E2E_DIR");
        const char* port = std::getenv("QL_RESTRICTED_E2E_PORT");
        return dir == nullptr || port == nullptr ? std::string() : std::string(dir) + "/restricted";
    }

    static std::string RestrictedDb()
    {
        return RestrictedDir() + "/quicklogger.db";
    }

    static std::string FilesDir()
    {
        return RestrictedDir() + "/../restricted-files";
    }

    // One closed session of `net_name` in `file`, and the net with it in
    // `net_file`, as another QuickLogger exported them.
    static void WriteFiles(const std::string& net_name, const std::string& session_file, const std::string& net_file)
    {
        TempDir source_dir;
        Database source(source_dir.File("source.db"));
        std::int64_t net_id = AddTestNet(&source, net_name);
        std::int64_t instance = AddTestInstance(&source, net_id, "2026-09-14", 1789428600, "K4ABC");
        AddTestCheckIn(&source, instance, "K4ABC", 1);
        AddTestCheckIn(&source, instance, "W4XYZ", 2);
        source.CloseNetInstance(instance, 1789428600 + 1800);
        std::string error;
        if (!session_file.empty())
        {
            REQUIRE(WriteNetSliceFile(FilesDir() + "/" + session_file, GatherSessionSlice(&source, instance), &error));
        }
        if (!net_file.empty())
        {
            REQUIRE(WriteNetSliceFile(FilesDir() + "/" + net_file, GatherNetSlice(&source, net_id), &error));
        }
    }

    QL_TEST(RestrictedSeed)
    {
        std::string dir = RestrictedDir();
        if (dir.empty())
        {
            return;
        }
        std::string key = ReadTextFile(std::string(std::getenv("QL_PUSH_E2E_DIR")) + "/ssh/key.pub");
        REQUIRE(!key.empty());
        key = key.substr(0, key.find_last_not_of("\r\n") + 1);

        Database db(RestrictedDb());
        MarkAllLoaded(&db, static_cast<std::int64_t>(std::time(nullptr)));
        std::int64_t tuesday = AddTestNet(&db, "Tuesday Net");
        AddTestNet(&db, "Friday Net");
        struct Seed
        {
            const char* username;
            int level;
        };
        for (const Seed& seed :
             {Seed{"N4FUL", kAccessUser}, Seed{"N4NET", kAccessNetAdmin}, Seed{"N4ADM", kAccessAdmin}})
        {
            User user;
            user.username = seed.username;
            user.public_key = key;
            user.amateur_callsign = seed.username;
            user.access_level = seed.level;
            REQUIRE(db.CreateUser(user));
            // The net list, not Settings, is where they land.
            std::filesystem::create_directories(dir + "/settings");
            WriteTextFile(dir + "/settings/" + seed.username + ".txt", "location=37415\n");
        }
        db.GrantNet("N4FUL", tuesday, "console");
        db.GrantNet("N4NET", tuesday, "console");
        db.SetServerOption(kOptionRestrictedNets, true);

        std::filesystem::create_directories(FilesDir());
        WriteFiles("Tuesday Net", "Tuesday.qlsession", "Tuesday.qlnet");
        WriteFiles("Friday Net", "Friday.qlsession", "Friday.qlnet");
        WriteFiles("Saturday Net", "", "Saturday.qlnet");
        WriteFiles("Sunday Net", "", "Sunday.qlnet");
    }

    // Uploads `file` to `user`'s /imports and runs `command` on it, as
    // another QuickLogger does for a push.
    static ProgramResult Push(const std::string& user, const std::string& file, const std::string& command)
    {
        std::string port = std::getenv("QL_RESTRICTED_E2E_PORT");
        ProgramResult copied =
            RunProgram(FindProgramOnPath("scp"), {"-P", port, FilesDir() + "/" + file, user + "@127.0.0.1:/imports/"},
                       120, nullptr);
        REQUIRE(copied.started);
        REQUIRE(copied.exit_status == 0);
        return RunProgram(FindProgramOnPath("ssh"), {"-T", "-p", port, user + "@127.0.0.1", command}, 60, nullptr);
    }

    static bool Says(const ProgramResult& result, const std::string& text)
    {
        return result.output.find(text) != std::string::npos || result.errors.find(text) != std::string::npos;
    }

    static std::int64_t NetIdNamed(Database* db, const std::string& name)
    {
        for (const Net& net : db->GetAllNets())
        {
            if (net.name == name)
            {
                return net.id;
            }
        }
        return 0;
    }

    QL_TEST(RestrictedExecEndToEnd)
    {
        if (RestrictedDir().empty())
        {
            return;
        }
        REQUIRE(!FindProgramOnPath("ssh").empty());
        REQUIRE(!FindProgramOnPath("scp").empty());
        Database db(RestrictedDb());
        std::int64_t tuesday = NetIdNamed(&db, "Tuesday Net");
        std::int64_t friday = NetIdNamed(&db, "Friday Net");
        REQUIRE(tuesday != 0);
        REQUIRE(friday != 0);

        // A full user pushes into the net they were given, not another.
        ProgramResult result = Push("N4FUL", "Friday.qlsession", "import-session Friday.qlsession");
        REQUIRE(result.started);
        CHECK_EQ(result.exit_status, kRemoteExitRefused);
        CHECK(Says(result, "status: refused\n"));
        CHECK(Says(result, "Friday Net isn't one of your nets."));
        CHECK(db.GetNetInstancesForNet(friday).empty());
        result = Push("N4FUL", "Tuesday.qlsession", "import-session Tuesday.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK(Says(result, "status: imported\n"));
        CHECK_EQ(db.GetNetInstancesForNet(tuesday).size(), std::size_t{1});

        // A new net is a Net Admin's; a merge needs the net.
        result = Push("N4FUL", "Saturday.qlnet", "import-net Saturday.qlnet");
        CHECK_EQ(result.exit_status, kRemoteExitRefused);
        CHECK(Says(result, "Only Net Admins can add a new net."));
        CHECK_EQ(NetIdNamed(&db, "Saturday Net"), std::int64_t{0});
        result = Push("N4FUL", "Friday.qlnet", "import-net --confirm-net \"Friday Net\" Friday.qlnet");
        CHECK_EQ(result.exit_status, kRemoteExitRefused);
        CHECK(db.GetNetInstancesForNet(friday).empty());
        result = Push("N4FUL", "Tuesday.qlnet", "import-net Tuesday.qlnet");
        CHECK_EQ(result.exit_status, kRemoteExitNeedsConfirmation);
        result = RunProgram(FindProgramOnPath("ssh"),
                            {"-T", "-p", std::getenv("QL_RESTRICTED_E2E_PORT"), "N4FUL@127.0.0.1",
                             "import-net --confirm-net \"Tuesday Net\" Tuesday.qlnet"},
                            60, nullptr);
        CHECK_EQ(result.exit_status, kRemoteExitOk);

        // A Net Admin adds a net, and is given it.
        result = Push("N4NET", "Saturday.qlnet", "import-net Saturday.qlnet");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        std::int64_t saturday = NetIdNamed(&db, "Saturday Net");
        CHECK(saturday != 0);
        CHECK(db.GetNetGrants("N4NET").size() == 2);

        // Not a view of the old answer: an Admin gives Friday to N4FUL
        // behind the running server's back, and their very next push works.
        db.GrantNet("N4FUL", friday, "N4ADM");
        result = Push("N4FUL", "Friday.qlsession", "import-session Friday.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK_EQ(db.GetNetInstancesForNet(friday).size(), std::size_t{1});
        db.RevokeNet("N4FUL", friday);
        result = Push("N4FUL", "Friday.qlsession", "import-session Friday.qlsession");
        CHECK_EQ(result.exit_status, kRemoteExitRefused);

        // With Restricted off, as before: a full user adds a net.
        db.SetServerOption(kOptionRestrictedNets, false);
        result = Push("N4FUL", "Sunday.qlnet", "import-net Sunday.qlnet");
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK(NetIdNamed(&db, "Sunday Net") != 0);

        // Put it back as the screens expect it.
        db.SetServerOption(kOptionRestrictedNets, true);
        db.DeleteNetCompletely(saturday);
        db.DeleteNetCompletely(NetIdNamed(&db, "Sunday Net"));
        for (const NetInstance& session : db.GetNetInstancesForNet(tuesday))
        {
            db.DeleteNetInstance(session.id);
        }
        for (const NetInstance& session : db.GetNetInstancesForNet(friday))
        {
            db.DeleteNetInstance(session.id);
        }
        CHECK_EQ(db.GetNetGrants("N4NET").size(), std::size_t{1});
    }

}  // namespace ql
