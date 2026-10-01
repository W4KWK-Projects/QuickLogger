// Exporting one net to a .qlnet file and importing it into another
// database.

#include <optional>
#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/net_slice.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // A net with a saved station, a station that checked in but was never
    // saved, and two sessions -- one with a designated Logger.
    static std::int64_t BuildSourceNet(Database* db)
    {
        Net net;
        net.name = "Skywarn";
        net.mode = "FM";
        net.default_frequency = "146.940";
        net.repeater_offset = "-0.6";
        net.pl_tone = "100.0";
        net.recurrence_description = "Tuesdays 8pm";
        net.created_at = 1700000000;
        std::int64_t net_id = db->CreateNet(net);

        Station saved = MakeStation("K4SAV", "Sam Saved", "37415", "Chattanooga");
        saved.member_id = "SP-12";
        db->SaveNetStation(net_id, saved, "mobile", 1);

        std::int64_t first = AddTestInstance(db, net_id, "2026-01-06", 1000, "W4KWK");
        std::int64_t second = AddTestInstance(db, net_id, "2026-01-13", 2000, "W4KWK");
        db->CloseNetInstance(first, 1500);
        db->SetNetInstanceNotes(first, "Tornado damage reports.\nSecond line.");
        AddTestCheckIn(db, first, "K4SAV", 1);
        AddTestCheckIn(db, first, "K4UNS", 2, kRoleLogger);
        AddTestCheckIn(db, second, "K4SAV", 1);
        Station unsaved = MakeStation("K4UNS", "Una Unsaved");
        db->RecordManualCheckInStation(unsaved, 1);

        // Something that isn't part of this net, and mustn't be exported.
        std::int64_t other = AddTestNet(db, "Other Net");
        AddTestCheckIn(db, AddTestInstance(db, other, "2026-01-01", 1, "W4KWK"), "K4NOT", 1);
        return net_id;
    }

    QL_TEST(AnOlderNetsFreeTextLocationImportsAsAZip)
    {
        TempDir dir;
        Database dest(dir.File("dest.db"));
        NetSlice slice;
        slice.net.name = "Old Export";
        slice.net.default_location = "Chattanooga, TN 37415";
        std::int64_t with_zip = ApplyNetSlice(&dest, slice, 1800000000);
        CHECK_EQ(dest.GetNetById(with_zip)->default_location, std::string("37415"));
        slice.net.default_location = "Chattanooga";
        std::int64_t without_zip = ApplyNetSlice(&dest, slice, 1800000000);
        CHECK(dest.GetNetById(without_zip)->default_location.empty());
    }

    QL_TEST(AnOlderNetsFreeTextFrequencyImportsIntoComments)
    {
        TempDir dir;
        Database dest(dir.File("dest.db"));
        NetSlice slice;
        slice.net.name = "Old Export";
        slice.net.default_frequency = "146.940 (W4AM)";
        std::int64_t net_id = ApplyNetSlice(&dest, slice, 1800000000);
        CHECK_EQ(dest.GetNetById(net_id)->default_frequency, std::string("146.940"));
        CHECK_EQ(dest.GetNetById(net_id)->comments, std::string("Frequency: 146.940 (W4AM)"));
    }

    QL_TEST(AnOlderNetsFreeTextModeImportsAsAKnownModeOrBlank)
    {
        TempDir dir;
        Database dest(dir.File("dest.db"));
        NetSlice slice;
        slice.net.name = "Old Export";
        slice.net.mode = "ysf";
        std::int64_t fusion = ApplyNetSlice(&dest, slice, 1800000000);
        CHECK_EQ(dest.GetNetById(fusion)->mode, std::string("Fusion"));
        slice.net.name = "Older Export";
        slice.net.mode = "Digital";
        std::int64_t unknown = ApplyNetSlice(&dest, slice, 1800000000);
        CHECK_EQ(dest.GetNetById(unknown)->mode, std::string(""));
    }

    QL_TEST(ANetRoundTripsThroughAFile)
    {
        TempDir dir;
        std::string file = dir.File("exports/Skywarn.qlnet");
        {
            Database source(dir.File("source.db"));
            std::int64_t net_id = BuildSourceNet(&source);
            std::string error;
            REQUIRE(WriteNetSliceFile(file, GatherNetSlice(&source, net_id), &error));
        }
        CHECK(!FileExists(file + "-wal"));

        std::string error;
        std::optional<NetSlice> slice = ReadNetSliceFile(file, &error);
        REQUIRE(slice.has_value());
        CHECK_EQ(slice->net.created_at, std::int64_t{1700000000});
        CHECK_EQ(slice->net.imported_at, std::int64_t{0});  // Never imported yet.

        // Into a database that already has a net of its own (so ids collide).
        Database dest(dir.File("dest.db"));
        std::int64_t existing = AddTestNet(&dest, "Existing");
        AddTestCheckIn(&dest, AddTestInstance(&dest, existing, "2025-12-01", 1, "N0ONE"), "N0ONE", 1);
        std::int64_t imported = ApplyNetSlice(&dest, *slice, 1800000000);
        CHECK(imported != existing);

        std::optional<Net> net = dest.GetNetById(imported);
        REQUIRE(net.has_value());
        CHECK_EQ(net->name, std::string("Skywarn"));
        CHECK_EQ(net->default_frequency, std::string("146.940"));
        CHECK_EQ(net->repeater_offset, std::string("-0.6"));
        CHECK_EQ(net->pl_tone, std::string("100.0"));
        CHECK_EQ(net->recurrence_description, std::string("Tuesdays 8pm"));
        CHECK_EQ(net->created_at, std::int64_t{1700000000});
        CHECK_EQ(net->imported_at, std::int64_t{1800000000});

        std::vector<Station> saved = dest.GetSavedStationsForNet(imported);
        REQUIRE(saved.size() == 1);
        CHECK_EQ(saved[0].member_id, std::string("SP-12"));
        CHECK_EQ(dest.GetSavedNetStationRemarks(imported, "K4SAV"), std::string("mobile"));

        std::vector<NetInstance> sessions = dest.GetNetInstancesForNet(imported);
        REQUIRE(sessions.size() == 2);
        CHECK_EQ(sessions[0].instance_date, std::string("2026-01-13"));
        CHECK_EQ(sessions[1].started_at, std::int64_t{1000});
        CHECK(sessions[1].status == NetInstanceStatus::kClosed);
        CHECK_EQ(sessions[1].notes, std::string("Tornado damage reports.\nSecond line."));
        CHECK_EQ(sessions[0].notes, std::string(""));
        std::vector<CheckIn> check_ins = dest.GetCheckInsForNetInstance(sessions[1].id);
        REQUIRE(check_ins.size() == 2);
        CHECK_EQ(check_ins[1].callsign, std::string("K4UNS"));
        CHECK_EQ(check_ins[1].designated_role, kRoleLogger);

        // The unsaved station's details came along for its check-in.
        std::optional<Station> unsaved = dest.FindStationByCallsign("K4UNS");
        REQUIRE(unsaved.has_value());
        CHECK_EQ(unsaved->name, std::string("Una Unsaved"));
        CHECK(!dest.FindStationByCallsign("K4NOT").has_value());
        // The existing net is untouched.
        CHECK_EQ(dest.GetNetInstancesForNet(existing).size(), std::size_t{1});
    }

    QL_TEST(ReExportingOverwritesTheOldFile)
    {
        TempDir dir;
        std::string file = dir.File("net.qlnet");
        Database source(dir.File("source.db"));
        std::int64_t net_id = BuildSourceNet(&source);
        std::string error;
        REQUIRE(WriteNetSliceFile(file, GatherNetSlice(&source, net_id), &error));
        REQUIRE(WriteNetSliceFile(file, GatherNetSlice(&source, net_id), &error));
        std::optional<NetSlice> slice = ReadNetSliceFile(file, &error);
        REQUIRE(slice.has_value());
        CHECK_EQ(slice->instances.size(), std::size_t{2});
    }

    QL_TEST(ReadingSomethingThatIsntANetExportFails)
    {
        TempDir dir;
        std::string error;

        WriteTextFile(dir.File("text.qlnet"), "definitely not a database, just some text......");
        CHECK(!ReadNetSliceFile(dir.File("text.qlnet"), &error).has_value());
        CHECK(!error.empty());

        {
            Database empty(dir.File("empty.qlnet"));
        }
        error.clear();
        CHECK(!ReadNetSliceFile(dir.File("empty.qlnet"), &error).has_value());
        CHECK(error.find("no net") != std::string::npos);

        {
            Database whole(dir.File("whole.qlnet"));
            AddTestNet(&whole, "One");
            AddTestNet(&whole, "Two");
        }
        error.clear();
        CHECK(!ReadNetSliceFile(dir.File("whole.qlnet"), &error).has_value());
        CHECK(error.find("more than one") != std::string::npos);
    }

    // ---- Merging a .qlnet into a net here --------------------------------

    QL_TEST(SessionsAreTheSameWhenTheirTimesOverlap)
    {
        NetInstance a;
        a.instance_date = "2026-09-24";
        a.started_at = 10000;
        a.status = NetInstanceStatus::kClosed;
        a.closed_at = 13600;
        NetInstance b = a;
        b.started_at = 13000;  // Starts before the first ends.
        b.closed_at = 15000;
        CHECK(SameSession(a, {}, b, {}));
        b.started_at = 14000;  // Starts after it ends.
        CHECK(!SameSession(a, {}, b, {}));
        // Without an end, starts within 30 minutes.
        b.status = NetInstanceStatus::kOpen;
        b.closed_at = 0;
        b.started_at = 10000 + 29 * 60;
        CHECK(SameSession(a, {}, b, {}));
        b.started_at = 10000 + 31 * 60;
        CHECK(!SameSession(a, {}, b, {}));
        // Without start times: the same date and check-ins.
        CheckIn one;
        one.callsign = "k4aaa";
        CheckIn other = one;
        other.callsign = "K4AAA";
        a.started_at = 0;
        b.started_at = 0;
        CHECK(SameSession(a, {one}, b, {other}));
        other.remarks = "mobile";
        CHECK(!SameSession(a, {one}, b, {other}));
        CHECK(!SameSession(a, {}, b, {other}));
    }

    QL_TEST(AMergeAddsOnlyWhatTheNetDoesntHave)
    {
        TempDir dir;
        // The master: a net with one session and two saved stations.
        Database master(dir.File("master.db"));
        std::int64_t net_id = BuildSourceNet(&master);  // Two sessions, K4SAV saved.
        std::string file = dir.File("Skywarn.qlnet");
        std::string error;
        REQUIRE(WriteNetSliceFile(file, GatherNetSlice(&master, net_id), &error));

        // A local copy, logged during an outage: a new session, a new saved
        // station, a detail the master doesn't have, and a remark changed.
        {
            Database local(dir.File("local.db"));
            std::int64_t local_net = ApplyNetSlice(&local, *ReadNetSliceFile(file, &error), 1);
            Station sam = MakeStation("K4SAV", "Someone Else", "37415", "Chattanooga");
            sam.grid_square = "EM75";
            local.UpdateSavedNetStation(local_net, sam, "changed remark", 2);
            local.SaveNetStation(local_net, MakeStation("K4NEW", "Nell New"), "new here", 2);
            // A week after the last one (BuildSourceNet's start at 1000 and 2000).
            std::int64_t outage = AddTestInstance(&local, local_net, "2026-01-20", 2000 + 7 * 24 * 3600, "W4KWK");
            AddTestCheckIn(&local, outage, "K4NEW", 1);
            AddTestCheckIn(&local, outage, "K4SAV", 2);
            local.SetNetInstanceNotes(outage, "Logged during the outage.");
            REQUIRE(WriteNetSliceFile(dir.File("back.qlnet"), GatherNetSlice(&local, local_net), &error));
        }

        // Back on the master.
        std::optional<NetSlice> back = ReadNetSliceFile(dir.File("back.qlnet"), &error);
        REQUIRE(back.has_value());
        NetMergePlan plan = PlanNetMerge(&master, *back, net_id);
        REQUIRE(plan.sessions.size() == 3);
        int added = 0;
        int here = 0;
        for (const MergeSession& session : plan.sessions)
        {
            added += session.kind == MergeSessionKind::kNew ? 1 : 0;
            here += session.kind == MergeSessionKind::kAlreadyHere ? 1 : 0;
        }
        CHECK_EQ(added, 1);
        CHECK_EQ(here, 2);
        CHECK_EQ(plan.new_saved_stations, 1);
        CHECK_EQ(plan.known_saved_stations, 1);

        NetMergeResult result = ApplyNetMerge(&master, *back, plan);
        CHECK_EQ(result.sessions_added, 1);
        CHECK_EQ(result.sessions_replaced, 0);
        CHECK_EQ(result.saved_stations_added, 1);
        std::vector<NetInstance> sessions = master.GetNetInstancesForNet(net_id);
        REQUIRE(sessions.size() == 3);
        CHECK_EQ(sessions[0].instance_date, std::string("2026-01-20"));
        CHECK_EQ(sessions[0].notes, std::string("Logged during the outage."));
        CHECK_EQ(master.GetCheckInsForNetInstance(sessions[0].id).size(), std::size_t{2});
        // The new station comes with its remarks; the master's own station
        // keeps its details and remarks, gaining only the grid it lacked.
        CHECK_EQ(master.GetSavedNetStationRemarks(net_id, "K4NEW"), std::string("new here"));
        CHECK_EQ(master.GetSavedNetStationRemarks(net_id, "K4SAV"), std::string("mobile"));
        std::optional<Station> sam = master.FindStationByCallsign("K4SAV");
        REQUIRE(sam.has_value());
        CHECK_EQ(sam->name, std::string("Sam Saved"));
        CHECK_EQ(sam->grid_square, std::string("EM75"));

        // Merging the same file again adds nothing.
        NetMergePlan again = PlanNetMerge(&master, *back, net_id);
        NetMergeResult nothing = ApplyNetMerge(&master, *back, again);
        CHECK_EQ(nothing.sessions_added + nothing.saved_stations_added, 0);
        CHECK_EQ(master.GetNetInstancesForNet(net_id).size(), std::size_t{3});
    }

    QL_TEST(ASessionThatDiffersIsKeptUnlessReplaced)
    {
        TempDir dir;
        Database master(dir.File("master.db"));
        std::int64_t net_id = BuildSourceNet(&master);
        NetSlice slice = GatherNetSlice(&master, net_id);
        // The file's copy of the closed session has another check-in, and
        // its open one is open there too.
        std::int64_t closed_id = slice.instances[1].id;
        CheckIn extra;
        extra.net_instance_id = closed_id;
        extra.callsign = "K4XTR";
        extra.sequence_number = 3;
        extra.checked_in_at = 1400;
        slice.check_ins.push_back(extra);

        NetMergePlan plan = PlanNetMerge(&master, slice, net_id);
        REQUIRE(plan.sessions.size() == 2);
        CHECK(plan.sessions[1].kind == MergeSessionKind::kDiffers);
        CHECK_EQ(plan.sessions[1].file_check_ins, 3);
        CHECK_EQ(plan.sessions[1].local_check_ins, 2);
        CHECK(plan.sessions[1].check_ins_differ);
        CHECK(!plan.sessions[1].notes_differ);
        CHECK(!plan.sessions[1].replace);  // Keep, unless chosen.
        // An open session here is never replaced.
        CHECK(plan.sessions[0].kind == MergeSessionKind::kAlreadyHere);
        CHECK(plan.sessions[0].local_open);

        // Kept: nothing changes.
        ApplyNetMerge(&master, slice, plan);
        std::vector<NetInstance> sessions = master.GetNetInstancesForNet(net_id);
        REQUIRE(sessions.size() == 2);
        CHECK_EQ(master.GetCheckInsForNetInstance(sessions[1].id).size(), std::size_t{2});

        // Replaced: the file's copy takes its place.
        plan = PlanNetMerge(&master, slice, net_id);
        plan.sessions[1].replace = true;
        NetMergeResult result = ApplyNetMerge(&master, slice, plan);
        CHECK_EQ(result.sessions_replaced, 1);
        sessions = master.GetNetInstancesForNet(net_id);
        REQUIRE(sessions.size() == 2);
        CHECK_EQ(master.GetCheckInsForNetInstance(sessions[1].id).size(), std::size_t{3});
        CHECK(master.FindStationByCallsign("K4XTR").has_value());
    }

    QL_TEST(StationsWhoseDetailsDifferAreListedAndReplacedOnlyIfChosen)
    {
        TempDir dir;
        Database here(dir.File("here.db"));
        std::int64_t net_id = AddTestNet(&here, "Skywarn");
        Station mine = MakeStation("K4AAA", "Ann Able", "37415", "CHATTANOOGA");
        mine.member_id = "SP-1";
        here.SaveNetStation(net_id, mine, "mine", 1);
        here.SaveNetStation(net_id, MakeStation("K4BBB", "Bob"), "", 1);

        NetSlice slice;
        slice.net.name = "Skywarn";
        NetSliceSavedStation theirs;
        theirs.station = MakeStation("K4AAA", "Ann Able", "37415", "Chattanooga");
        theirs.station.member_id = "SP-9";
        theirs.station.grid_square = "EM75";  // Blank here: filled in, not asked.
        slice.saved_stations.push_back(theirs);
        NetSliceSavedStation same;
        same.station = MakeStation("K4BBB", "BOB");  // Case only: no conflict.
        slice.saved_stations.push_back(same);

        NetMergePlan plan = PlanNetMerge(&here, slice, net_id);
        REQUIRE(plan.station_conflicts.size() == 1);
        CHECK_EQ(plan.station_conflicts[0].file_station->callsign, std::string("K4AAA"));
        REQUIRE(plan.station_conflicts[0].differences.size() == 1);
        CHECK_EQ(std::string(plan.station_conflicts[0].differences[0].field), std::string("member ID"));

        // Kept: the member ID stays; the blank grid is still filled in.
        ApplyNetMerge(&here, slice, plan);
        std::optional<Station> kept = here.FindStationByCallsign("K4AAA");
        CHECK_EQ(kept->member_id, std::string("SP-1"));
        CHECK_EQ(kept->grid_square, std::string("EM75"));
        CHECK_EQ(kept->city, std::string("CHATTANOOGA"));

        // Replaced: the file's member ID; its blanks erase nothing.
        plan = PlanNetMerge(&here, slice, net_id);
        REQUIRE(plan.station_conflicts.size() == 1);
        plan.station_conflicts[0].replace = true;
        NetMergeResult result = ApplyNetMerge(&here, slice, plan);
        CHECK_EQ(result.stations_replaced, 1);
        std::optional<Station> replaced = here.FindStationByCallsign("K4AAA");
        CHECK_EQ(replaced->member_id, std::string("SP-9"));
        CHECK_EQ(replaced->name, std::string("Ann Able"));
        // Only what was listed changes: not the city, which differed in
        // case only.
        CHECK_EQ(replaced->city, std::string("CHATTANOOGA"));
        CHECK_EQ(here.GetSavedNetStationRemarks(net_id, "K4AAA"), std::string("mine"));
        CHECK(PlanNetMerge(&here, slice, net_id).station_conflicts.empty());
    }

    QL_TEST(ASessionOpenInTheFileIsMergedClosed)
    {
        TempDir dir;
        Database master(dir.File("master.db"));
        std::int64_t net_id = AddTestNet(&master, "Skywarn");

        Database other(dir.File("other.db"));
        std::int64_t other_net = AddTestNet(&other, "Skywarn");
        // Started just before its check-ins (AddTestCheckIn times them 1000 + #).
        std::int64_t open = AddTestInstance(&other, other_net, "2026-02-03", 900, "W4KWK");
        AddTestCheckIn(&other, open, "K4AAA", 1);
        AddTestCheckIn(&other, open, "K4BBB", 4);  // A gap from deletions.
        NetSlice slice = GatherNetSlice(&other, other_net);

        NetMergePlan plan = PlanNetMerge(&master, slice, net_id);
        REQUIRE(plan.sessions.size() == 1);
        CHECK(plan.sessions[0].file_open);
        ApplyNetMerge(&master, slice, plan);
        std::vector<NetInstance> sessions = master.GetNetInstancesForNet(net_id);
        REQUIRE(sessions.size() == 1);
        CHECK(sessions[0].status == NetInstanceStatus::kClosed);
        CHECK_EQ(sessions[0].closed_at, std::int64_t{1004});  // Its last check-in.
        std::vector<CheckIn> check_ins = master.GetCheckInsForNetInstance(sessions[0].id);
        REQUIRE(check_ins.size() == 2);
        CHECK_EQ(check_ins[1].sequence_number, 2);  // Renumbered.
    }

}  // namespace ql
