// Federated Logging end to end: pushes run through the system's real scp
// and ssh to a real upstream QuickLogger on this machine, which takes them
// with its own SFTP or legacy SCP and runs import-session. Needs that
// upstream running, so tests/ssh_end_to_end.sh sets it up and runs these;
// without QL_PUSH_E2E_DIR (every other run) they do nothing.
//
// PushEndToEndSeed makes both databases before the upstream starts;
// PushEndToEndRun pushes and checks what the upstream ended up with.

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/net_slice.hpp"
#include "../src/sftp_paths.hpp"
#include "../src/upstream_pull.hpp"
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
        // A second ordinary user, for pushes made at the same time as W4KWK's.
        user.username = "K4TWO";
        user.amateur_callsign = "K4TWO";
        user.view_only = false;
        REQUIRE(upstream.CreateUser(user));
        AddTestNet(&upstream, "Concurrent Net");

        Database local(LocalDbPath(dir));
        std::int64_t tag = AddTestNet(&local, "TAG Skywarn");
        AddClosedSession(&local, tag, "2026-09-15", 1789516800, 4);
        AddClosedSession(&local, tag, "2026-09-22", 1790121600, 6);
        std::int64_t dixie = AddTestNet(&local, "Dixie Traders Net");
        AddClosedSession(&local, dixie, "2026-09-25", 1790380800, 3);
        // For the confirmations that don't upload again.
        AddClosedSession(&local, dixie, "2026-09-24", 1790294400, 2);
        AddClosedSession(&local, dixie, "2026-09-26", 1790467200, 2);
        AddClosedSession(&local, dixie, "2026-09-27", 1790553600, 2);
        AddClosedSession(&local, dixie, "2026-09-28", 1790640000, 2);
        AddClosedSession(&local, AddTestNet(&local, "220 EOR net"), "2026-09-23", 1790208000, 2);
        // For pushing whole nets (PushNetEndToEnd): one the upstream lacks, and
        // one it has a session of already.
        AddClosedSession(&local, AddTestNet(&local, "Brand New Net"), "2026-12-01", 1796083200, 3);
        std::int64_t merge = AddTestNet(&local, "Merge Net");
        AddClosedSession(&local, merge, "2026-12-02", 1796169600, 3);
        AddClosedSession(&local, merge, "2026-12-09", 1796774400, 4);
        AddClosedSession(&local, merge, "2026-12-16", 1797379200, 2);
        AddClosedSession(&upstream, AddTestNet(&upstream, "Merge Net"), "2026-12-02", 1796169600, 3);
        // For the pushes made at the same time (PushConcurrentEndToEnd):
        // a session a day from 2026-11-01 to 2026-11-30, 2 to 5 stations.
        std::int64_t concurrent = AddTestNet(&local, "Concurrent Net");
        for (int day = 1; day <= 30; ++day)
        {
            std::string date = std::string("2026-11-") + (day < 10 ? "0" : "") + std::to_string(day);
            AddClosedSession(&local, concurrent, date, 1793491200 + day * 86400, 2 + day % 4);
        }
    }

    // The local database tests/push_screens.py drives the real program on:
    // its sessions are on dates of their own, so the pushes it makes never
    // meet the ones PushEndToEndRun makes.
    QL_TEST(PushScreensSeed)
    {
        std::string dir = EndToEndDir();
        if (dir.empty())
        {
            return;
        }
        Database local(dir + "/local-screens/quicklogger.db");
        MarkAllLoaded(&local, static_cast<std::int64_t>(std::time(nullptr)));
        std::int64_t tag = AddTestNet(&local, "TAG Skywarn");
        AddClosedSession(&local, tag, "2026-10-06", 1791331200, 3);
        AddClosedSession(&local, AddTestNet(&local, "Dixie Traders Net"), "2026-10-07", 1791417600, 2);
        AddClosedSession(&local, AddTestNet(&local, "220 EOR net"), "2026-10-08", 1791504000, 2);
        std::int64_t open = AddTestInstance(&local, tag, "2026-10-13", 1791936000, "W4KWK");
        AddTestCheckIn(&local, open, "W4KWK", 1, kRoleNetControl);
        AddTestCheckIn(&local, open, "K4TB", 2);
    }

    // A local session written as a .qlsession, ready to push.
    struct PreparedSession
    {
        std::string local_path;
        std::string remote_name;
        std::string net_name;
        std::string error;
    };

    // Writes the local session on `date` as a .qlsession, the way a push
    // does; `error` says why not, if it can't.
    static PreparedSession PrepareSession(const std::string& dir, const std::string& date)
    {
        PreparedSession prepared;
        Database local(LocalDbPath(dir));
        std::int64_t instance_id = 0;
        for (const Net& net : local.GetAllNets())
        {
            for (const NetInstance& instance : local.GetNetInstancesForNet(net.id))
            {
                if (instance.instance_date == date)
                {
                    instance_id = instance.id;
                    prepared.net_name = net.name;
                }
            }
        }
        if (instance_id == 0)
        {
            prepared.error = "no local session on " + date;
            return prepared;
        }
        prepared.remote_name = SanitizeFilenameComponent(prepared.net_name) + "_" + date + ".qlsession";
        prepared.local_path = dir + "/local/" + prepared.remote_name;
        std::string error;
        if (!WriteNetSliceFile(prepared.local_path, GatherSessionSlice(&local, instance_id), &error))
        {
            prepared.error = error;
        }
        return prepared;
    }

    static Upstream UpstreamFor(const std::string& user, const std::string& host = "127.0.0.1", int port_offset = 0)
    {
        Upstream upstream;
        upstream.host = host;
        upstream.user = user;
        upstream.port = std::atoi(std::getenv("QL_PUSH_E2E_PORT")) + port_offset;
        return upstream;
    }

    // Pushes the local session on `date` as `user`, to `host` on
    // QL_PUSH_E2E_PORT.
    static PushResult PushSession(const std::string& dir, const std::string& date, const std::string& user,
                                  const std::string& confirm_net, const std::string& host = "127.0.0.1",
                                  int port_offset = 0)
    {
        PreparedSession prepared = PrepareSession(dir, date);
        if (!prepared.error.empty())
        {
            PushResult missing;
            missing.message = prepared.error;
            return missing;
        }
        return PushSessionFile(UpstreamFor(user, host, port_offset), prepared.local_path, prepared.remote_name,
                               confirm_net, prepared.net_name, nullptr);
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

        // Confirming doesn't upload again: with the local file gone, it
        // still pushes, from what the upstream kept when it asked.
        result = PushSession(dir, "2026-09-24", "W4KWK", "");
        CHECK(result.kind == PushResultKind::kNeedsConfirmation);
        {
            Upstream upstream;
            upstream.host = "127.0.0.1";
            upstream.user = "W4KWK";
            upstream.port = std::atoi(std::getenv("QL_PUSH_E2E_PORT"));
            std::string remote_name = "Dixie_Traders_Net_2026-09-24.qlsession";
            CHECK(FileExists(SessionImportsDir(UpstreamDbPath(dir), "W4KWK") + "/" + remote_name));
            result = PushSessionFile(upstream, dir + "/local/no-such-file.qlsession", remote_name, "Dixie Traders",
                                     "Dixie Traders Net", nullptr);
            CHECK(result.kind == PushResultKind::kPushed);
            CHECK_EQ(UpstreamCheckIns(dir, "Dixie Traders", "2026-09-24"), 2);

            // If the upstream has cleared it meanwhile, it is copied again
            // (here from a file that exists), once.
            result = PushSession(dir, "2026-09-26", "W4KWK", "");
            CHECK(result.kind == PushResultKind::kNeedsConfirmation);
            std::filesystem::remove(SessionImportsDir(UpstreamDbPath(dir), "W4KWK") +
                                    "/Dixie_Traders_Net_2026-09-26.qlsession");
            result = PushSession(dir, "2026-09-26", "W4KWK", "Dixie Traders");
            CHECK(result.kind == PushResultKind::kPushed);
            CHECK_EQ(UpstreamCheckIns(dir, "Dixie Traders", "2026-09-26"), 2);

            // Gone, and nothing to copy again: it says so, once, without looping.
            result = PushSession(dir, "2026-09-27", "W4KWK", "");
            CHECK(result.kind == PushResultKind::kNeedsConfirmation);
            std::filesystem::remove(SessionImportsDir(UpstreamDbPath(dir), "W4KWK") +
                                    "/Dixie_Traders_Net_2026-09-27.qlsession");
            result = PushSessionFile(upstream, dir + "/local/no-such-file.qlsession",
                                     "Dixie_Traders_Net_2026-09-27.qlsession", "Dixie Traders", "Dixie Traders Net",
                                     nullptr);
            CHECK(result.kind == PushResultKind::kFailed);
            CHECK_EQ(UpstreamCheckIns(dir, "Dixie Traders", "2026-09-27"), -1);
        }

        // A full /imports is named over SFTP as over legacy SCP: OpenSSH's
        // scp shows only the server's reason on standard error.
        std::vector<std::string> fillers;
        for (int i = 1; i <= 4; ++i)
        {
            fillers.push_back(imports + "/Filler" + std::to_string(i) + ".qlnet");
            WriteTextFile(fillers.back(), "");
            std::filesystem::resize_file(fillers.back(), kSftpMaxUploadBytes);
        }
        for (const char* extra : {"", "-O"})
        {
            SetTestEnvironment("SCP_EXTRA", extra);
            CHECK_EQ(PushSession(dir, "2026-09-22", "W4KWK", "").message,
                     std::string("Your /imports on 127.0.0.1 is full."));
        }
        SetTestEnvironment("SCP_EXTRA", "");
        for (const std::string& filler : fillers)
        {
            std::filesystem::remove(filler);
        }

        // A look-alike that's declined: the client has the upstream delete
        // the upload it kept to ask about.
        result = PushSession(dir, "2026-09-28", "W4KWK", "");
        CHECK(result.kind == PushResultKind::kNeedsConfirmation);
        CHECK(FileExists(imports + "/Dixie_Traders_Net_2026-09-28.qlsession"));
        {
            Upstream declined;
            declined.host = "127.0.0.1";
            declined.user = "W4KWK";
            declined.port = std::atoi(std::getenv("QL_PUSH_E2E_PORT"));
            DiscardUpstreamUpload(declined, "Dixie_Traders_Net_2026-09-28.qlsession", nullptr);
            CHECK(!FileExists(imports + "/Dixie_Traders_Net_2026-09-28.qlsession"));
            // Again, with nothing there, is harmless.
            DiscardUpstreamUpload(declined, "Dixie_Traders_Net_2026-09-28.qlsession", nullptr);
        }
        CHECK_EQ(UpstreamCheckIns(dir, "Dixie Traders", "2026-09-28"), -1);

        // Every way it fails, in one sentence; none leaves its upload on the
        // upstream.
        result = PushSession(dir, "2026-09-23", "W4KWK", "");
        CHECK(result.kind == PushResultKind::kFailed);
        CHECK_EQ(result.message, std::string("No net named like 220 EOR net on 127.0.0.1."));
        CHECK(!FileExists(imports + "/220_EOR_net_2026-09-23.qlsession"));
        CHECK_EQ(PushSession(dir, "2026-09-22", "K4VIEW", "").message,
                 std::string("Your user on 127.0.0.1 is view-only."));
        CHECK_EQ(PushSession(dir, "2026-09-22", "K4NOBODY", "").message, std::string("127.0.0.1 refused your key."));
        CHECK_EQ(PushSession(dir, "2026-09-22", "W4KWK", "", "localhost").message,
                 std::string("localhost's host key isn't known here or has changed; log in once with ssh to check "
                             "it."));
        CHECK_EQ(PushSession(dir, "2026-09-22", "W4KWK", "", "127.0.0.1", 1).message,
                 std::string("Couldn't reach 127.0.0.1."));
    }

    // How many sessions the upstream's `net` has on `date`.
    static int UpstreamSessionsOn(const std::string& dir, const std::string& net, const std::string& date)
    {
        Database upstream(UpstreamDbPath(dir));
        int sessions = 0;
        for (const Net& candidate : upstream.GetAllNets())
        {
            if (candidate.name != net)
            {
                continue;
            }
            for (const NetInstance& instance : upstream.GetNetInstancesForNet(candidate.id))
            {
                sessions += instance.instance_date == date ? 1 : 0;
            }
        }
        return sessions;
    }

    // One push to start on its own thread: the result is kept for the main
    // thread, since the test framework's checks belong there.
    struct ConcurrentPush
    {
        std::string date;
        std::string user;
        PreparedSession prepared;
        PushResult result;
    };

    // Starts every push at once (after all are prepared) and waits for them.
    static void PushAllAtOnce(const std::string& dir, std::vector<ConcurrentPush>* pushes)
    {
        for (ConcurrentPush& push : *pushes)
        {
            push.prepared = PrepareSession(dir, push.date);
            REQUIRE(push.prepared.error.empty());
        }
        std::vector<std::thread> threads;
        for (ConcurrentPush& push : *pushes)
        {
            ConcurrentPush* mine = &push;
            threads.emplace_back(
                [dir, mine]()
                {
                    mine->result = PushSessionFile(UpstreamFor(mine->user), mine->prepared.local_path,
                                                   mine->prepared.remote_name, "", mine->prepared.net_name, nullptr);
                });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
    }

    // Real ssh and scp clients pushing to one upstream at the same moment.
    // Pulling (upstream_pull.hpp) through the real ssh and scp, after the
    // pushes above have given the upstream's TAG Skywarn two sessions: the
    // list of nets, a net's sessions and the net itself, as a view-only user
    // (who may read) and over SFTP and legacy SCP alike.
    QL_TEST(PullEndToEnd)
    {
        std::string dir = EndToEndDir();
        if (dir.empty())
        {
            return;
        }
        REQUIRE(std::getenv("QL_PUSH_E2E_PORT") != nullptr);

        PullResult list = PullNetList(UpstreamFor("K4VIEW"), nullptr);
        REQUIRE(list.kind == PullResultKind::kNets);
        bool has_tag = false;
        for (const UpstreamNet& net : list.nets)
        {
            if (net.name == "TAG Skywarn")
            {
                has_tag = true;
                CHECK_EQ(net.sessions, 2);
                CHECK_EQ(net.service, std::string("amateur"));
            }
        }
        CHECK(has_tag);

        for (const char* extra : {"", "-O"})
        {
            SetTestEnvironment("SCP_EXTRA", extra);
            std::string pulled = dir + "/pulled" + extra;
            PullResult sessions = PullNetFiles(UpstreamFor("K4VIEW"), "TAG Skywarn", true, pulled, nullptr);
            REQUIRE(sessions.kind == PullResultKind::kFiles);
            CHECK_EQ(sessions.net, std::string("TAG Skywarn"));
            REQUIRE(sessions.files.size() == 2);
            std::string error;
            std::optional<NetSlice> newest = ReadSessionSliceFile(sessions.files[0], &error);
            REQUIRE(newest.has_value());
            CHECK_EQ(newest->instances[0].instance_date, std::string("2026-09-22"));
            CHECK_EQ(newest->check_ins.size(), std::size_t(6));
            std::optional<NetSlice> oldest = ReadSessionSliceFile(sessions.files[1], &error);
            REQUIRE(oldest.has_value());
            CHECK_EQ(oldest->check_ins.size(), std::size_t(4));

            PullResult net = PullNetFiles(UpstreamFor("W4KWK"), "tag skywarn", false, pulled, nullptr);
            REQUIRE(net.kind == PullResultKind::kFiles);
            REQUIRE(net.files.size() == 1);
            std::optional<NetSlice> slice = ReadNetSliceFile(net.files[0], &error);
            REQUIRE(slice.has_value());
            CHECK_EQ(slice->net.name, std::string("TAG Skywarn"));
            CHECK_EQ(slice->instances.size(), std::size_t(2));
        }
        SetTestEnvironment("SCP_EXTRA", "");

        // A net it hasn't got, and a user it doesn't know.
        PullResult missing = PullNetFiles(UpstreamFor("K4VIEW"), "No Such Net", true, dir + "/pulled-none", nullptr);
        CHECK(missing.kind == PullResultKind::kFailed);
        CHECK_EQ(missing.message, std::string("127.0.0.1 has no net named No Such Net any more."));
        CHECK_EQ(PullNetList(UpstreamFor("nobody"), nullptr).message, std::string("127.0.0.1 refused your key."));
    }

    // Pushing whole nets (import-net) through the real ssh and scp: one the
    // upstream lacks is added, and one it has is merged into once confirmed.
    QL_TEST(PushNetEndToEnd)
    {
        std::string dir = EndToEndDir();
        if (dir.empty())
        {
            return;
        }
        REQUIRE(std::getenv("QL_PUSH_E2E_PORT") != nullptr);
        Database local(LocalDbPath(dir));
        std::string error;
        PushResult result;
        for (const char* name : {"Brand New Net", "Merge Net"})
        {
            std::int64_t net_id = 0;
            for (const Net& net : local.GetAllNets())
            {
                net_id = net.name == name ? net.id : net_id;
            }
            REQUIRE(net_id != 0);
            std::string remote_name = SanitizeFilenameComponent(name) + ".qlnet";
            std::string local_path = dir + "/local/" + remote_name;
            REQUIRE(WriteNetSliceFile(local_path, GatherNetSlice(&local, net_id), &error));

            result = PushSessionFile(UpstreamFor("W4KWK"), local_path, remote_name, "", name, nullptr, true);
            if (std::string(name) == "Brand New Net")
            {
                CHECK(result.kind == PushResultKind::kPushed);
                CHECK_EQ(result.message, std::string("Pushed to Brand New Net on 127.0.0.1."));
                CHECK_EQ(UpstreamCheckIns(dir, name, "2026-12-01"), 3);
                continue;
            }
            // Its own name still asks, and says what merging would add.
            CHECK(result.kind == PushResultKind::kNeedsConfirmation);
            CHECK_EQ(result.upstream_net, std::string("Merge Net"));
            CHECK_EQ(result.merge_summary, std::string("adds 2 sessions and 0 saved stations"));
            CHECK_EQ(UpstreamCheckIns(dir, name, "2026-12-09"), -1);
            result = PushSessionFile(UpstreamFor("W4KWK"), local_path, remote_name, "Merge Net", name, nullptr, true);
            CHECK(result.kind == PushResultKind::kPushed);
            CHECK_EQ(result.message,
                     std::string("Merged into Merge Net on 127.0.0.1: it adds 2 sessions and 0 saved stations."));
            CHECK_EQ(UpstreamCheckIns(dir, name, "2026-12-09"), 4);
            CHECK_EQ(UpstreamCheckIns(dir, name, "2026-12-16"), 2);

            // Again: nothing left to add.
            result = PushSessionFile(UpstreamFor("W4KWK"), local_path, remote_name, "Merge Net", name, nullptr, true);
            CHECK_EQ(result.message, std::string("Merge Net on 127.0.0.1 already has all of this net."));
        }

        // A view-only user can't push a net.
        std::string remote_name = "Brand_New_Net.qlnet";
        result = PushSessionFile(UpstreamFor("K4VIEW"), dir + "/local/" + remote_name, remote_name, "", "Brand New Net",
                                 nullptr, true);
        CHECK_EQ(result.message, std::string("Your user on 127.0.0.1 is view-only."));
    }

    QL_TEST(PushConcurrentEndToEnd)
    {
        std::string dir = EndToEndDir();
        if (dir.empty())
        {
            return;
        }
        REQUIRE(std::getenv("QL_PUSH_E2E_PORT") != nullptr);

        // Eight different sessions of one net, four from each of two users.
        std::vector<ConcurrentPush> different;
        for (int day = 1; day <= 8; ++day)
        {
            ConcurrentPush push;
            push.date = std::string("2026-11-0") + std::to_string(day);
            push.user = day % 2 == 0 ? "W4KWK" : "K4TWO";
            different.push_back(push);
        }
        PushAllAtOnce(dir, &different);
        for (const ConcurrentPush& push : different)
        {
            CHECK(push.result.kind == PushResultKind::kPushed);
            CHECK_EQ(push.result.message, std::string("Pushed to Concurrent Net on 127.0.0.1."));
            CHECK_EQ(UpstreamSessionsOn(dir, "Concurrent Net", push.date), 1);
            CHECK(!FileExists(SessionImportsDir(UpstreamDbPath(dir), push.user) + "/" + push.prepared.remote_name));
        }

        // The same session from two clients of one user, several times over
        // (the two share one name in their /imports). Whichever import runs
        // second may find the upload already taken and deleted by the first;
        // it is then told the upstream refused the session. The session must
        // still arrive exactly once, with nothing left in /imports.
        for (int round = 0; round < 5; ++round)
        {
            std::vector<ConcurrentPush> same;
            for (int copy = 0; copy < 2; ++copy)
            {
                ConcurrentPush push;
                push.date = std::string("2026-11-") + std::to_string(11 + round);
                push.user = "W4KWK";
                same.push_back(push);
            }
            PushAllAtOnce(dir, &same);
            int pushed = 0;
            for (const ConcurrentPush& push : same)
            {
                pushed += push.result.kind == PushResultKind::kPushed ? 1 : 0;
                CHECK(push.result.kind == PushResultKind::kPushed ||
                      push.result.message == "127.0.0.1 refused the session.");
            }
            CHECK(pushed >= 1);
            CHECK_EQ(UpstreamSessionsOn(dir, "Concurrent Net", same[0].date), 1);
            CHECK(!FileExists(SessionImportsDir(UpstreamDbPath(dir), "W4KWK") + "/" + same[0].prepared.remote_name));
        }

        // The same session from two different users, several times over.
        for (int round = 0; round < 5; ++round)
        {
            std::vector<ConcurrentPush> same;
            for (const char* user : {"W4KWK", "K4TWO"})
            {
                ConcurrentPush push;
                push.date = std::string("2026-11-") + std::to_string(21 + round);
                push.user = user;
                same.push_back(push);
            }
            PushAllAtOnce(dir, &same);
            for (const ConcurrentPush& push : same)
            {
                CHECK(push.result.kind == PushResultKind::kPushed);
            }
            CHECK_EQ(UpstreamSessionsOn(dir, "Concurrent Net", same[0].date), 1);
            CHECK_EQ(UpstreamCheckIns(dir, "Concurrent Net", same[0].date), 2 + (21 + round) % 4);
        }
    }

}  // namespace ql
