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
        net.recurrence_description = "Tuesdays 8pm";
        net.created_at = 1700000000;
        std::int64_t net_id = db->CreateNet(net);

        Station saved = MakeStation("K4SAV", "Sam Saved", "37415", "Chattanooga");
        saved.member_id = "SP-12";
        db->SaveNetStation(net_id, saved, "mobile", 1);

        std::int64_t first = AddTestInstance(db, net_id, "2026-01-06", 1000, "W4KWK");
        std::int64_t second = AddTestInstance(db, net_id, "2026-01-13", 2000, "W4KWK");
        db->CloseNetInstance(first, 1500);
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
        AddTestCheckIn(&dest, AddTestInstance(&dest, existing, "2025-12-01", 1, "N0ONE"), "N0ONE",
                       1);
        std::int64_t imported = ApplyNetSlice(&dest, *slice, 1800000000);
        CHECK(imported != existing);

        std::optional<Net> net = dest.GetNetById(imported);
        REQUIRE(net.has_value());
        CHECK_EQ(net->name, std::string("Skywarn"));
        CHECK_EQ(net->default_frequency, std::string("146.940"));
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

}  // namespace ql
