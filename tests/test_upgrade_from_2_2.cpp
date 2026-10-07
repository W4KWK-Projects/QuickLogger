// A real QuickLogger 2.2.0 database, upgraded on its first open by this
// version. tests/fixtures/quicklogger-2.2.0.sql was written by 2.2.0's own
// code (tests/fixtures/make_database_2_2_0.cpp); an upgrade is one-way, so
// nothing in it may be lost or changed except where listed below.

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "../src/db/database.hpp"
#include "../src/net_slice.hpp"
#include "../src/uls_import.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // A database at `path` made by running `sql_file`, never opened by
    // Database.
    static void LoadSqlFile(const std::string& sql_file, const std::string& path)
    {
        std::ifstream in(sql_file);
        REQUIRE(in.good());
        std::stringstream sql;
        sql << in.rdbuf();
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        // .dump's own sqlite_sequence lines need this, which some builds
        // (Apple's) turn on by default.
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 0, nullptr);
        char* error = nullptr;
        int status = sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &error);
        std::string message = error != nullptr ? error : "";
        sqlite3_free(error);
        sqlite3_close(db);
        CHECK_EQ(message, std::string());
        REQUIRE(status == SQLITE_OK);
    }

    // Each row of `sql`'s results, its columns joined by '|'.
    static std::vector<std::string> QueryRows(sqlite3* db, const std::string& sql)
    {
        std::vector<std::string> rows;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        {
            rows.push_back(std::string("error: ") + sqlite3_errmsg(db) + " in " + sql);
            return rows;
        }
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            std::string row;
            for (int i = 0; i < sqlite3_column_count(statement); ++i)
            {
                const unsigned char* text = sqlite3_column_text(statement, i);
                row += (i > 0 ? "|" : "") + std::string(text != nullptr ? reinterpret_cast<const char*>(text) : "NULL");
            }
            rows.push_back(row);
        }
        sqlite3_finalize(statement);
        return rows;
    }

    // `rows` as one string, a row a line, for CHECK_EQ.
    static std::string Lines(const std::vector<std::string>& rows)
    {
        std::string lines;
        for (const std::string& row : rows)
        {
            lines += row + "\n";
        }
        return lines;
    }

    static std::string FixturePath()
    {
        return std::string(QL_TEST_FIXTURES_DIR) + "/quicklogger-2.2.0.sql";
    }

    // The 2.2.0 database before (`old`, as 2.2.0 left it) and after this
    // version opened it, as one connection to the upgraded one with the
    // other attached.
    class UpgradedFixture22
    {
    public:
        UpgradedFixture22()
        {
            LoadSqlFile(FixturePath(), dir_.File("before.db"));
            LoadSqlFile(FixturePath(), dir_.File("quicklogger.db"));
            {
                Database upgrade(dir_.File("quicklogger.db"));
            }
            sqlite3_open(dir_.File("quicklogger.db").c_str(), &db_);
            QueryRows(db_, "ATTACH DATABASE '" + dir_.File("before.db") + "' AS old");
        }
        ~UpgradedFixture22()
        {
            sqlite3_close(db_);
        }
        UpgradedFixture22(const UpgradedFixture22&) = delete;
        UpgradedFixture22& operator=(const UpgradedFixture22&) = delete;

        sqlite3* db() const
        {
            return db_;
        }
        std::string path() const
        {
            return dir_.File("quicklogger.db");
        }

    private:
        TempDir dir_;
        sqlite3* db_ = nullptr;
    };

    QL_TEST(Upgrade22KeepsEveryRowAndColumn)
    {
        UpgradedFixture22 f;
        CHECK_EQ(QueryRows(f.db(), "PRAGMA old.user_version")[0], std::string("18"));
        CHECK_EQ(QueryRows(f.db(), "PRAGMA user_version")[0], std::string("19"));

        std::vector<std::string> tables =
            QueryRows(f.db(), "SELECT name FROM old.sqlite_schema WHERE type = 'table' ORDER BY name");
        REQUIRE(tables.size() >= 12);
        for (const std::string& table : tables)
        {
            // Every 2.2.0 column, still there, holding the same values in
            // every row: nothing lost, nothing added, nothing changed.
            std::vector<std::string> columns =
                QueryRows(f.db(), "SELECT name FROM pragma_table_info('" + table + "', 'old')");
            std::string list;
            for (const std::string& column : columns)
            {
                list += (list.empty() ? "" : ", ") + std::string("\"") + column + "\"";
            }
            std::vector<std::string> lost =
                QueryRows(f.db(), "SELECT " + list + " FROM old.\"" + table + "\" EXCEPT SELECT " + list +
                                      " FROM main.\"" + table + "\"");
            std::vector<std::string> gained =
                QueryRows(f.db(), "SELECT " + list + " FROM main.\"" + table + "\" EXCEPT SELECT " + list +
                                      " FROM old.\"" + table + "\"");
            for (const std::string& row : lost)
            {
                RecordFailure(__FILE__, __LINE__, table + ": lost or changed: " + row);
            }
            for (const std::string& row : gained)
            {
                RecordFailure(__FILE__, __LINE__, table + ": new or changed: " + row);
            }
            CHECK_EQ(QueryRows(f.db(), "SELECT COUNT(*) FROM old.\"" + table + "\"")[0],
                     QueryRows(f.db(), "SELECT COUNT(*) FROM main.\"" + table + "\"")[0]);
        }

        // Every 2.2.0 index is still there.
        for (const std::string& index :
             QueryRows(f.db(),
                       "SELECT name FROM old.sqlite_schema WHERE type = 'index' AND sql IS NOT NULL "
                       "EXCEPT SELECT name FROM main.sqlite_schema WHERE type = 'index'"))
        {
            RecordFailure(__FILE__, __LINE__, "index lost: " + index);
        }
    }

    QL_TEST(Upgrade22KeepsEachNetsMemberId)
    {
        UpgradedFixture22 f;
        // Each net's own ID, one station with a different ID on each of two
        // nets, and a station with none.
        CHECK_EQ(Lines(QueryRows(f.db(),
                                 "SELECT n.name, ns.callsign, ns.member_id FROM net_saved_stations ns "
                                 "JOIN nets n ON n.id = ns.net_id ORDER BY n.name, ns.callsign, ns.name")),
                 std::string("Bare Net|KX0TST|10001\n"
                             "Cross-Border HF Net|VE3ZZD|VE-77\n"
                             "Family GMRS Net|WZZZ123|\n"
                             "Family GMRS Net|WZZZ123|\n"
                             "Fusion Net|KX0ZZA|CLUB-7\n"
                             "Test County Skywarn|AB4ZZE|\n"
                             "Test County Skywarn|KX0ZZA|SW-2002\n"));
        Database db(f.path());
        std::int64_t skywarn = 0;
        std::int64_t fusion = 0;
        for (const Net& net : db.GetAllNets())
        {
            skywarn = net.name == "Test County Skywarn" ? net.id : skywarn;
            fusion = net.name == "Fusion Net" ? net.id : fusion;
        }
        CHECK_EQ(db.GetNetMemberId(skywarn, "KX0ZZA"), std::string("SW-2002"));
        CHECK_EQ(db.GetNetMemberId(fusion, "KX0ZZA"), std::string("CLUB-7"));
    }

    QL_TEST(Upgrade22KeysStartOnAndSelfServiceStartsOff)
    {
        UpgradedFixture22 f;
        // Every existing key was put there by the admin and works.
        CHECK_EQ(Lines(QueryRows(f.db(), "SELECT added_by_user, disabled FROM users ORDER BY id")),
                 std::string("0|0\n0|0\n0|0\n0|0\n0|0\n"));
        // The key log is empty and the users' switch is off.
        CHECK_EQ(QueryRows(f.db(), "SELECT COUNT(*) FROM key_log")[0], std::string("0"));
        CHECK_EQ(QueryRows(f.db(), "SELECT COUNT(*) FROM server_options")[0], std::string("0"));
        Database db(f.path());
        CHECK(!db.ServerOptionOn(kOptionSelfServiceKeys));
        CHECK(db.RecentKeyEvents(10).empty());
        for (const User& key : db.GetUserKeys("KX0TST"))
        {
            CHECK(!key.disabled);
            CHECK(!key.added_by_user);
        }
        // An old key can be turned off and on, and a key log line written.
        std::vector<User> keys = db.GetUserKeys("KX0TST");
        REQUIRE(keys.size() == 2);
        db.SetUserKeyDisabled(keys[0].id, true);
        CHECK(db.GetUserKeys("KX0TST")[0].disabled);
        CHECK(!db.GetUserKeys("KX0TST")[1].disabled);
        KeyEvent event;
        event.at = 1790000000;
        event.actor = "console";
        event.action = "disabled";
        event.username = "KX0TST";
        db.LogKeyEvent(event);
        CHECK_EQ(db.RecentKeyEvents(10).size(), std::size_t{1});
    }

    QL_TEST(Upgrade22KeepsTheKeysTransferMethods)
    {
        UpgradedFixture22 f;
        CHECK_EQ(QueryRows(f.db(), "SELECT COUNT(*) FROM users")[0], std::string("5"));
        CHECK_EQ(Lines(QueryRows(f.db(), "SELECT transfer_method FROM users ORDER BY id")),
                 std::string("1\n2\n0\n0\n0\n"));
        CHECK_EQ(Lines(QueryRows(f.db(),
                                 "SELECT username, amateur_callsign, gmrs_callsign, view_only FROM users "
                                 "ORDER BY id")),
                 std::string(
                     "KX0TST|KX0TST||0\nKX0TST|KX0TST||0\nKX0ZZA|KX0ZZA||1\nkd4zzb|KD4ZZB||0\nWQXX000||WQXX000|0\n"));
    }

    QL_TEST(Upgrade22DatabaseWorks)
    {
        TempDir dir;
        std::string path = dir.File("quicklogger.db");
        LoadSqlFile(FixturePath(), path);
        Database db(path);

        std::vector<Net> nets = db.GetAllNets();
        REQUIRE(nets.size() == 6);
        const Net* skywarn = nullptr;
        const Net* gmrs = nullptr;
        for (const Net& net : nets)
        {
            if (net.name == "Test County Skywarn")
            {
                skywarn = &net;
            }
            if (net.name == "Family GMRS Net")
            {
                gmrs = &net;
            }
        }
        REQUIRE(skywarn != nullptr);
        REQUIRE(gmrs != nullptr);
        CHECK_EQ(skywarn->mode, std::string("FM"));
        CHECK_EQ(skywarn->default_frequency, std::string("145.390"));
        CHECK(skywarn->service == NetService::kAmateur);
        CHECK_EQ(skywarn->comments, std::string("Weather spotters.\nSecond line, with \"quotes\"."));

        // A GMRS net keeps its service, its family told apart by name, and
        // the session that was pushed.
        CHECK(gmrs->service == NetService::kGmrs);
        CHECK_EQ(gmrs->default_frequency, std::string("462.5625"));
        CHECK_EQ(db.GetSavedStationsForNet(gmrs->id).size(), std::size_t{2});
        CHECK_EQ(db.GetSavedNetStationRemarks(gmrs->id, "WZZZ123", "Pat"), std::string("Mom"));
        CHECK_EQ(db.GetSavedNetStationRemarks(gmrs->id, "WZZZ123", "Alex"), std::string("Kid"));
        std::vector<NetInstance> family = db.GetNetInstancesForNet(gmrs->id);
        REQUIRE(family.size() == 1);
        CHECK(family[0].pushed_at != 0);
        std::vector<CheckIn> family_check_ins = db.GetCheckInsForNetInstance(family[0].id);
        REQUIRE(family_check_ins.size() == 2);
        CHECK_EQ(family_check_ins[0].name, std::string("Pat"));
        CHECK_EQ(family_check_ins[1].name, std::string("Alex"));

        // Sessions newest first, with their roles, check-ins and notes.
        std::vector<NetInstance> sessions = db.GetNetInstancesForNet(skywarn->id);
        REQUIRE(sessions.size() == 2);
        CHECK_EQ(sessions[1].notes, std::string("Severe watch until 10pm.\nNo damage reports."));
        CHECK_EQ(sessions[0].logger_callsign, std::string("KX0ZZA"));
        std::vector<CheckIn> check_ins = db.GetCheckInsForNetInstance(sessions[1].id);
        REQUIRE(check_ins.size() == 4);
        CHECK_EQ(check_ins[1].comment, std::string("Heavy rain"));
        std::optional<Station> accented = db.FindStationByCallsign("KD4ZZB");
        REQUIRE(accented.has_value());
        CHECK_EQ(accented->name, std::string("Peña, José"));
        CHECK_EQ(db.GetAdHocNetInstances().size(), std::size_t{1});

        // The open session is still open, and can be logged and closed.
        std::vector<std::int64_t> open = db.GetNetIdsWithOpenInstances();
        REQUIRE(open.size() == 1);
        std::vector<NetInstance> open_net = db.GetNetInstancesForNet(open[0]);
        REQUIRE(open_net.size() == 1);
        std::int64_t open_id = open_net[0].id;
        AddTestCheckIn(&db, open_id, "K4NEW", 3);
        CHECK(db.CloseNetInstance(open_id, 1790600000));
        CHECK_EQ(db.GetCheckInsForNetInstance(open_id).size(), std::size_t{3});

        // A closed session can be exported, as F7 and a push do.
        std::string error;
        CHECK(WriteNetSliceFile(dir.File("Skywarn.qlsession"), GatherSessionSlice(&db, sessions[1].id), &error));
        CHECK_EQ(error, std::string());

        // Users, their keys and their access; a key's Transfer method is
        // kept, and can be changed.
        std::vector<User> keys = db.GetUserKeys("KX0TST");
        REQUIRE(keys.size() == 2);
        CHECK_EQ(keys[0].transfer_method, kTransferZmodem);
        CHECK_EQ(keys[1].transfer_method, kTransferSftp);
        db.UpdateUserKeyTransfer(keys[0].id, kTransferAsk);
        CHECK_EQ(db.GetUserKeys("KX0TST")[0].transfer_method, kTransferAsk);
        CHECK_EQ(db.GetUserKeys("KX0TST")[1].transfer_method, kTransferSftp);
        CHECK(db.IsUserViewOnly("KX0ZZA"));
        CHECK(!db.IsUserViewOnly("KD4ZZB"));
        CHECK_EQ(db.GetUserKeys("WQXX000")[0].gmrs_callsign, std::string("WQXX000"));

        // The station data 2.2.0 downloaded still counts as current.
        DataRefreshPlan plan = PlanDataRefresh(&db, 1790000000);
        CHECK(!plan.uls);
        CHECK(!plan.ised);
        CHECK(!plan.zip_centroids);
        CHECK(!plan.zip_counties);
        CHECK(!plan.gmrs);
        CHECK(!plan.ca_postal);
        CHECK(db.FindUlsStationByCallsign("W4ZZF").has_value());
        CHECK(db.FindUlsStationByCallsign("WZZZ123", LicenseTable::kGmrs).has_value());
    }

    QL_TEST(Upgrade22HappensOnce)
    {
        TempDir dir;
        std::string path = dir.File("quicklogger.db");
        LoadSqlFile(FixturePath(), path);
        {
            Database first(path);
        }
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        std::string once = Lines(QueryRows(db, "SELECT * FROM nets ORDER BY id"));
        std::string users_once = Lines(QueryRows(db, "SELECT * FROM users ORDER BY id"));
        sqlite3_close(db);
        {
            Database second(path);
        }
        sqlite3_open(path.c_str(), &db);
        CHECK_EQ(Lines(QueryRows(db, "SELECT * FROM nets ORDER BY id")), once);
        CHECK_EQ(Lines(QueryRows(db, "SELECT * FROM users ORDER BY id")), users_once);
        CHECK_EQ(QueryRows(db, "PRAGMA user_version")[0], std::string("19"));
        sqlite3_close(db);
    }

}  // namespace ql
