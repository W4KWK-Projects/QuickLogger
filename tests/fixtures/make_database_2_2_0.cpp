// Makes tests/fixtures/quicklogger-2.2.0.sql, the database a QuickLogger
// 2.2.0 user brings to 2.3 (see test_upgrade_from_2_2.cpp). Not built with
// the tests: it compiles only against 2.2.0's own code, which writes the
// database exactly as 2.2.0 does. To make it again:
//
//   git worktree add --detach /tmp/ql-2.2.0 v2.2.0
//   cp tests/fixtures/make_database_2_2_0.cpp /tmp/ql-2.2.0/tests/
//   (add tests/make_database_2_2_0.cpp to quicklogger_tests there)
//   cmake -S /tmp/ql-2.2.0 -B /tmp/ql-2.2.0/build && cmake --build /tmp/ql-2.2.0/build
//   QL_FIXTURE_DB=/tmp/q.db /tmp/ql-2.2.0/build/quicklogger_tests MakeDatabase
//   (sqlite3 /tmp/q.db .dump | python3 tests/fixtures/portable_dump.py;
//    echo "PRAGMA user_version = 18;") > tests/fixtures/quicklogger-2.2.0.sql
//
// (.dump leaves out user_version, the schema version 2.2.0 wrote;
// portable_dump.py makes it readable by SQLite before 3.50.)
//
// Write only what 2.2.0's screens could have: its Database functions don't
// check everything its forms do (a net's mode, say, is one of NetModes()).
//
// Every value is made up. Never use a real database: the repository is
// public, and a real one holds other operators' names and addresses.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/uls_import.hpp"
#include "test_framework.hpp"

namespace ql
{

    static Station FixtureStation(const std::string& callsign, const std::string& name, const std::string& city,
                                  const std::string& state, const std::string& zip)
    {
        Station station;
        station.callsign = callsign;
        station.name = name;
        station.city = city;
        station.state = state;
        station.zip = zip;
        return station;
    }

    static std::int64_t AddSession(Database* db, std::int64_t net_id, const std::string& date, std::int64_t started_at,
                                   const std::string& operator_callsign, int operator_role)
    {
        NetInstance instance;
        instance.net_id = net_id;
        instance.instance_date = date;
        instance.started_at = started_at;
        instance.created_by = operator_callsign;
        instance.operator_role = operator_role;
        if (operator_role == kRoleNetControl)
        {
            instance.net_control_callsign = operator_callsign;
        }
        else if (operator_role == kRoleAlternateNetControl)
        {
            instance.alternate_net_control_callsign = operator_callsign;
        }
        else
        {
            instance.logger_callsign = operator_callsign;
        }
        return db->CreateNetInstance(instance);
    }

    static void AddFixtureCheckIn(Database* db, std::int64_t instance_id, const Station& station, int sequence,
                                  std::int64_t at, const std::string& signal_report, const std::string& remarks,
                                  const std::string& comment, int designated_role)
    {
        db->RecordManualCheckInStation(station, at);
        CheckIn check_in;
        check_in.net_instance_id = instance_id;
        check_in.callsign = station.callsign;
        check_in.sequence_number = sequence;
        check_in.signal_report = signal_report;
        check_in.remarks = remarks;
        check_in.comment = comment;
        check_in.checked_in_at = at;
        check_in.designated_role = designated_role;
        db->AddCheckIn(check_in);
    }

    QL_TEST(MakeDatabaseFixture)
    {
        const char* path = std::getenv("QL_FIXTURE_DB");
        if (path == nullptr)
        {
            return;
        }
        Database db(path);
        const std::int64_t t = 1790000000;  // 2026-09-21

        Station operator_station = FixtureStation("KX0TST", "Tester, Pat Q", "Chattanooga", "TN", "37415");
        operator_station.county = "Hamilton";
        operator_station.grid_square = "EM75";
        operator_station.license_class = "E";
        operator_station.email = "pat@example.org";
        operator_station.street_address = "1 Main St";
        Station mobile = FixtureStation("KX0ZZA", "O'Brien, Sam \"Skip\"", "Ringgold", "GA", "30736");
        mobile.county = "Catoosa";
        Station accented = FixtureStation("KD4ZZB", "Peña, José", "Cleveland", "TN", "37311");
        Station portable = FixtureStation("NX4ZZC/P", "Lee, Chris", "", "", "");
        Station canadian = FixtureStation("VE3ZZD", "Tremblay, Élise", "Montréal", "QC", "");
        Station bare = FixtureStation("AB4ZZE", "", "", "", "");

        // A recurring net with every field set, and one with only a name.
        Net skywarn;
        skywarn.name = "Test County Skywarn";
        skywarn.mode = "FM";
        skywarn.default_frequency = "145.390";
        skywarn.repeater_offset = "-0.6";
        skywarn.pl_tone = "107.2";
        skywarn.default_location = "37415";
        skywarn.default_grid_square = "EM75";
        skywarn.recurrence_description = "Tuesdays at 8pm ET";
        skywarn.comments = "Weather spotters.\nSecond line, with \"quotes\".";
        skywarn.created_at = t - 86400 * 30;
        skywarn.partial_match_canada = false;
        std::int64_t skywarn_id = db.CreateNet(skywarn);

        Net bare_net;
        bare_net.name = "Bare Net";
        bare_net.created_at = t - 86400 * 20;
        std::int64_t bare_net_id = db.CreateNet(bare_net);

        // Imported from someone else, Canadian partial matching, other modes.
        Net hf;
        hf.name = "Cross-Border HF Net";
        hf.mode = "SSB";
        hf.default_frequency = "7.255";
        hf.recurrence_description = "Sundays 1400Z";
        hf.created_at = t - 86400 * 10;
        hf.imported_at = t - 86400 * 9;
        hf.partial_match_canada = true;
        std::int64_t hf_id = db.CreateNet(hf);

        Net digital;
        digital.name = "Fusion Net";
        digital.mode = "Fusion";
        digital.default_frequency = "442.100";
        digital.repeater_offset = "+5";
        digital.default_location = "30736";
        digital.created_at = t - 86400 * 5;
        std::int64_t digital_id = db.CreateNet(digital);

        // Saved stations, one never checked in, with default remarks.
        db.SaveNetStation(skywarn_id, mobile, "Mobile spotter", t - 86400 * 29);
        db.SetNetMemberId(skywarn_id, "KX0ZZA", "SW-2002");  // The NWS spotter ID.
        db.SaveNetStation(skywarn_id, bare, "", t - 86400 * 29);
        db.SaveNetStation(hf_id, canadian, "Relays from VE3", t - 86400 * 9);
        db.SetNetMemberId(hf_id, "VE3ZZD", "VE-77");
        db.SaveNetStation(digital_id, mobile, "", t - 86400 * 4);
        db.SetNetMemberId(digital_id, "KX0ZZA", "CLUB-7");  // A different ID on the other net.
        // A club ID, saved on one net only.
        db.SaveNetStation(bare_net_id, operator_station, "", t - 86400 * 19);
        db.SetNetMemberId(bare_net_id, "KX0TST", "10001");

        // Two closed sessions and one still open.
        std::int64_t first = AddSession(&db, skywarn_id, "2026-09-15", t - 86400 * 6, "KX0TST", kRoleNetControl);
        AddFixtureCheckIn(&db, first, operator_station, 1, t - 86400 * 6, "", "", "", kRoleNone);
        AddFixtureCheckIn(&db, first, mobile, 2, t - 86400 * 6 + 60, "59", "Spotter 12", "Heavy rain", kRoleNone);
        AddFixtureCheckIn(&db, first, accented, 3, t - 86400 * 6 + 120, "57", "", "", kRoleAlternateNetControl);
        AddFixtureCheckIn(&db, first, portable, 4, t - 86400 * 6 + 180, "", "Portable", "", kRoleLogger);
        db.SetNetInstanceNotes(first, "Severe watch until 10pm.\nNo damage reports.");
        db.CloseNetInstance(first, t - 86400 * 6 + 2700);

        std::int64_t second = AddSession(&db, skywarn_id, "2026-09-22", t + 86400, "KX0ZZA", kRoleLogger);
        db.SetNetInstanceRoleCallsign(second, kRoleNetControl, "KX0TST");
        AddFixtureCheckIn(&db, second, mobile, 1, t + 86400, "", "", "", kRoleNone);
        AddFixtureCheckIn(&db, second, operator_station, 2, t + 86400 + 30, "", "", "", kRoleNone);
        AddFixtureCheckIn(&db, second, bare, 3, t + 86400 + 90, "55", "First time", "", kRoleNone);
        db.CloseNetInstance(second, t + 86400 + 1800);

        std::int64_t open = AddSession(&db, hf_id, "2026-09-27", t + 86400 * 6, "KX0TST", kRoleAlternateNetControl);
        AddFixtureCheckIn(&db, open, operator_station, 1, t + 86400 * 6, "", "", "", kRoleNone);
        AddFixtureCheckIn(&db, open, canadian, 2, t + 86400 * 6 + 45, "5x9", "", "Montréal relay", kRoleNone);

        std::int64_t fusion = AddSession(&db, digital_id, "2026-09-26", t + 86400 * 5, "KX0TST", kRoleNetControl);
        AddFixtureCheckIn(&db, fusion, operator_station, 1, t + 86400 * 5, "", "", "", kRoleNone);
        db.CloseNetInstance(fusion, t + 86400 * 5 + 600);

        // A GMRS net: a family sharing one call sign, told apart by name,
        // saved and checked in; one session pushed upstream.
        Net gmrs_net;
        gmrs_net.name = "Family GMRS Net";
        gmrs_net.service = NetService::kGmrs;
        gmrs_net.mode = "FM";
        gmrs_net.default_frequency = "462.5625";
        gmrs_net.default_location = "37415";
        gmrs_net.created_at = t - 86400 * 3;
        std::int64_t gmrs_id = db.CreateNet(gmrs_net);
        Station family = FixtureStation("WZZZ123", "Family, Fixture", "Chattanooga", "TN", "37415");
        db.SaveNetStation(gmrs_id, family, "Mom", t - 86400 * 3, "Pat");
        db.SaveNetStation(gmrs_id, family, "Kid", t - 86400 * 3, "Alex");
        std::int64_t family_session = AddSession(&db, gmrs_id, "2026-09-24", t + 86400 * 3, "KX0TST", kRoleNetControl);
        db.RecordManualCheckInStation(family, t + 86400 * 3);
        for (const char* person : {"Pat", "Alex"})
        {
            CheckIn check_in;
            check_in.net_instance_id = family_session;
            check_in.callsign = family.callsign;
            check_in.name = person;
            check_in.sequence_number = person[0] == 'P' ? 1 : 2;
            check_in.checked_in_at = t + 86400 * 3 + 30;
            check_in.designated_role = kRoleNone;
            db.AddCheckIn(check_in);
        }
        db.CloseNetInstance(family_session, t + 86400 * 3 + 900);
        db.SetNetInstancePushedAt(family_session, t + 86400 * 3 + 1200);

        // An ad hoc net with a closed session.
        Net ad_hoc;
        ad_hoc.name = "Tailgate Test";
        ad_hoc.is_ad_hoc = true;
        ad_hoc.mode = "FM";
        ad_hoc.default_frequency = "146.520";
        ad_hoc.created_at = t + 86400 * 2;
        std::int64_t ad_hoc_id = db.CreateNet(ad_hoc);
        std::int64_t tailgate = AddSession(&db, ad_hoc_id, "2026-09-23", t + 86400 * 2, "KX0TST", kRoleNetControl);
        AddFixtureCheckIn(&db, tailgate, operator_station, 1, t + 86400 * 2, "", "", "", kRoleNone);
        AddFixtureCheckIn(&db, tailgate, accented, 2, t + 86400 * 2 + 20, "", "Simplex", "", kRoleNone);
        db.CloseNetInstance(tailgate, t + 86400 * 2 + 900);

        // SSH users: one with two keys, one view-only, one stored in lower
        // case.
        User user;
        user.username = "KX0TST";
        user.amateur_callsign = "KX0TST";
        user.public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyOneFixtureKeyOneFixtureKeyOne0 laptop";
        user.created_at = t - 86400 * 30;
        db.CreateUser(user);
        user.public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyTwoFixtureKeyTwoFixtureKeyTwo0 phone";
        db.CreateUser(user);
        user.username = "KX0ZZA";
        user.amateur_callsign = "KX0ZZA";
        user.public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyThreeFixtureKeyThreeFixtureKe0";
        user.view_only = true;
        db.CreateUser(user);
        user.username = "kd4zzb";
        user.amateur_callsign = "KD4ZZB";
        user.public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyFourFixtureKeyFourFixtureKey0";
        user.view_only = false;
        db.CreateUser(user);
        user.username = "WQXX000";
        user.public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyFiveFixtureKeyFiveFixtureKey0 gmrs";
        user.amateur_callsign = "";
        user.gmrs_callsign = "WQXX000";
        db.CreateUser(user);
        std::vector<User> keys = db.GetUserKeys("KX0TST");
        if (keys.size() == 2)
        {
            db.UpdateUserLastLogin(keys[0].id, t + 3600);
            db.UpdateUserKeyTransfer(keys[0].id, kTransferZmodem);
            db.UpdateUserKeyTransfer(keys[1].id, kTransferSftp);
        }

        // A little of the downloaded station data, and its status rows.
        Station uls = FixtureStation("W4ZZF", "LICENSEE, FIXTURE A", "CHATTANOOGA", "TN", "37421");
        uls.license_class = "G";
        uls.street_address = "2 Test Rd";
        uls.data_source = StationDataSource::kUls;
        std::vector<Station> uls_batch = {uls};
        db.BulkUpsertUlsStations(uls_batch, 0, uls_batch.size(), t - 3600);
        Station ised = FixtureStation("VA3ZZG", "Fixture, Ised", "Ottawa", "ON", "K1A 0B1");
        ised.data_source = StationDataSource::kIsed;
        db.ReplaceIsedStations({ised}, t - 3600);
        Station gmrs_licensee = FixtureStation("WZZZ123", "FAMILY, FIXTURE", "CHATTANOOGA", "TN", "37415");
        gmrs_licensee.data_source = StationDataSource::kUls;
        std::vector<Station> gmrs_batch = {gmrs_licensee};
        db.BulkUpsertUlsStations(gmrs_batch, 0, gmrs_batch.size(), t - 3600, LicenseTable::kGmrs);
        ZipCentroid centroid;
        centroid.zip = "37415";
        centroid.lat = 35.1121;
        centroid.lon = -85.2793;
        db.BulkUpsertZipCentroids({centroid});
        ZipCounty county;
        county.zip = "37415";
        county.county = "Hamilton";
        ZipPlaceCounty place;
        place.zip = "30736";
        place.place = "RINGGOLD";
        place.county = "Catoosa";
        db.ReplaceZipCountyData({county}, {place});
        for (const char* source :
             {kUlsDataset, kIsedDataset, kZipCentroidsDataset, kZipCountyDataset, kGmrsDataset, kCaPostalDataset})
        {
            ImportRunStatus run;
            run.source = source;
            run.status = "complete";
            run.started_at = t - 3700;
            run.completed_at = t - 3600;
            run.records_imported = 1;
            db.UpsertImportRunStatus(run);
        }
        ImportRunStatus job;
        job.source = kDataRefreshJob;
        job.status = "complete";
        job.started_at = t - 3700;
        job.completed_at = t - 3600;
        job.percent = 100;
        db.UpsertImportRunStatus(job);
    }

}  // namespace ql
