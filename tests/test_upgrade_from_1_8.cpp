// A real QuickLogger 1.8.6 database, upgraded on its first open by this
// version. tests/fixtures/quicklogger-1.8.6.sql was written by 1.8.6's own
// code (tests/fixtures/make_database_1_8_6.cpp); an upgrade is one-way, so
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
        return std::string(QL_TEST_FIXTURES_DIR) + "/quicklogger-1.8.6.sql";
    }

    // The 1.8.6 database before (`old`, as 1.8.6 left it) and after this
    // version opened it, as one connection to the upgraded one with the
    // other attached.
    class UpgradedFixture
    {
    public:
        UpgradedFixture()
        {
            LoadSqlFile(FixturePath(), dir_.File("before.db"));
            LoadSqlFile(FixturePath(), dir_.File("quicklogger.db"));
            {
                Database upgrade(dir_.File("quicklogger.db"));
            }
            sqlite3_open(dir_.File("quicklogger.db").c_str(), &db_);
            QueryRows(db_, "ATTACH DATABASE '" + dir_.File("before.db") + "' AS old");
        }
        ~UpgradedFixture()
        {
            sqlite3_close(db_);
        }
        UpgradedFixture(const UpgradedFixture&) = delete;
        UpgradedFixture& operator=(const UpgradedFixture&) = delete;

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

    QL_TEST(Upgrade18KeepsEveryRowAndColumn)
    {
        UpgradedFixture f;
        CHECK_EQ(QueryRows(f.db(), "PRAGMA old.user_version")[0], std::string("13"));
        CHECK_EQ(QueryRows(f.db(), "PRAGMA user_version")[0], std::string("16"));

        std::vector<std::string> tables =
            QueryRows(f.db(), "SELECT name FROM old.sqlite_schema WHERE type = 'table' ORDER BY name");
        REQUIRE(tables.size() >= 12);
        for (const std::string& table : tables)
        {
            // Every 1.8.6 column, still there, holding the same values in
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

        // Every 1.8.6 index is still there.
        for (const std::string& index :
             QueryRows(f.db(),
                       "SELECT name FROM old.sqlite_schema WHERE type = 'index' AND sql IS NOT NULL "
                       "EXCEPT SELECT name FROM main.sqlite_schema WHERE type = 'index'"))
        {
            RecordFailure(__FILE__, __LINE__, "index lost: " + index);
        }
    }

    QL_TEST(Upgrade18FillsTheNewColumns)
    {
        UpgradedFixture f;
        // Every net is Amateur Radio; nothing has been pushed; no check-in
        // or saved station has a name of its own (those are GMRS's).
        CHECK_EQ(Lines(QueryRows(f.db(), "SELECT DISTINCT service FROM nets")), std::string("amateur\n"));
        CHECK_EQ(QueryRows(f.db(), "SELECT COUNT(*) FROM net_instances WHERE pushed_at != 0")[0], std::string("0"));
        CHECK_EQ(QueryRows(f.db(), "SELECT COUNT(*) FROM check_ins WHERE name != ''")[0], std::string("0"));
        CHECK_EQ(QueryRows(f.db(), "SELECT COUNT(*) FROM net_saved_stations WHERE name != ''")[0], std::string("0"));
        // A username was an amateur call sign, so it becomes the user's
        // amateur call sign, in capitals.
        CHECK_EQ(Lines(QueryRows(f.db(), "SELECT username, amateur_callsign, gmrs_callsign FROM users ORDER BY id")),
                 std::string("W4TST|W4TST|\nW4TST|W4TST|\nKQ4ZZA|KQ4ZZA|\nkd4zzb|KD4ZZB|\n"));
    }

    QL_TEST(Upgrade18DatabaseWorks)
    {
        TempDir dir;
        std::string path = dir.File("quicklogger.db");
        LoadSqlFile(FixturePath(), path);
        Database db(path);

        std::vector<Net> nets = db.GetAllNets();
        REQUIRE(nets.size() == 5);
        const Net* skywarn = nullptr;
        for (const Net& net : nets)
        {
            if (net.name == "Test County Skywarn")
            {
                skywarn = &net;
            }
        }
        REQUIRE(skywarn != nullptr);
        CHECK_EQ(skywarn->mode, std::string("FM"));
        CHECK_EQ(skywarn->default_frequency, std::string("145.390"));
        CHECK_EQ(skywarn->repeater_offset, std::string("-0.6"));
        CHECK_EQ(skywarn->pl_tone, std::string("107.2"));
        CHECK_EQ(skywarn->default_location, std::string("37415"));
        CHECK_EQ(skywarn->comments, std::string("Weather spotters.\nSecond line, with \"quotes\"."));

        // Sessions newest first, with their roles, check-ins and notes.
        std::vector<NetInstance> sessions = db.GetNetInstancesForNet(skywarn->id);
        REQUIRE(sessions.size() == 2);
        CHECK_EQ(sessions[1].notes, std::string("Severe watch until 10pm.\nNo damage reports."));
        CHECK_EQ(sessions[0].logger_callsign, std::string("KQ4ZZA"));
        CHECK_EQ(sessions[0].net_control_callsign, std::string("W4TST"));
        std::vector<CheckIn> check_ins = db.GetCheckInsForNetInstance(sessions[1].id);
        REQUIRE(check_ins.size() == 4);
        CHECK_EQ(check_ins[1].remarks, std::string("Spotter 12"));
        CHECK_EQ(check_ins[1].comment, std::string("Heavy rain"));
        CHECK_EQ(check_ins[2].designated_role, kRoleAlternateNetControl);
        CHECK_EQ(check_ins[3].callsign, std::string("N4ZZC/P"));
        std::optional<Station> accented = db.FindStationByCallsign("KD4ZZB");
        REQUIRE(accented.has_value());
        CHECK_EQ(accented->name, std::string("Peña, José"));
        CHECK_EQ(db.GetSavedNetStationRemarks(skywarn->id, "KQ4ZZA"), std::string("Mobile spotter"));
        CHECK_EQ(db.GetSavedStationsForNet(skywarn->id).size(), std::size_t{2});
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

        // Users and their access.
        CHECK_EQ(db.GetUserKeys("W4TST").size(), std::size_t{2});
        CHECK(db.IsUserViewOnly("KQ4ZZA"));
        CHECK(!db.IsUserViewOnly("KD4ZZB"));

        // The station data 1.8.6 downloaded still counts as current; only
        // what's new since (GMRS licensees) is due.
        DataRefreshPlan plan = PlanDataRefresh(&db, 1790000000);
        CHECK(!plan.uls);
        CHECK(!plan.ised);
        CHECK(!plan.zip_centroids);
        CHECK(!plan.zip_counties);
        CHECK(plan.gmrs);
        CHECK(plan.ca_postal);
        CHECK(db.FindUlsStationByCallsign("W4ZZF").has_value());
    }

    QL_TEST(Upgrade18HappensOnce)
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
        std::string saved_once = Lines(QueryRows(db, "SELECT * FROM net_saved_stations ORDER BY 1, 2"));
        sqlite3_close(db);
        {
            Database second(path);
        }
        sqlite3_open(path.c_str(), &db);
        CHECK_EQ(Lines(QueryRows(db, "SELECT * FROM nets ORDER BY id")), once);
        CHECK_EQ(Lines(QueryRows(db, "SELECT * FROM net_saved_stations ORDER BY 1, 2")), saved_once);
        CHECK_EQ(QueryRows(db, "PRAGMA user_version")[0], std::string("16"));
        sqlite3_close(db);
    }

}  // namespace ql
