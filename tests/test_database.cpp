// The SQLite layer: schema and upgrades, stations, nets, sessions,
// check-ins, deletes and cleanup, import bookkeeping, FCC and ZIP tables,
// SSH users.

#include <sqlite3.h>

#include <stdexcept>
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
        for (const char* table : {"stations", "nets", "net_instances", "check_ins", "net_saved_stations", "import_runs",
                                  "uls_stations", "zip_centroids", "zip_counties", "zip_place_counties", "users"})
        {
            CHECK_EQ(CountRows(dir.File("q.db"),
                               std::string("SELECT COUNT(*) FROM sqlite_schema WHERE name='") + table + "'"),
                     std::int64_t{1});
        }
        CHECK_EQ(CountRows(dir.File("q.db"), "PRAGMA user_version"), std::int64_t{13});
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
            INSERT INTO nets (name, mode) VALUES ('Old Net', 'fm');
            INSERT INTO nets (name, mode) VALUES ('Fusion Net', 'C4FM');
            INSERT INTO nets (name, mode) VALUES ('Mystery Net', 'Digital');
            INSERT INTO net_instances (net_id, instance_date) VALUES (1, '2025-01-01');
            INSERT INTO stations (callsign, name, data_source) VALUES ('OLDULS', 'From ULS', 2);
            INSERT INTO stations (callsign, name) VALUES ('KEEPME', 'Manual');
            INSERT INTO uls_stations (callsign, zip) VALUES ('K1CSA', '307524915');
        )sql");

        Database db(path);
        CHECK_EQ(CountRows(path, "PRAGMA user_version"), std::int64_t{13});
        std::vector<Net> nets = db.GetAllNets();
        REQUIRE(nets.size() == 3);
        // Sorted by name: Fusion Net, Mystery Net, Old Net. Known spellings
        // of a mode are converted since 1.8.0; anything else is blanked.
        CHECK_EQ(nets[0].mode, std::string("Fusion"));
        CHECK_EQ(nets[1].mode, std::string(""));
        CHECK_EQ(nets[2].mode, std::string("FM"));
        nets.erase(nets.begin(), nets.begin() + 2);
        CHECK_EQ(nets[0].created_at, std::int64_t{0});  // Unknown, not guessed.
        CHECK_EQ(nets[0].imported_at, std::int64_t{0});
        CHECK(!nets[0].is_ad_hoc);  // Nothing is guessed to be ad hoc.
        // Nets from before 1.7.0 are US nets.
        CHECK(!nets[0].partial_match_canada);
        // New columns exist, with their defaults.
        std::vector<NetInstance> instances = db.GetNetInstancesForNet(1);
        REQUIRE(instances.size() == 1);
        CHECK_EQ(instances[0].started_at, std::int64_t{0});
        CHECK_EQ(instances[0].operator_role, kRoleNetControl);
        CHECK_EQ(instances[0].notes, std::string(""));
        CHECK(db.FindStationByCallsign("KEEPME").has_value());
        // The old ULS row moved to its own table; the ZIP+4 was trimmed.
        CHECK(!db.FindStationByCallsign("OLDULS").has_value());
        CHECK(db.FindUlsStationByCallsign("OLDULS").has_value());
        std::optional<Station> k1csa = db.FindUlsStationByCallsign("K1CSA");
        REQUIRE(k1csa.has_value());
        CHECK_EQ(k1csa->zip, std::string("30752"));
    }

    QL_TEST(AnOldSeedStationsTableIsMergedAndDropped)
    {
        TempDir dir;
        std::string path = dir.File("old.db");
        {
            Database db(path);
            std::int64_t net_id = AddTestNet(&db, "Oldest Net");
            db.SaveNetStation(net_id, MakeStation("K4AAA"), "Training", 1);
            db.RecordManualCheckInStation(MakeStation("K4BBB"), 1);
        }
        // The table as the earliest builds left it: K4AAA is already a saved
        // station there, K4BBB isn't.
        RunSql(path, R"sql(
            CREATE TABLE net_seed_stations (
                net_id INTEGER NOT NULL REFERENCES nets(id),
                callsign TEXT NOT NULL REFERENCES stations(callsign),
                default_remarks TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (net_id, callsign));
            INSERT INTO net_seed_stations VALUES (1, 'K4AAA', 'Old remark');
            INSERT INTO net_seed_stations VALUES (1, 'K4BBB', 'Mobile');
            PRAGMA user_version = 12;
        )sql");

        Database db(path);
        CHECK_EQ(CountRows(path, "SELECT COUNT(*) FROM sqlite_schema WHERE name='net_seed_stations'"), std::int64_t{0});
        // Today's remarks win; the one only in the old table is kept.
        CHECK_EQ(db.GetSavedNetStationRemarks(1, "K4AAA"), std::string("Training"));
        CHECK_EQ(db.GetSavedNetStationRemarks(1, "K4BBB"), std::string("Mobile"));
        // And the net can now be deleted.
        db.DeleteNetCompletely(1);
        CHECK(!db.GetNetById(1).has_value());
    }

    QL_TEST(OldNetFrequenciesMoveToComments)
    {
        TempDir dir;
        std::string path = dir.File("q.db");
        {
            Database db(path);
            AddTestNet(&db, "Free Text");
            AddTestNet(&db, "Good");
        }
        // As a 1.4.1 database could have them.
        RunSql(path, R"sql(
            UPDATE nets SET default_frequency = '146.94 -600 PL 100' WHERE name = 'Free Text';
            UPDATE nets SET default_frequency = '145.390' WHERE name = 'Good';
            PRAGMA user_version = 4;
        )sql");

        Database db(path);
        for (const Net& net : db.GetAllNets())
        {
            if (net.name == "Free Text")
            {
                CHECK_EQ(net.default_frequency, std::string("146.94"));
                CHECK_EQ(net.comments, std::string("Frequency: 146.94 -600 PL 100"));
            }
            else
            {
                CHECK_EQ(net.default_frequency, std::string("145.390"));
                CHECK(net.comments.empty());
            }
        }
    }

    QL_TEST(OldNetLocationsBecomeZipsOrBlank)
    {
        TempDir dir;
        std::string path = dir.File("q.db");
        {
            Database db(path);
            AddTestNet(&db, "Has ZIP");
            AddTestNet(&db, "City Only");
            AddTestNet(&db, "Already ZIP");
            AddTestNet(&db, "Empty");
        }
        // As a version-1.1 database would have them.
        RunSql(path, R"sql(
            UPDATE nets SET default_location = 'Chattanooga, TN 37415-2623' WHERE name = 'Has ZIP';
            UPDATE nets SET default_location = 'Chattanooga' WHERE name = 'City Only';
            UPDATE nets SET default_location = '37402' WHERE name = 'Already ZIP';
            PRAGMA user_version = 2;
        )sql");

        Database db(path);
        std::vector<Net> nets = db.GetAllNets();
        REQUIRE(nets.size() == 4);
        for (const Net& net : nets)
        {
            if (net.name == "Has ZIP")
            {
                CHECK_EQ(net.default_location, std::string("37415"));
            }
            else if (net.name == "Already ZIP")
            {
                CHECK_EQ(net.default_location, std::string("37402"));
            }
            else
            {
                CHECK(net.default_location.empty());
            }
        }
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

    QL_TEST(NetLastStartedByIsTheNewestRecurringOne)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t skywarn = AddTestNet(&db, "Skywarn");
        std::int64_t ares = AddTestNet(&db, "ARES");
        Net ad_hoc;
        ad_hoc.name = "Tailgate";
        ad_hoc.is_ad_hoc = true;
        std::int64_t tailgate = db.CreateNet(ad_hoc);
        CHECK(!db.GetNetLastStartedBy("W4KWK").has_value());

        AddTestInstance(&db, ares, "2026-01-01", 1, "W4KWK");
        AddTestInstance(&db, skywarn, "2026-01-02", 2, "W4KWK");
        // Newer, but an ad hoc net, or someone else's session.
        AddTestInstance(&db, tailgate, "2026-01-03", 3, "W4KWK");
        AddTestInstance(&db, ares, "2026-01-04", 4, "K4AAA");
        CHECK_EQ(db.GetNetLastStartedBy("W4KWK").value_or(0), skywarn);
        CHECK_EQ(db.GetNetLastStartedBy("w4kwk").value_or(0), skywarn);
        CHECK_EQ(db.GetNetLastStartedBy("K4AAA").value_or(0), ares);
    }

    static std::vector<std::string> CallsignsWithNumbers(Database* db, std::int64_t instance_id)
    {
        std::vector<std::string> rows;
        for (const CheckIn& check_in : db->GetCheckInsForNetInstance(instance_id))
        {
            rows.push_back(std::to_string(check_in.sequence_number) + " " + check_in.callsign);
        }
        return rows;
    }

    QL_TEST(CheckInsAreRenumberedOnlyOnceTheSessionIsClosed)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "Skywarn");
        std::int64_t session = AddTestInstance(&db, net, "2026-01-01", 1, "W4KWK");
        AddTestCheckIn(&db, session, "W4KWK", 1, kRoleNone);
        std::int64_t typo = AddTestCheckIn(&db, session, "K4AAA", 2, kRoleNone);
        AddTestCheckIn(&db, session, "K4BBB", 3, kRoleNone);
        AddTestCheckIn(&db, session, "K4CCC", 4, kRoleNone);

        // Open: the others keep their numbers.
        db.DeleteCheckIn(typo);
        CHECK(CallsignsWithNumbers(&db, session) == std::vector<std::string>({"1 W4KWK", "3 K4BBB", "4 K4CCC"}));

        // Closed: numbered without gaps, in the same order.
        REQUIRE(db.CloseNetInstance(session, 5));
        CHECK(CallsignsWithNumbers(&db, session) == std::vector<std::string>({"1 W4KWK", "2 K4BBB", "3 K4CCC"}));

        // A delete from a closed session renumbers straight away.
        db.DeleteCheckIn(db.GetCheckInsForNetInstance(session)[1].id);
        CHECK(CallsignsWithNumbers(&db, session) == std::vector<std::string>({"1 W4KWK", "2 K4CCC"}));
    }

    QL_TEST(WriteTransactionsAreAllOrNothing)
    {
        TempDir dir;
        Database db(dir.File("q.db"));

        // Not committed: nothing kept.
        {
            Database::WriteTransaction transaction(&db);
            AddTestNet(&db, "Dropped");
        }
        CHECK(db.GetAllNets().empty());

        // Committed: kept.
        {
            Database::WriteTransaction transaction(&db);
            AddTestNet(&db, "Kept");
            transaction.Commit();
        }
        CHECK_EQ(db.GetAllNets().size(), std::size_t{1});

        // An inner one joins the outer, whose rollback undoes both.
        {
            Database::WriteTransaction outer(&db);
            {
                Database::WriteTransaction inner(&db);
                AddTestNet(&db, "Inner");
                inner.Commit();
            }
            AddTestNet(&db, "Outer");
        }
        CHECK_EQ(db.GetAllNets().size(), std::size_t{1});

        // An exception unwinding one rolls it back.
        try
        {
            Database::WriteTransaction transaction(&db);
            AddTestNet(&db, "Thrown");
            throw std::runtime_error("failed part-way");
        }
        catch (const std::runtime_error&)
        {
        }
        CHECK_EQ(db.GetAllNets().size(), std::size_t{1});

        // Reads grouped under a ReadTransaction see the same data.
        {
            Database::ReadTransaction reads(&db);
            CHECK_EQ(db.GetAllNets().size(), std::size_t{1});
            CHECK_EQ(db.GetAllNets()[0].name, std::string("Kept"));
        }
        AddTestNet(&db, "After");
        CHECK_EQ(db.GetAllNets().size(), std::size_t{2});
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

    QL_TEST(NextSequenceNumberIsOnePastTheHighest)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t net = AddTestNet(&db, "A");
        std::int64_t session = AddTestInstance(&db, net, "2026-01-01", 100, "W4KWK");
        std::int64_t other = AddTestInstance(&db, net, "2026-01-08", 200, "W4KWK");
        AddTestCheckIn(&db, other, "K4ZZZ", 9);  // Another session's numbers don't count.
        for (const char* callsign : {"K4AAA", "K4BBB", "K4CCC"})
        {
            db.RecordManualCheckInStation(MakeStation(callsign), 1);
        }
        CheckIn check_in;
        check_in.net_instance_id = session;
        check_in.callsign = "K4AAA";
        db.AddCheckInAtNextSequence(check_in);
        check_in.callsign = "K4BBB";
        std::int64_t second = db.AddCheckInAtNextSequence(check_in);
        check_in.callsign = "K4CCC";
        db.AddCheckInAtNextSequence(check_in);
        db.DeleteCheckIn(second);
        // Recorded after the delete, which clears out unused stations.
        db.RecordManualCheckInStation(MakeStation("K4DDD"), 1);
        check_in.callsign = "K4DDD";
        db.AddCheckInAtNextSequence(check_in);
        std::vector<CheckIn> check_ins = db.GetCheckInsForNetInstance(session);
        REQUIRE(check_ins.size() == 3);
        CHECK_EQ(check_ins[0].sequence_number, 1);
        CHECK_EQ(check_ins[1].sequence_number, 3);
        CHECK_EQ(check_ins[2].sequence_number, 4);
        CHECK_EQ(check_ins[2].callsign, std::string("K4DDD"));
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
        std::vector<Station> stations = {MakeStation("K1AAA", "One", "37415"), MakeStation("K1BBB", "Two", "30752")};
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

    static NearbyZip Near(const std::string& zip, double miles)
    {
        NearbyZip nearby;
        nearby.zip = zip;
        nearby.miles = miles;
        return nearby;
    }

    QL_TEST(BlankGridSquaresAreFilledFromTheZipAndTypedOnesKept)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        Station blank = MakeStation("K1AAA", "", "37415");
        Station zip_plus_four = MakeStation("K1BBB", "", "374152623");
        Station typed = MakeStation("K1CCC", "", "37415");
        typed.grid_square = "EM74xx";
        Station no_centroid = MakeStation("K1DDD", "", "37499");
        Station canadian = MakeStation("VE3AAA", "", "K1A 0B1");
        db.UpsertStation(blank);
        db.UpsertStation(zip_plus_four);
        db.UpsertStation(typed);
        db.UpsertStation(no_centroid);
        db.UpsertStation(canadian);
        std::vector<ZipCentroid> centroids = {{"37415", 35.10, -85.28}};
        db.BulkUpsertZipCentroids(centroids);

        CHECK_EQ(db.FillBlankGridSquaresFromZip(), 2);
        CHECK_EQ(db.FindStationByCallsign("K1AAA")->grid_square, std::string("EM75"));
        CHECK_EQ(db.FindStationByCallsign("K1BBB")->grid_square, std::string("EM75"));
        CHECK_EQ(db.FindStationByCallsign("K1CCC")->grid_square, std::string("EM74xx"));
        CHECK_EQ(db.FindStationByCallsign("K1DDD")->grid_square, std::string());
        CHECK_EQ(db.FindStationByCallsign("VE3AAA")->grid_square, std::string());
        // Nothing left to fill the next time.
        CHECK_EQ(db.FillBlankGridSquaresFromZip(), 0);
    }

    QL_TEST(NearbyUlsSearchListsTheNearestFirst)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::vector<Station> stations = {MakeStation("K1AAA", "", "37402"),   // 12 mi
                                         MakeStation("K1BBB", "", "37415"),   // 0 mi
                                         MakeStation("K1CCC", "", "37415"),   // 0 mi
                                         MakeStation("K1DDD", "", "37450"),   // Same prefix, beyond the radius.
                                         MakeStation("K1EEE", "", "37499"),   // No centroid on file (PO Box ZIP).
                                         MakeStation("K1FFF", "", "90210"),   // Far away.
                                         MakeStation("W9ZZZ", "", "37415")};  // Doesn't match.
        db.BulkUpsertUlsStations(stations, 0, stations.size(), 1);
        std::vector<ZipCentroid> centroids = {
            {"37402", 35.05, -85.31}, {"37415", 35.10, -85.28}, {"37450", 36.5, -84.0}};
        db.BulkUpsertZipCentroids(centroids);

        std::vector<NearbyZip> nearby = {Near("37415", 0.0), Near("37402", 12.4)};
        std::vector<NearbyUlsStation> found = db.SearchNearbyUlsStations("k1", nearby, {"374"}, 8);
        REQUIRE(found.size() == 4);
        CHECK_EQ(found[0].station.callsign, std::string("K1BBB"));
        CHECK_EQ(found[1].station.callsign, std::string("K1CCC"));
        CHECK_EQ(found[2].station.callsign, std::string("K1AAA"));
        CHECK(found[2].miles > 12.3 && found[2].miles < 12.5);
        CHECK_EQ(found[3].station.callsign, std::string("K1EEE"));
        CHECK_EQ(found[3].miles, -1.0);

        CHECK_EQ(db.SearchNearbyUlsStations("K1", nearby, {"374"}, 2).size(), std::size_t{2});

        // The in-memory list: every nearby station, the same order, callsign
        // and distance only.
        std::vector<NearbyUlsCallsign> listed = db.ListNearbyUlsCallsigns(nearby, {"374"});
        REQUIRE(listed.size() == 5);
        CHECK_EQ(std::string(listed[0].callsign), std::string("K1BBB"));
        CHECK_EQ(std::string(listed[1].callsign), std::string("K1CCC"));
        CHECK_EQ(std::string(listed[2].callsign), std::string("W9ZZZ"));
        CHECK_EQ(std::string(listed[3].callsign), std::string("K1AAA"));
        CHECK(listed[3].miles > 12.3F && listed[3].miles < 12.5F);
        CHECK_EQ(std::string(listed[4].callsign), std::string("K1EEE"));
        CHECK(listed[4].miles < 0.0F);

        // Single lookups, and the centroids in a box.
        CHECK_EQ(db.FindZipCentroid("37402")->lat, 35.05);
        CHECK(!db.FindZipCentroid("99999").has_value());
        CHECK_EQ(db.GetZipCentroidsInBox(35.0, 35.2, -85.4, -85.2).size(), std::size_t{2});
        // A prefix ending in 9 (whose "next" prefix isn't a digit).
        std::vector<NearbyZip> nine = {Near("37900", 5.0)};
        db.BulkUpsertUlsStations({MakeStation("K1GGG", "", "37900"), MakeStation("K1HHH", "", "37999")}, 0, 2, 1);
        CHECK_EQ(db.SearchNearbyUlsStations("K1", nine, {"379"}, 8).size(), std::size_t{2});
        CHECK(db.SearchNearbyUlsStations("K1", {}, {"374"}, 8).empty());
    }

    QL_TEST(ZipCountyDataIsReplacedWholesale)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        db.ReplaceZipCountyData({{"37415", "Hamilton"}, {"30752", "Dade"}}, {{"02467", "NEWTON", "Middlesex"}});
        CHECK_EQ(db.GetAllZipCounties().size(), std::size_t{2});
        db.ReplaceZipCountyData({{"37415", "Hamilton"}}, {});
        CHECK_EQ(db.GetAllZipCounties().size(), std::size_t{1});
        CHECK_EQ(db.FindZipCounty("37415"), std::string("Hamilton"));
        CHECK_EQ(db.FindZipCounty("30752"), std::string(""));
        CHECK(db.GetAllZipPlaceCounties().empty());
    }

    // ---- SSH users -----------------------------------------------------------------

    QL_TEST(AUserCanHaveSeveralKeys)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        User user;
        user.username = "wes";
        user.public_key = "ssh-ed25519 AAAA laptop";
        CHECK(db.CreateUser(user));
        user.public_key = "ssh-ed25519 BBBB desktop";
        CHECK(db.CreateUser(user));  // Same name, another key: both kept.
        std::vector<User> keys = db.GetUserKeys("wes");
        REQUIRE(keys.size() == 2);
        CHECK_EQ(keys[0].public_key, std::string("ssh-ed25519 AAAA laptop"));
        CHECK_EQ(keys[1].public_key, std::string("ssh-ed25519 BBBB desktop"));

        // The same key again only updates its comment.
        user.public_key = "ssh-ed25519 AAAA old-laptop";
        CHECK(!db.CreateUser(user));
        keys = db.GetUserKeys("wes");
        REQUIRE(keys.size() == 2);
        CHECK_EQ(keys[0].public_key, std::string("ssh-ed25519 AAAA old-laptop"));

        db.UpdateUserLastLogin(keys[1].id, 99);
        keys = db.GetUserKeys("wes");
        CHECK_EQ(keys[0].last_login_at, std::int64_t{0});
        CHECK_EQ(keys[1].last_login_at, std::int64_t{99});

        db.DeleteUserKey(keys[0].id);
        keys = db.GetUserKeys("wes");
        REQUIRE(keys.size() == 1);
        CHECK_EQ(keys[0].public_key, std::string("ssh-ed25519 BBBB desktop"));
        db.DeleteUserKey(keys[0].id);
        CHECK(db.GetUserKeys("wes").empty());
    }

    QL_TEST(ViewOnlyBelongsToTheUsername)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        User user;
        user.username = "viewer";
        user.public_key = "ssh-ed25519 AAAA laptop";
        user.view_only = true;
        db.CreateUser(user);
        // Another key takes the username's access, not its own.
        user.public_key = "ssh-ed25519 BBBB desktop";
        user.view_only = false;
        db.CreateUser(user);
        for (const User& key : db.GetUserKeys("viewer"))
        {
            CHECK(key.view_only);
        }
        db.SetUserViewOnly("viewer", false);
        CHECK(!db.IsUserViewOnly("viewer"));
        for (const User& key : db.GetUserKeys("viewer"))
        {
            CHECK(!key.view_only);
        }
        CHECK(!db.IsUserViewOnly("nobody"));
    }

    QL_TEST(OldUsersTableGainsKeyRows)
    {
        TempDir dir;
        std::string path = dir.File("q.db");
        {
            Database db(path);
        }
        // As a 1.4.3 database has it: one key per username.
        RunSql(path, R"sql(
            DROP TABLE users;
            CREATE TABLE users (username TEXT PRIMARY KEY, public_key TEXT NOT NULL,
                created_at INTEGER NOT NULL DEFAULT 0, last_login_at INTEGER NOT NULL DEFAULT 0);
            INSERT INTO users VALUES ('wes', 'ssh-ed25519 AAAA laptop', 5, 7);
            PRAGMA user_version = 6;
        )sql");

        Database db(path);
        std::vector<User> keys = db.GetUserKeys("wes");
        REQUIRE(keys.size() == 1);
        CHECK(keys[0].id > 0);
        CHECK_EQ(keys[0].public_key, std::string("ssh-ed25519 AAAA laptop"));
        CHECK_EQ(keys[0].created_at, std::int64_t{5});
        CHECK_EQ(keys[0].last_login_at, std::int64_t{7});
        CHECK(!keys[0].view_only);  // Everyone already there stays a full user.
        User user;
        user.username = "wes";
        user.public_key = "ssh-ed25519 BBBB desktop";
        CHECK(db.CreateUser(user));
        CHECK_EQ(db.GetUserKeys("wes").size(), std::size_t{2});
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
