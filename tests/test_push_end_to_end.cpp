// Federated Logging end to end: pushes run through the system's real scp
// and ssh to a real upstream QuickLogger on this machine, which takes them
// with its own SFTP or legacy SCP and runs import-session. Needs that
// upstream running, so tests/push_end_to_end.sh sets it up and runs these;
// without QL_PUSH_E2E_DIR (every other run) they do nothing.
//
// PushEndToEndSeed makes both databases before the upstream starts;
// PushEndToEndRun pushes and checks what the upstream ended up with.

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/net_slice.hpp"
#include "../src/upstream_push.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static std::string EndToEndDir()
    {
        const char* dir = std::getenv("QL_PUSH_E2E_DIR");
        return dir == nullptr ? std::string() : std::string(dir);
    }

    static std::string UpstreamDbPath(const std::string& dir)
    {
        return dir + "/up/quicklogger.db";
    }

    static std::string LocalDbPath(const std::string& dir)
    {
        return dir + "/local/quicklogger.db";
    }

    // A closed session of `net_id` on `date` with `check_ins` stations.
    static std::int64_t AddClosedSession(Database* db, std::int64_t net_id, const std::string& date,
                                         std::int64_t started_at, int check_ins)
    {
        std::int64_t instance_id = AddTestInstance(db, net_id, date, started_at, "W4KWK");
        AddTestCheckIn(db, instance_id, "W4KWK", 1, kRoleNetControl);
        for (int i = 2; i <= check_ins; ++i)
        {
            AddTestCheckIn(db, instance_id, "K4T" + std::string(1, static_cast<char>('A' + i)), i);
        }
        db->CloseNetInstance(instance_id, started_at + 1800);
        return instance_id;
    }

    QL_TEST(PushEndToEndSeed)
    {
        std::string dir = EndToEndDir();
        if (dir.empty())
        {
            return;
        }
        std::string key = ReadTextFile(dir + "/ssh/key.pub");
        REQUIRE(!key.empty());

        // Its station data reads as loaded, so it never starts downloading.
        Database upstream(UpstreamDbPath(dir));
        MarkAllLoaded(&upstream, static_cast<std::int64_t>(std::time(nullptr)));
        AddTestNet(&upstream, "TAG Skywarn");
        AddTestNet(&upstream, "Dixie Traders");
        User user;
        user.username = "W4KWK";
        user.public_key = key.substr(0, key.find_last_not_of("\r\n") + 1);
        user.amateur_callsign = "W4KWK";
        REQUIRE(upstream.CreateUser(user));
        user.username = "K4VIEW";
        user.amateur_callsign = "K4VIEW";
        user.view_only = true;
        REQUIRE(upstream.CreateUser(user));

        Database local(LocalDbPath(dir));
        std::int64_t tag = AddTestNet(&local, "TAG Skywarn");
        AddClosedSession(&local, tag, "2026-09-15", 1789516800, 4);
        AddClosedSession(&local, tag, "2026-09-22", 1790121600, 6);
        AddClosedSession(&local, AddTestNet(&local, "Dixie Traders Net"), "2026-09-25", 1790380800, 3);
        AddClosedSession(&local, AddTestNet(&local, "220 EOR net"), "2026-09-23", 1790208000, 2);
    }

    // Writes the local session on `date` as a .qlsession, the way a push
    // does, and pushes it as `user`, to `host` on QL_PUSH_E2E_PORT.
    static PushResult PushSession(const std::string& dir, const std::string& date, const std::string& user,
                                  const std::string& confirm_net, const std::string& host = "127.0.0.1",
                                  int port_offset = 0)
    {
        Database local(LocalDbPath(dir));
        std::int64_t instance_id = 0;
        std::string net_name;
        for (const Net& net : local.GetAllNets())
        {
            for (const NetInstance& instance : local.GetNetInstancesForNet(net.id))
            {
                if (instance.instance_date == date)
                {
                    instance_id = instance.id;
                    net_name = net.name;
                }
            }
        }
        PushResult missing;
        missing.message = "no local session on " + date;
        if (instance_id == 0)
        {
            return missing;
        }

        std::string remote_name = SanitizeFilenameComponent(net_name) + "_" + date + ".qlsession";
        std::string local_path = dir + "/local/" + remote_name;
        std::string error;
        if (!WriteNetSliceFile(local_path, GatherSessionSlice(&local, instance_id), &error))
        {
            missing.message = error;
            return missing;
        }
        Upstream upstream;
        upstream.host = host;
        upstream.user = user;
        upstream.port = std::atoi(std::getenv("QL_PUSH_E2E_PORT")) + port_offset;
        return PushSessionFile(upstream, local_path, remote_name, confirm_net, net_name, nullptr);
    }

    // How many check-ins the upstream's `net` has on `date`; -1 if it has
    // no session then.
    static int UpstreamCheckIns(const std::string& dir, const std::string& net, const std::string& date)
    {
        Database upstream(UpstreamDbPath(dir));
        for (const Net& candidate : upstream.GetAllNets())
        {
            if (candidate.name != net)
            {
                continue;
            }
            for (const NetInstance& instance : upstream.GetNetInstancesForNet(candidate.id))
            {
                if (instance.instance_date == date)
                {
                    return static_cast<int>(upstream.GetCheckInsForNetInstance(instance.id).size());
                }
            }
        }
        return -1;
    }

    QL_TEST(PushEndToEndRun)
    {
        std::string dir = EndToEndDir();
        if (dir.empty())
        {
            return;
        }
        REQUIRE(std::getenv("QL_PUSH_E2E_PORT") != nullptr);
        std::string imports = SessionImportsDir(UpstreamDbPath(dir), "W4KWK");

        // An exact net name, over SFTP (scp's default since OpenSSH 9.0).
        PushResult result = PushSession(dir, "2026-09-15", "W4KWK", "");
        CHECK(result.kind == PushResultKind::kPushed);
        CHECK_EQ(result.message, std::string("Pushed to TAG Skywarn on 127.0.0.1."));
        CHECK_EQ(UpstreamCheckIns(dir, "TAG Skywarn", "2026-09-15"), 4);
        CHECK(!FileExists(imports + "/TAG_Skywarn_2026-09-15.qlsession"));

        // Again: harmless.
        result = PushSession(dir, "2026-09-15", "W4KWK", "");
        CHECK(result.kind == PushResultKind::kPushed);
        CHECK_EQ(result.message, std::string("127.0.0.1 already had this session, in TAG Skywarn."));

        // Over legacy SCP, as Windows 10 and 11's scp copies.
        SetTestEnvironment("SCP_EXTRA", "-O");
        result = PushSession(dir, "2026-09-22", "W4KWK", "");
        CHECK(result.kind == PushResultKind::kPushed);
        CHECK_EQ(UpstreamCheckIns(dir, "TAG Skywarn", "2026-09-22"), 6);
        result = PushSession(dir, "2026-09-22", "K4VIEW", "");
        CHECK_EQ(result.message, std::string("Your user on 127.0.0.1 is view-only."));
        SetTestEnvironment("SCP_EXTRA", "");

        // A look-alike net name: asked about, then pushed once confirmed.
        result = PushSession(dir, "2026-09-25", "W4KWK", "");
        CHECK(result.kind == PushResultKind::kNeedsConfirmation);
        CHECK_EQ(result.upstream_net, std::string("Dixie Traders"));
        CHECK_EQ(UpstreamCheckIns(dir, "Dixie Traders", "2026-09-25"), -1);
        result = PushSession(dir, "2026-09-25", "W4KWK", result.upstream_net);
        CHECK(result.kind == PushResultKind::kPushed);
        CHECK_EQ(result.message, std::string("Pushed to Dixie Traders on 127.0.0.1."));
        CHECK_EQ(UpstreamCheckIns(dir, "Dixie Traders", "2026-09-25"), 3);

        // Every way it fails, in one sentence.
        result = PushSession(dir, "2026-09-23", "W4KWK", "");
        CHECK(result.kind == PushResultKind::kFailed);
        CHECK_EQ(result.message, std::string("No net named like 220 EOR net on 127.0.0.1."));
        CHECK_EQ(PushSession(dir, "2026-09-22", "K4VIEW", "").message,
                 std::string("Your user on 127.0.0.1 is view-only."));
        CHECK_EQ(PushSession(dir, "2026-09-22", "K4NOBODY", "").message, std::string("127.0.0.1 refused your key."));
        CHECK_EQ(PushSession(dir, "2026-09-22", "W4KWK", "", "localhost").message,
                 std::string("localhost's host key isn't known here or has changed; log in once with ssh to check "
                             "it."));
        CHECK_EQ(PushSession(dir, "2026-09-22", "W4KWK", "", "127.0.0.1", 1).message,
                 std::string("Couldn't reach 127.0.0.1."));
    }

}  // namespace ql
