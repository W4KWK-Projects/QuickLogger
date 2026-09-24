// The SQLite layer: schema and upgrades, stations, nets, sessions,
// check-ins, deletes and cleanup, import bookkeeping, FCC and ZIP tables,
// SSH users.

#include <sqlite3.h>

#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static std::int64_t CountRows(const std::string& db_path, const std::string& sql)
    {
        sqlite3* db = nullptr;
        sqlite3_open(db_path.c_str(), &db);
        sqlite3_stmt* statement = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr);
        std::int64_t count = -1;
        if (sqlite3_step(statement) == SQLITE_ROW)
        {
            count = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
        sqlite3_close(db);
        return count;
    }

    static void RunSql(const std::string& db_path, const std::string& sql)
    {
        sqlite3* db = nullptr;
        sqlite3_open(db_path.c_str(), &db);
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    // ---- Schema ----------------------------------------------------------------

    QL_TEST(NewDatabaseHasEveryTableAndIsAtTheCurrentVersion)
    {
        TempDir dir;
        {
            Database db(dir.File("q.db"));
        }
        for (const char* table :
             {"stations", "nets", "net_instances", "check_ins", "net_saved_stations", "import_runs",
              "uls_stations", "zip_centroids", "zip_counties", "zip_place_counties", "users"})
        {
            CHECK_EQ(CountRows(dir.File("q.db"),
                               std::string("SELECT COUNT(*) FROM sqlite_schema WHERE name='") +
                                   table + "'"),
                     std::int64_t{1});
        }
        CHECK_EQ(CountRows(dir.File("q.db"), "PRAGMA user_version"), std::int64_t{2});
        CHECK_EQ(CountRows(dir.File("q.db"),
                           "SELECT COUNT(*) FROM pragma_table_info('import_runs') WHERE name IN "
                           "('phase','percent','heartbeat_at','requested_at')"),
                 std::int64_t{4});
    }

    QL_TEST(ReopeningADatabaseKeepsItsData)
    {
        TempDir dir;
        {
            Database db(dir.File("q.db"));
            AddTestNet(&db, "Keep Me");
        }
        Database reopened(dir.File("q.db"));
        std::vector<Net> nets = reopened.GetAllNets();
        REQUIRE(nets.size() == 1);
        CHECK_EQ(nets[0].name, std::string("Keep Me"));
    }

    QL_TEST(OldDatabaseIsUpgradedOnOpen)
    {
        TempDir dir;
        std::string path = dir.File("old.db");
        // The shape of a database from before several later additions, with a
        // leftover ULS row still in `stations` and a run-together ZIP+4.
        RunSql(path, R"sql(
            CREATE TABLE stations (callsign TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '',
                member_id TEXT NOT NULL DEFAULT '', city TEXT NOT NULL DEFAULT '',
                county TEXT NOT NULL DEFAULT '', state TEXT NOT NULL DEFAULT '',
                zip TEXT NOT NULL DEFAULT '', grid_square TEXT NOT NULL DEFAULT '',
                license_class TEXT NOT NULL DEFAULT '', email TEXT NOT NULL DEFAULT '',
                data_source INTEGER NOT NULL DEFAULT 0, last_updated INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE nets (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL,
                mode TEXT NOT NULL DEFAULT '', default_frequency TEXT NOT NULL DEFAULT '',
                default_location TEXT NOT NULL DEFAULT '', default_grid_square TEXT NOT NULL DEFAULT '',
                recurrence_description TEXT NOT NULL DEFAULT '', notes TEXT NOT NULL DEFAULT '');
            CREATE TABLE net_instances (id INTEGER PRIMARY KEY AUTOINCREMENT,
                net_id INTEGER NOT NULL REFERENCES nets(id), instance_date TEXT NOT NULL,
                net_control_callsign TEXT NOT NULL DEFAULT '',
                alternate_net_control_callsign TEXT NOT NULL DEFAULT '',
                logger_callsign TEXT NOT NULL DEFAULT '', created_by TEXT NOT NULL DEFAULT '',
                frequency TEXT NOT NULL DEFAULT '', location TEXT NOT NULL DEFAULT '',
                status INTEGER NOT NULL DEFAULT 0, closed_at INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE uls_stations (callsign TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '',
                street_address TEXT NOT NULL DEFAULT '', city TEXT NOT NULL DEFAULT '',
                state TEXT NOT NULL DEFAULT '', zip TEXT NOT NULL DEFAULT '',
                license_class TEXT NOT NULL DEFAULT '', last_updated INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE import_runs (source TEXT PRIMARY KEY, status TEXT NOT NULL DEFAULT 'never_run',
                started_at INTEGER NOT NULL DEFAULT 0, completed_at INTEGER NOT NULL DEFAULT 0,
                records_imported INTEGER NOT NULL DEFAULT 0, last_error TEXT NOT NULL DEFAULT '');
            INSERT INTO nets (name) VALUES ('Old Net');
            INSERT INTO net_instances (net_id, instance_date) VALUES (1, '2025-01-01');
            INSERT INTO stations (callsign, name, data_source) VALUES ('OLDULS', 'From ULS', 2);
            INSERT INTO stations (callsign, name) VALUES ('KEEPME', 'Manual');
            INSERT INTO uls_stations (callsign, zip) VALUES ('K1CSA', '307524915');
        )sql");

        Database db(path);
        CHECK_EQ(CountRows(path, "PRAGMA user_version"), std::int64_t{2});
        std::vector<Net> nets = db.GetAllNets();
        REQUIRE(nets.size() == 1);
        CHECK_EQ(nets[0].created_at, std::int64_t{0});  // Unknown, not guessed.
        CHECK_EQ(nets[0].imported_at, std::int64_t{0});
        // New columns exist, with their defaults.
        std::vector<NetInstance> instances = db.GetNetInstancesForNet(1);
        REQUIRE(instances.size() == 1);
        CHECK_EQ(instances[0].started_at, std::int64_t{0});
        CHECK_EQ(instances[0].operator_role, kRoleNetControl);
        CHECK(db.FindStationByCallsign("KEEPME").has_value());
        // The old ULS row moved to its own table; the ZIP+4 was trimmed.
        CHECK(!db.FindStationByCallsign("OLDULS").has_value());
        CHECK(db.FindUlsStationByCallsign("OLDULS").has_value());
        std::optional<Station> k1csa = db.FindUlsStationByCallsign("K1CSA");
        REQUIRE(k1csa.has_value());
        CHECK_EQ(k1csa->zip, std::string("30752"));
    }

    // ---- Stations ----------------------------------------------------------------

    QL_TEST(CallsignsAreStoredUppercase)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        db.RecordManualCheckInStation(MakeStation("w4kwk", "Wes"), 1);
        std::optional<Station> station = db.FindStationByCallsign("W4KWK");
        REQUIRE(station.has_value());
        CHECK_EQ(station->callsign, std::string("W4KWK"));
        CHECK(db.FindStationByCallsign("w4KwK").has_value());
    }

    QL_TEST(LoggingAStationNeverBlanksKnownDetails)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        Station first = MakeStation("N4ABC", "Ann Able", "37415", "Chattanooga");
        first.member_id = "123";
        db.RecordManualCheckInStation(first, 1);
        // Logged again later with only a new remark-free, name-free entry.
        Station second = MakeStation("N4ABC");
        second.county = "Hamilton";
        db.RecordManualCheckInStation(second, 2);
        std::optional<Station> stored = db.FindStationByCallsign("N4ABC");
        REQUIRE(stored.has_value());
        CHECK_EQ(stored->name, std::string("Ann Able"));
        CHECK_EQ(stored->member_id, std::string("123"));
        CHECK_EQ(stored->county, std::string("Hamilton"));
    }

    QL_TEST(EditingAStationCanClearAField)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        Station station = MakeStation("N4ABC", "Ann Able", "37415");
        station.member_id = "123";
        db.RecordManualCheckInStation(station, 1);
        station.member_id = "";
        db.UpdateStationFields(station, 2);
        CHECK_EQ(db.FindStationByCallsign("N4ABC")->member_id, std::string(""));
    }

    QL_TEST(CallsignSearchMatchesAnySubstringIgnoringCase)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        db.RecordManualCheckInStation(MakeStation("W4KWK"), 1);
        db.RecordManualCheckInStation(MakeStation("AA4FA"), 1);
        CHECK_EQ(db.SearchStationsByCallsignSubstring("kwk").size(), std::size_t{1});
        CHECK_EQ(db.SearchStationsByCallsignSubstring("4").size(), std::size_t{2});
        CHECK_EQ(db.SearchStationsByCallsignSubstring("wk").size(), std::size_t{1});
        CHECK(db.SearchStationsByCallsignSubstring("zzz").empty());
        // LIKE wildcards typed by the operator are matched literally enough
        // not to return everything.
        CHECK(db.SearchStationsByCallsignSubstring("%").size() <= std::size_t{2});
    }

    QL_TEST(ThisNetSearchCoversCheckInsAndSavedStationsOnly)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net_a = AddTestNet(&db, "A");
        std::int64_t net_b = AddTestNet(&db, "B");
        std::int64_t session = AddTestInstance(&db, net_a, "2026-01-01", 1, "W4KWK");
        AddTestCheckIn(&db, session, "K4AAA", 1);
        db.SaveNetStation(net_a, MakeStation("K4BBB"), "", 1);
        db.SaveNetStation(net_b, MakeStation("K4CCC"), "", 1);

        std::vector<Station> matches = db.SearchNetStationsByCallsignSubstring(net_a, "K4");
        REQUIRE(matches.size() == 2);
        CHECK_EQ(matches[0].callsign, std::string("K4AAA"));
        CHECK_EQ(matches[1].callsign, std::string("K4BBB"));
    }

    QL_TEST(SavedStationRemarksAreKeptPerNet)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net_a = AddTestNet(&db, "A");
        std::int64_t net_b = AddTestNet(&db, "B");
        db.SaveNetStation(net_a, MakeStation("K4AAA"), "mobile", 1);
        db.SaveNetStation(net_b, MakeStation("K4AAA"), "base", 1);
        CHECK_EQ(db.GetSavedNetStationRemarks(net_a, "k4aaa"), std::string("mobile"));
        CHECK_EQ(db.GetSavedNetStationRemarks(net_b, "K4AAA"), std::string("base"));
        CHECK_EQ(db.GetSavedNetStationRemarks(net_a, "NOBODY"), std::string(""));
        db.UpdateSavedNetStation(net_a, MakeStation("K4AAA"), "", 2);
        CHECK_EQ(db.GetSavedNetStationRemarks(net_a, "K4AAA"), std::string(""));
    }

    // ---- Nets, sessions and check-ins --------------------------------------------

    QL_TEST(SessionsListNewestFirstIncludingSameDay)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "A");
        AddTestInstance(&db, net, "2026-01-01", 100, "W4KWK");
        std::int64_t morning = AddTestInstance(&db, net, "2026-01-02", 200, "W4KWK");
        std::int64_t evening = AddTestInstance(&db, net, "2026-01-02", 300, "W4KWK");
        std::vector<NetInstance> sessions = db.GetNetInstancesForNet(net);
        REQUIRE(sessions.size() == 3);
        CHECK_EQ(sessions[0].id, evening);
        CHECK_EQ(sessions[1].id, morning);
        CHECK_EQ(sessions[2].instance_date, std::string("2026-01-01"));
    }

    QL_TEST(NetsKeepTheirCreationAndImportTimes)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        Net net;
        net.name = "skywarn";
        net.created_at = 111;
        net.imported_at = 222;
        std::int64_t first = db.CreateNet(net);
        net.name = "Skywarn";
        net.created_at = 333;
        net.imported_at = 0;
        std::int64_t second = db.CreateNet(net);
        CHECK_EQ(db.GetNetById(first)->created_at, std::int64_t{111});
        CHECK_EQ(db.GetNetById(first)->imported_at, std::int64_t{222});
        // Same name in any case: listed together, oldest first.
        std::vector<Net> nets = db.GetAllNets();
        REQUIRE(nets.size() == 2);
        CHECK_EQ(nets[0].id, first);
        CHECK_EQ(nets[1].id, second);
    }

    QL_TEST(NetsWithOpenSessionsAreFound)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t open_net = AddTestNet(&db, "Open");
        std::int64_t closed_net = AddTestNet(&db, "Closed");
        AddTestInstance(&db, open_net, "2026-01-01", 1, "W4KWK");
        AddTestInstance(&db, open_net, "2026-01-02", 2, "W4KWK");
        db.CloseNetInstance(AddTestInstance(&db, closed_net, "2026-01-01", 1, "W4KWK"), 5);
        CHECK(db.GetNetIdsWithOpenInstances() == std::vector<std::int64_t>({open_net}));
    }

    QL_TEST(ClosingASessionRecordsWhen)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "A");
        std::int64_t session = AddTestInstance(&db, net, "2026-01-01", 100, "W4KWK");
        CHECK(db.GetNetInstanceById(session)->status == NetInstanceStatus::kOpen);
        db.CloseNetInstance(session, 555);
        std::optional<NetInstance> closed = db.GetNetInstanceById(session);
        CHECK(closed->status == NetInstanceStatus::kClosed);
        CHECK_EQ(closed->closed_at, std::int64_t{555});
    }

    QL_TEST(OnlyOneCheckInHoldsARole)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "A");
        std::int64_t session = AddTestInstance(&db, net, "2026-01-01", 100, "W4KWK");
        std::int64_t first = AddTestCheckIn(&db, session, "K4AAA", 1, kRoleLogger);
        std::int64_t second = AddTestCheckIn(&db, session, "K4BBB", 2, kRoleLogger);
        db.ClearCheckInRoleForInstance(session, kRoleLogger, second);
        std::vector<CheckIn> check_ins = db.GetCheckInsForNetInstance(session);
        for (const CheckIn& check_in : check_ins)
        {
            if (check_in.id == first)
            {
                CHECK_EQ(check_in.designated_role, kRoleNone);
            }
            if (check_in.id == second)
            {
                CHECK_EQ(check_in.designated_role, kRoleLogger);
            }
        }
        db.SetNetInstanceRoleCallsign(session, kRoleLogger, "k4bbb");
        CHECK_EQ(db.GetNetInstanceById(session)->logger_callsign, std::string("K4BBB"));
    }

    QL_TEST(CheckInsComeBackInLoggingOrder)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "A");
        std::int64_t session = AddTestInstance(&db, net, "2026-01-01", 100, "W4KWK");
        AddTestCheckIn(&db, session, "K4CCC", 1);
        AddTestCheckIn(&db, session, "K4AAA", 2);
        AddTestCheckIn(&db, session, "K4BBB", 3);
        std::vector<CheckIn> check_ins = db.GetCheckInsForNetInstance(session);
        REQUIRE(check_ins.size() == 3);
        CHECK_EQ(check_ins[0].callsign, std::string("K4CCC"));
        CHECK_EQ(check_ins[2].sequence_number, 3);
    }

    // ---- Deletes and station cleanup ---------------------------------------------

    QL_TEST(RemovingTheLastUseOfAStationDeletesItsDetails)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net_a = AddTestNet(&db, "A");
        std::int64_t net_b = AddTestNet(&db, "B");
        db.SaveNetStation(net_a, MakeStation("TYPO123", "Oops"), "", 1);
        db.SaveNetStation(net_a, MakeStation("K4TWO"), "", 1);
        db.SaveNetStation(net_b, MakeStation("K4TWO"), "", 1);

        CHECK(!db.IsStationUsedOutsideNet("TYPO123", net_a));
        CHECK(db.IsStationUsedOutsideNet("K4TWO", net_a));

        db.RemoveSavedNetStation(net_a, "typo123");
        CHECK(!db.FindStationByCallsign("TYPO123").has_value());

        db.RemoveSavedNetStation(net_a, "K4TWO");
        CHECK(db.FindStationByCallsign("K4TWO").has_value());  // Still saved to B.
    }

    QL_TEST(StationsWithCheckInsSurviveRemoval)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "A");
        std::int64_t session = AddTestInstance(&db, net, "2026-01-01", 100, "W4KWK");
        AddTestCheckIn(&db, session, "K4AAA", 1);
        db.SaveNetStation(net, MakeStation("K4AAA", "Al"), "", 1);
        CHECK(db.IsStationUsedOutsideNet("K4AAA", net));
        db.RemoveSavedNetStation(net, "K4AAA");
        CHECK(db.FindStationByCallsign("K4AAA").has_value());
    }

    QL_TEST(DeletingTheLastCheckInCleansUpItsStation)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "A");
        std::int64_t session = AddTestInstance(&db, net, "2026-01-01", 100, "W4KWK");
        std::int64_t only = AddTestCheckIn(&db, session, "K4ONCE", 1);
        db.DeleteCheckIn(only);
        CHECK(!db.FindStationByCallsign("K4ONCE").has_value());
    }

    QL_TEST(DeletingASessionRemovesItsCheckInsOnly)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "A");
        std::int64_t keep = AddTestInstance(&db, net, "2026-01-01", 100, "W4KWK");
        std::int64_t drop = AddTestInstance(&db, net, "2026-01-02", 200, "W4KWK");
        AddTestCheckIn(&db, keep, "K4KEEP", 1);
        AddTestCheckIn(&db, drop, "K4DROP", 1);
        AddTestCheckIn(&db, drop, "K4KEEP", 2);
        db.DeleteNetInstance(drop);
        CHECK(!db.GetNetInstanceById(drop).has_value());
        CHECK_EQ(db.GetCheckInsForNetInstance(keep).size(), std::size_t{1});
        CHECK(db.FindStationByCallsign("K4KEEP").has_value());
        CHECK(!db.FindStationByCallsign("K4DROP").has_value());
    }

    QL_TEST(DeletingANetRemovesEverythingOfItsOwn)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t doomed = AddTestNet(&db, "Doomed");
        std::int64_t other = AddTestNet(&db, "Other");
        std::int64_t session = AddTestInstance(&db, doomed, "2026-01-01", 100, "W4KWK");
        AddTestCheckIn(&db, session, "K4ONLY", 1);
        AddTestCheckIn(&db, session, "K4BOTH", 2);
        db.SaveNetStation(doomed, MakeStation("K4SAVED"), "", 1);
        db.SaveNetStation(other, MakeStation("K4BOTH"), "", 1);

        db.DeleteNetCompletely(doomed);
        CHECK(!db.GetNetById(doomed).has_value());
        CHECK(db.GetNetInstancesForNet(doomed).empty());
        CHECK(db.GetSavedStationsForNet(doomed).empty());
        CHECK(!db.FindStationByCallsign("K4ONLY").has_value());
        CHECK(!db.FindStationByCallsign("K4SAVED").has_value());
        CHECK(db.FindStationByCallsign("K4BOTH").has_value());
        CHECK_EQ(db.GetAllNets().size(), std::size_t{1});
    }

    // ---- Import bookkeeping --------------------------------------------------------

    QL_TEST(OnlyOneProcessCanClaimTheRefreshJob)
    {
        TempDir dir;
        Database first(dir.File("q.db"));
        Database second(dir.File("q.db"));
        CHECK(first.TryClaimImportRun("job", 1000, 120));
        CHECK(!second.TryClaimImportRun("job", 1001, 120));
        // Still being worked on: a heartbeat keeps the claim alive.
        first.UpdateImportProgress("job", "Working", 50, 10, 1100);
        CHECK(!second.TryClaimImportRun("job", 1200, 120));
        // The worker went silent for longer than the stale limit.
        CHECK(second.TryClaimImportRun("job", 1300, 120));
    }

    QL_TEST(ProgressAndRequestsAreRecorded)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        db.TryClaimImportRun("job", 1000, 120);
        db.UpdateImportProgress("job", "Downloading", 42, 7, 1010);
        std::optional<ImportRunStatus> status = db.GetImportRunStatus("job");
        REQUIRE(status.has_value());
        CHECK_EQ(status->status, std::string("running"));
        CHECK_EQ(status->phase, std::string("Downloading"));
        CHECK_EQ(status->percent, 42);
        CHECK_EQ(status->heartbeat_at, std::int64_t{1010});
        db.RequestImportRun("job", 2000);
        db.UpsertImportRunStatus(*status);  // Doesn't clobber the request.
        CHECK_EQ(db.GetImportRunStatus("job")->requested_at, std::int64_t{2000});
        CHECK(!db.GetImportRunStatus("never").has_value());
    }

    // ---- FCC and ZIP data ----------------------------------------------------------

    QL_TEST(UlsUpsertUpdatesOnlyChangedRows)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::vector<Station> stations = {MakeStation("K1AAA", "One", "37415"),
                                         MakeStation("K1BBB", "Two", "30752")};
        db.BulkUpsertUlsStations(stations, 0, stations.size(), 100);
        stations[1].name = "Two Renamed";
        db.BulkUpsertUlsStations(stations, 0, stations.size(), 200);
        CHECK_EQ(db.FindUlsStationByCallsign("K1AAA")->last_updated, std::int64_t{100});
        CHECK_EQ(db.FindUlsStationByCallsign("K1BBB")->last_updated, std::int64_t{200});
        CHECK_EQ(db.FindUlsStationByCallsign("K1BBB")->name, std::string("Two Renamed"));
        // A range past the end is clamped, not an error.
        db.BulkUpsertUlsStations(stations, 1, 99, 300);
        CHECK(db.HasAnyUlsStations());
    }

    QL_TEST(UlsSearchIsLimitedToTheGivenZipPrefixes)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::vector<Station> stations = {
            MakeStation("K1AAA", "", "37415"), MakeStation("K1BBB", "", "37499"),
            MakeStation("K1CCC", "", "37500"), MakeStation("K1DDD", "", "37900"),
            MakeStation("K1EEE", "", "37999"), MakeStation("W9ZZZ", "", "37415")};
        db.BulkUpsertUlsStations(stations, 0, stations.size(), 1);
        std::vector<Station> found = db.SearchUlsStationsByCallsignAndZip3Prefixes("k1", {"374"});
        REQUIRE(found.size() == 2);
        CHECK_EQ(found[0].callsign, std::string("K1AAA"));
        CHECK_EQ(found[1].callsign, std::string("K1BBB"));
        // A prefix ending in 9 (whose "next" prefix isn't a digit).
        CHECK_EQ(db.SearchUlsStationsByCallsignAndZip3Prefixes("K1", {"379"}).size(),
                 std::size_t{2});
        CHECK(db.SearchUlsStationsByCallsignAndZip3Prefixes("K1", {}).empty());
    }

    QL_TEST(ZipCountyDataIsReplacedWholesale)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        db.ReplaceZipCountyData({{"37415", "Hamilton"}, {"30752", "Dade"}},
                                {{"02467", "NEWTON", "Middlesex"}});
        CHECK_EQ(db.GetAllZipCounties().size(), std::size_t{2});
        db.ReplaceZipCountyData({{"37415", "Hamilton"}}, {});
        CHECK_EQ(db.GetAllZipCounties().size(), std::size_t{1});
        CHECK(db.GetAllZipPlaceCounties().empty());
    }

    // ---- SSH users -----------------------------------------------------------------

    QL_TEST(UsersCanBeAddedUpdatedAndRemoved)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        User user;
        user.username = "wes";
        user.public_key = "ssh-ed25519 AAAA one";
        db.CreateUser(user);
        user.public_key = "ssh-ed25519 BBBB two";
        db.CreateUser(user);  // Same name: replaces the key.
        CHECK_EQ(db.ListUsers().size(), std::size_t{1});
        CHECK_EQ(db.GetUserByUsername("wes")->public_key, std::string("ssh-ed25519 BBBB two"));
        db.UpdateUserLastLogin("wes", 99);
        CHECK_EQ(db.GetUserByUsername("wes")->last_login_at, std::int64_t{99});
        db.DeleteUser("wes");
        CHECK(!db.GetUserByUsername("wes").has_value());
    }

    QL_TEST(UsersAreListedAlphabeticallyRegardlessOfCase)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        for (const char* name : {"bob", "W4KWK", "alice"})
        {
            User user;
            user.username = name;
            user.public_key = "k";
            db.CreateUser(user);
        }
        std::vector<User> users = db.ListUsers();
        REQUIRE(users.size() == 3);
        CHECK_EQ(users[0].username, std::string("alice"));
        CHECK_EQ(users[1].username, std::string("bob"));
        CHECK_EQ(users[2].username, std::string("W4KWK"));
    }

}  // namespace ql
