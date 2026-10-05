#include "database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include "../frequency_rules.hpp"
#include "../geo_utils.hpp"
#include "../mode_rules.hpp"
#include "../public_key.hpp"
#include "../text_utils.hpp"
#include "sqlite_statement.hpp"

namespace ql
{

    static const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS stations (
    callsign TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    member_id TEXT NOT NULL DEFAULT '',
    street_address TEXT NOT NULL DEFAULT '',
    city TEXT NOT NULL DEFAULT '',
    county TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT '',
    zip TEXT NOT NULL DEFAULT '',
    grid_square TEXT NOT NULL DEFAULT '',
    license_class TEXT NOT NULL DEFAULT '',
    email TEXT NOT NULL DEFAULT '',
    data_source INTEGER NOT NULL DEFAULT 0,
    last_updated INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS nets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    mode TEXT NOT NULL DEFAULT '',
    default_frequency TEXT NOT NULL DEFAULT '',
    default_location TEXT NOT NULL DEFAULT '',
    default_grid_square TEXT NOT NULL DEFAULT '',
    recurrence_description TEXT NOT NULL DEFAULT '',
    notes TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL DEFAULT 0,
    imported_at INTEGER NOT NULL DEFAULT 0,
    is_ad_hoc INTEGER NOT NULL DEFAULT 0,
    repeater_offset TEXT NOT NULL DEFAULT '',
    pl_tone TEXT NOT NULL DEFAULT '',
    partial_match_canada INTEGER NOT NULL DEFAULT 0,
    service TEXT NOT NULL DEFAULT 'amateur'
);

CREATE TABLE IF NOT EXISTS net_instances (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    net_id INTEGER NOT NULL REFERENCES nets(id),
    instance_date TEXT NOT NULL,
    net_control_callsign TEXT NOT NULL DEFAULT '',
    alternate_net_control_callsign TEXT NOT NULL DEFAULT '',
    logger_callsign TEXT NOT NULL DEFAULT '',
    created_by TEXT NOT NULL DEFAULT '',
    frequency TEXT NOT NULL DEFAULT '',
    location TEXT NOT NULL DEFAULT '',
    status INTEGER NOT NULL DEFAULT 0,
    closed_at INTEGER NOT NULL DEFAULT 0,
    operator_role INTEGER NOT NULL DEFAULT 0,
    started_at INTEGER NOT NULL DEFAULT 0,
    notes TEXT NOT NULL DEFAULT '',
    pushed_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_net_instances_net ON net_instances(net_id);

CREATE TABLE IF NOT EXISTS check_ins (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    net_instance_id INTEGER NOT NULL REFERENCES net_instances(id),
    callsign TEXT NOT NULL REFERENCES stations(callsign),
    sequence_number INTEGER NOT NULL DEFAULT 0,
    signal_report TEXT NOT NULL DEFAULT '',
    remarks TEXT NOT NULL DEFAULT '',
    comment TEXT NOT NULL DEFAULT '',
    checked_in_at INTEGER NOT NULL DEFAULT 0,
    designated_role INTEGER NOT NULL DEFAULT -1,
    name TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_check_ins_net_instance ON check_ins(net_instance_id);
CREATE INDEX IF NOT EXISTS idx_check_ins_callsign ON check_ins(callsign);

-- On a GMRS net a call sign may be saved more than once, each a different
-- person (one license covers a family), told apart by `name` ("Jane" and
-- "JANE" are one). On an
-- Amateur Radio net `name` is blank and the station's own name stands.
CREATE TABLE IF NOT EXISTS net_saved_stations (
    net_id INTEGER NOT NULL REFERENCES nets(id),
    callsign TEXT NOT NULL REFERENCES stations(callsign),
    default_remarks TEXT NOT NULL DEFAULT '',
    name TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
    PRIMARY KEY (net_id, callsign, name)
);
-- For "is this station saved anywhere" (DeleteUnusedStations and the
-- station card): the primary key starts with net_id.
CREATE INDEX IF NOT EXISTS idx_net_saved_stations_callsign ON net_saved_stations(callsign);

CREATE TABLE IF NOT EXISTS import_runs (
    source TEXT PRIMARY KEY,
    status TEXT NOT NULL DEFAULT 'never_run',
    started_at INTEGER NOT NULL DEFAULT 0,
    completed_at INTEGER NOT NULL DEFAULT 0,
    records_imported INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS uls_stations (
    callsign TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    street_address TEXT NOT NULL DEFAULT '',
    city TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT '',
    zip TEXT NOT NULL DEFAULT '',
    license_class TEXT NOT NULL DEFAULT '',
    last_updated INTEGER NOT NULL DEFAULT 0
);
-- (zip, callsign) rather than zip alone: the nearby-stations list is read
-- from this index without touching the table (ListNearbyUlsCallsigns).
CREATE INDEX IF NOT EXISTS idx_uls_stations_zip_callsign ON uls_stations(zip, callsign);

-- The FCC's GMRS licenses (l_gmrs.zip), loaded and searched like
-- uls_stations, for GMRS nets only.
CREATE TABLE IF NOT EXISTS gmrs_stations (
    callsign TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    street_address TEXT NOT NULL DEFAULT '',
    city TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT '',
    zip TEXT NOT NULL DEFAULT '',
    license_class TEXT NOT NULL DEFAULT '',
    last_updated INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_gmrs_stations_zip_callsign ON gmrs_stations(zip, callsign);

CREATE TABLE IF NOT EXISTS ised_stations (
    callsign TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    street_address TEXT NOT NULL DEFAULT '',
    city TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT '',
    zip TEXT NOT NULL DEFAULT '',
    license_class TEXT NOT NULL DEFAULT '',
    last_updated INTEGER NOT NULL DEFAULT 0
);

-- By postal code, to find the licensees in an FSA: a range on its first
-- three characters, read from the index alone (ListNearbyIsedCallsigns).
CREATE INDEX IF NOT EXISTS idx_ised_stations_zip_callsign ON ised_stations(zip, callsign);

CREATE TABLE IF NOT EXISTS zip_centroids (
    zip TEXT PRIMARY KEY,
    lat REAL NOT NULL,
    lon REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_zip_centroids_lat_lon ON zip_centroids(lat, lon, zip);

CREATE TABLE IF NOT EXISTS zip_counties (
    zip TEXT PRIMARY KEY,
    county TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS zip_place_counties (
    zip TEXT NOT NULL,
    place TEXT NOT NULL,
    county TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (zip, place)
);

CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL,
    public_key TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT 0,
    last_login_at INTEGER NOT NULL DEFAULT 0,
    view_only INTEGER NOT NULL DEFAULT 0,
    amateur_callsign TEXT NOT NULL DEFAULT '',
    gmrs_callsign TEXT NOT NULL DEFAULT '',
    transfer_method INTEGER NOT NULL DEFAULT 0
);
)sql";

    // The version of the upgrades CreateSchema has applied to this
    // database; see the comment there.
    static constexpr int kSchemaVersion = 17;

    // How SQLite waits for another connection's lock: 1 ms pauses for the
    // first 20 tries, then 5 ms ones, giving up after 5 seconds by the clock.
    // Counting tries instead ran long where sleeps overshoot (the GitHub
    // macOS runner: 7 seconds for "5"). A wait is one call chain on one
    // thread, so its start can live in a thread_local.
    static constexpr int kBusyFastTries = 20;
    static constexpr std::chrono::milliseconds kBusyLimit{5000};

    static int WaitForLock(void*, int tries)
    {
        thread_local std::chrono::steady_clock::time_point began;
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (tries == 0)
        {
            began = now;
        }
        else if (now - began >= kBusyLimit)
        {
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(tries < kBusyFastTries ? 1 : 5));
        return 1;
    }

    static int ReadUserVersion(sqlite3* db)
    {
        Statement statement(db, "PRAGMA user_version;");
        return statement.Step() ? static_cast<int>(statement.ColumnInt64(0)) : 0;
    }

    // Adds `column` to `table` if it isn't already there, for evolving the
    // schema of a database created by an older version of QuickLogger without
    // forcing the user to delete it. kSchemaSql's CREATE TABLE only covers
    // brand-new databases.
    static void EnsureColumnExists(sqlite3* db, const char* table, const char* column, const char* column_declaration)
    {
        std::string pragma_sql = std::string("PRAGMA table_info(") + table + ");";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, pragma_sql.c_str(), -1, &stmt, nullptr);

        bool exists = false;
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            if (name != nullptr && std::string(reinterpret_cast<const char*>(name)) == column)
            {
                exists = true;
                break;
            }
        }
        sqlite3_finalize(stmt);

        if (!exists)
        {
            std::string alter_sql =
                std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " " + column_declaration + ";";
            sqlite3_exec(db, alter_sql.c_str(), nullptr, nullptr, nullptr);
        }
    }

    // Orders stations by callsign.
    class StationsByCallsign
    {
    public:
        bool operator()(const Station& a, const Station& b) const
        {
            return a.callsign < b.callsign;
        }
    };

    static Station ReadStationRow(const Statement& row)
    {
        Station station;
        station.callsign = row.ColumnText(0);
        station.name = row.ColumnText(1);
        station.member_id = row.ColumnText(2);
        station.street_address = row.ColumnText(3);
        station.city = row.ColumnText(4);
        station.county = row.ColumnText(5);
        station.state = row.ColumnText(6);
        station.zip = row.ColumnText(7);
        station.grid_square = row.ColumnText(8);
        station.license_class = row.ColumnText(9);
        station.email = row.ColumnText(10);
        station.data_source = static_cast<StationDataSource>(row.ColumnInt64(11));
        station.last_updated = row.ColumnInt64(12);
        return station;
    }

    static Net ReadNetRow(const Statement& row)
    {
        Net net;
        net.id = row.ColumnInt64(0);
        net.name = row.ColumnText(1);
        net.mode = row.ColumnText(2);
        net.default_frequency = row.ColumnText(3);
        net.default_location = row.ColumnText(4);
        net.default_grid_square = row.ColumnText(5);
        net.recurrence_description = row.ColumnText(6);
        net.comments = row.ColumnText(7);
        net.created_at = row.ColumnInt64(8);
        net.imported_at = row.ColumnInt64(9);
        net.is_ad_hoc = row.ColumnInt64(10) != 0;
        net.repeater_offset = row.ColumnText(11);
        net.pl_tone = row.ColumnText(12);
        net.partial_match_canada = row.ColumnInt64(13) != 0;
        net.service = row.ColumnText(14) == "gmrs" ? NetService::kGmrs : NetService::kAmateur;
        return net;
    }

    // A NetInstance from the 15 columns starting at `first` (see
    // QL_NET_INSTANCE_COLUMNS for their order).
    static NetInstance ReadNetInstanceColumns(const Statement& row, int first)
    {
        NetInstance instance;
        instance.id = row.ColumnInt64(first);
        instance.net_id = row.ColumnInt64(first + 1);
        instance.instance_date = row.ColumnText(first + 2);
        instance.net_control_callsign = row.ColumnText(first + 3);
        instance.alternate_net_control_callsign = row.ColumnText(first + 4);
        instance.logger_callsign = row.ColumnText(first + 5);
        instance.created_by = row.ColumnText(first + 6);
        instance.frequency = row.ColumnText(first + 7);
        instance.location = row.ColumnText(first + 8);
        instance.status = static_cast<NetInstanceStatus>(row.ColumnInt64(first + 9));
        instance.closed_at = row.ColumnInt64(first + 10);
        instance.operator_role = static_cast<int>(row.ColumnInt64(first + 11));
        instance.started_at = row.ColumnInt64(first + 12);
        instance.notes = row.ColumnText(first + 13);
        instance.pushed_at = row.ColumnInt64(first + 14);
        return instance;
    }

    static NetInstance ReadNetInstanceRow(const Statement& row)
    {
        return ReadNetInstanceColumns(row, 0);
    }

    // A CheckIn from the 10 columns starting at `first` (see
    // QL_CHECK_IN_COLUMNS).
    static CheckIn ReadCheckInColumns(const Statement& row, int first)
    {
        CheckIn check_in;
        check_in.id = row.ColumnInt64(first);
        check_in.net_instance_id = row.ColumnInt64(first + 1);
        check_in.callsign = row.ColumnText(first + 2);
        check_in.sequence_number = static_cast<int>(row.ColumnInt64(first + 3));
        check_in.signal_report = row.ColumnText(first + 4);
        check_in.remarks = row.ColumnText(first + 5);
        check_in.comment = row.ColumnText(first + 6);
        check_in.checked_in_at = row.ColumnInt64(first + 7);
        check_in.designated_role = static_cast<int>(row.ColumnInt64(first + 8));
        check_in.name = row.ColumnText(first + 9);
        return check_in;
    }

    static CheckIn ReadCheckInRow(const Statement& row)
    {
        return ReadCheckInColumns(row, 0);
    }

    // The column lists the two readers above expect, for queries that join
    // net_instances (as i) and check_ins (as c).
    // Macros rather than constants so a query can be one string literal
    // (adjacent literals join at compile time), which StatementCache needs.
#define QL_NET_INSTANCE_COLUMNS                                                        \
    "i.id, i.net_id, i.instance_date, i.net_control_callsign, "                        \
    "i.alternate_net_control_callsign, i.logger_callsign, i.created_by, i.frequency, " \
    "i.location, i.status, i.closed_at, i.operator_role, i.started_at, i.notes, i.pushed_at"
#define QL_CHECK_IN_COLUMNS                                                                \
    "c.id, c.net_instance_id, c.callsign, c.sequence_number, c.signal_report, c.remarks, " \
    "c.comment, c.checked_in_at, c.designated_role, c.name"

    Database::Database(const std::string& path, bool use_wal)
    {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
        {
            std::string message = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            throw std::runtime_error("Failed to open database '" + path + "': " + message);
        }
        // SQLite allows only one writer at a time regardless of journal mode.
        // Without this, a second connection's write that arrives while
        // another is mid-transaction gets SQLITE_BUSY immediately, and
        // Statement::Step() turns that into a thrown exception -- which
        // would otherwise crash the whole app the moment two connections'
        // writes overlap even briefly. This covers every source of a second
        // connection: the background ULS import worker (its own Database on
        // a separate thread, see uls_import.hpp) *and* a second instance of
        // the app entirely, pointed at the same quicklogger.db (e.g. Net
        // Control and Logger running on separate machines against a shared
        // file). 5s comfortably covers a single bulk-upsert transaction
        // (each is a batch of 1000 rows, well under that) without ever
        // making the UI feel stuck on a genuine deadlock -- there shouldn't
        // be one, since every write here is a single short statement/
        // transaction, never a held connection waiting on user input.
        statements_.Attach(db_);
        sqlite3_busy_handler(db_, WaitForLock, nullptr);
        sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
        // Reads go through a memory map of the file rather than into each
        // connection's own page cache: the operating system keeps one copy
        // of the file's pages, shared by every session's process, instead
        // of every connection copying what it reads into memory of its own
        // (SQLite's default cache is up to 8 MB a connection on some
        // systems, and each session has two connections). The cache is then
        // left small, for the pages being written. 1 GB covers the database
        // many times over; only the parts actually read take any memory.
        sqlite3_exec(db_, "PRAGMA mmap_size = 1073741824;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA cache_size = -1024;", nullptr, nullptr, nullptr);
        // WAL lets the UI thread's reads proceed while a background import
        // (see uls_import.hpp) holds a writer transaction open on a second
        // connection to this same file. It's also what makes it safe for
        // multiple *processes* (not just threads) to open this same file at
        // once -- WAL's reader/writer model is per-connection, not
        // per-process, on a local filesystem. Skipped for `use_wal = false`
        // callers (see the header) -- there's no second connection to help
        // there, only a "-wal"/"-shm" sidecar to accidentally leave behind.
        if (use_wal)
        {
            sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
        }
        try
        {
            CreateSchema();
        }
        catch (...)
        {
            // A constructor that throws never runs its destructor: close the
            // connection here, or the file (a non-database someone uploaded,
            // say) stays open and Windows can't delete it.
            statements_.Clear();
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    // Ends the open transaction without keeping its changes. For the
    // transactions' destructors, so it never throws.
    void Database::Rollback(Database* db)
    {
        try
        {
            Statement rollback(&db->statements_, "ROLLBACK;");
            rollback.Step();
        }
        catch (const std::exception&)
        {
            // Nothing more to do: the connection rolls back when it closes.
        }
    }

    Database::ReadTransaction::ReadTransaction(Database* db) : db_(db)
    {
        if (sqlite3_get_autocommit(db_->db_) == 0)
        {
            return;  // Already inside one.
        }
        Statement begin(&db_->statements_, "BEGIN;");
        begin.Step();
        began_ = true;
    }

    Database::ReadTransaction::~ReadTransaction()
    {
        if (!began_ || sqlite3_get_autocommit(db_->db_) != 0)
        {
            return;
        }
        // Nothing was written, so there's nothing to lose if ending it
        // fails; never throw from a destructor.
        try
        {
            Statement commit(&db_->statements_, "COMMIT;");
            commit.Step();
        }
        catch (const std::exception&)
        {
            Rollback(db_);
        }
    }

    Database::WriteTransaction::WriteTransaction(Database* db) : db_(db)
    {
        if (sqlite3_get_autocommit(db_->db_) == 0)
        {
            return;  // Part of one already open.
        }
        // IMMEDIATE takes the write lock now, waiting its turn (busy
        // timeout) if another connection is writing, rather than failing
        // part-way through when a read has to become a write.
        Statement begin(&db_->statements_, "BEGIN IMMEDIATE;");
        begin.Step();
        began_ = true;
    }

    Database::WriteTransaction::~WriteTransaction()
    {
        if (began_ && !finished_ && sqlite3_get_autocommit(db_->db_) == 0)
        {
            Rollback(db_);
        }
    }

    void Database::WriteTransaction::Commit()
    {
        if (!began_ || finished_)
        {
            return;
        }
        Statement commit(&db_->statements_, "COMMIT;");
        commit.Step();
        finished_ = true;
    }

    Database::~Database()
    {
        // Cached statements must be finalized before the connection closes.
        statements_.Clear();
        sqlite3_close(db_);
    }

    void Database::CreateSchema()
    {
        char* error_message = nullptr;
        if (sqlite3_exec(db_, kSchemaSql, nullptr, nullptr, &error_message) != SQLITE_OK)
        {
            std::string message = error_message != nullptr ? error_message : "unknown error";
            sqlite3_free(error_message);
            throw std::runtime_error("Failed to create schema: " + message);
        }

        // Everything below upgrades a database created by an older version.
        // It's all idempotent, but some of it scans whole tables, and a
        // database is opened by every session, the SSH listener's
        // connections and the data updater -- so it runs once per database,
        // recorded in SQLite's user_version, rather than on every open. Bump
        // kSchemaVersion when adding to it.
        if (ReadUserVersion(db_) >= kSchemaVersion)
        {
            return;
        }

        EnsureColumnExists(db_, "stations", "street_address", "TEXT NOT NULL DEFAULT ''");
        EnsureColumnExists(db_, "net_saved_stations", "default_remarks", "TEXT NOT NULL DEFAULT ''");
        EnsureColumnExists(db_, "net_instances", "operator_role", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "net_instances", "started_at", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "check_ins", "designated_role", "INTEGER NOT NULL DEFAULT -1");
        EnsureColumnExists(db_, "import_runs", "phase", "TEXT NOT NULL DEFAULT ''");
        EnsureColumnExists(db_, "import_runs", "percent", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "import_runs", "heartbeat_at", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "import_runs", "requested_at", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "nets", "created_at", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "nets", "imported_at", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "nets", "is_ad_hoc", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "nets", "repeater_offset", "TEXT NOT NULL DEFAULT ''");
        EnsureColumnExists(db_, "nets", "pl_tone", "TEXT NOT NULL DEFAULT ''");
        // Since 2.0.0 a net is Amateur Radio or GMRS; every net already
        // there is Amateur Radio. Added before the fix-ups below, which
        // leave GMRS nets alone.
        EnsureColumnExists(db_, "nets", "service", "TEXT NOT NULL DEFAULT 'amateur'");

        // One-time migration for databases created before ULS import moved
        // to its own table (uls_stations): any leftover data_source=2 (kUls)
        // rows in `stations` came from that earlier version and would keep
        // bloating every autocomplete query against `stations` forever.
        // Idempotent -- a no-op once no such rows remain.
        sqlite3_exec(db_,
                     "INSERT OR IGNORE INTO uls_stations "
                     "(callsign, name, street_address, city, state, zip, license_class, "
                     "last_updated) "
                     "SELECT callsign, name, street_address, city, state, zip, license_class, "
                     "last_updated FROM stations WHERE data_source = 2;",
                     nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "DELETE FROM stations WHERE data_source = 2;", nullptr, nullptr, nullptr);

        // One-time fix-up for ULS rows imported before uls_import.cpp
        // trimmed FCC's run-together ZIP+4 values ("307524915") to five
        // digits: every ZIP lookup (county, distance) is keyed on the
        // five-digit form, so those rows got neither until the next weekly
        // refresh rewrote them. Idempotent -- a no-op once none remain.
        {
            Statement trim_zips(&statements_, "UPDATE uls_stations SET zip = substr(zip, 1, 5) WHERE length(zip) > 5;");
            trim_zips.Step();
        }

        // One-time fix-up for nets saved before a net's location became a
        // 5-digit ZIP field: keep the ZIP if the old free text has one
        // ("Chattanooga, TN 37415"), otherwise blank it ("Hamilton County"),
        // so an older net can still be saved from Edit Net without first
        // having to clear a value the form now rejects. A blank ZIP just
        // means autocomplete measures from the operator's home ZIP.
        NormalizeNetZips();

        // Likewise for the frequency, checked since 1.4.2: text that isn't
        // an amateur frequency in MHz moves to the net's comments, and any
        // frequency found in it stays (see MoveBadFrequencyToComments).
        NormalizeNetFrequencies();

        // Up to 1.4.3 a username had only one key.
        UpgradeUsersTable();
        // Since 1.6.0 a user can be view-only; everyone already there
        // stays a full user.
        EnsureColumnExists(db_, "users", "view_only", "INTEGER NOT NULL DEFAULT 0");
        // Since 1.6.0 covered by idx_uls_stations_zip_callsign.
        sqlite3_exec(db_, "DROP INDEX IF EXISTS idx_uls_stations_zip;", nullptr, nullptr, nullptr);
        // Since 1.7.0 partial matching is set per net; every net already
        // there is taken to be a US net.
        EnsureColumnExists(db_, "nets", "partial_match_canada", "INTEGER NOT NULL DEFAULT 0");
        // Since 1.8.0 a session has notes of its own (F12), and a net's mode
        // is one of a fixed list: a known spelling of one ("fm", "C4FM") is
        // converted, anything else blanked (see NormalizeMode).
        EnsureColumnExists(db_, "net_instances", "notes", "TEXT NOT NULL DEFAULT ''");
        NormalizeNetModes();
        // A table from QuickLogger's earliest builds, before a net's saved
        // stations had their present name. Nothing reads it, but it still
        // refers to nets and stations, so deleting a net it names failed
        // ("FOREIGN KEY constraint failed"). Anything in it that isn't
        // already a saved station becomes one first.
        DropOldSeedStations();
        // Since 2.0.0 a session records when it was last pushed upstream
        // (Federated Logging); 0 is never. Only this database's own:
        // CreateNetInstance never writes it, so exports, imports and merges
        // don't carry it.
        EnsureColumnExists(db_, "net_instances", "pushed_at", "INTEGER NOT NULL DEFAULT 0");
        // Since 2.0.0 a username is any login name, and a user's call signs
        // are kept apart from it: an amateur one and a GMRS one. Until now a
        // username had to be an amateur call sign, so it becomes that.
        // On a GMRS net a check-in has a name of its own (family members
        // share a call sign), and a net's saved stations are kept by call
        // sign and name.
        EnsureColumnExists(db_, "check_ins", "name", "TEXT NOT NULL DEFAULT ''");
        UpgradeSavedStationsTable();
        // Again after a rebuild above, whose new table has no indexes.
        {
            Statement index(&statements_,
                            "CREATE INDEX IF NOT EXISTS idx_net_saved_stations_callsign ON "
                            "net_saved_stations(callsign);");
            index.Step();
        }
        EnsureColumnExists(db_, "users", "amateur_callsign", "TEXT NOT NULL DEFAULT ''");
        EnsureColumnExists(db_, "users", "gmrs_callsign", "TEXT NOT NULL DEFAULT ''");
        EnsureColumnExists(db_, "users", "transfer_method", "INTEGER NOT NULL DEFAULT 0");
        {
            Statement copy(&statements_,
                           "UPDATE users SET amateur_callsign = upper(username) "
                           "WHERE amateur_callsign = '' AND gmrs_callsign = '';");
            copy.Step();
        }

        std::string set_version = "PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";";
        sqlite3_exec(db_, set_version.c_str(), nullptr, nullptr, nullptr);
    }

    void Database::UpgradeUsersTable()
    {
        {
            Statement columns(&statements_, "PRAGMA table_info(users);");
            while (columns.Step())
            {
                if (columns.ColumnText(1) == "id")
                {
                    return;
                }
            }
        }
        // The same table as kSchemaSql's, which a database this old already
        // has in its earlier shape, so CREATE TABLE IF NOT EXISTS skipped it.
        const char* upgrade_sql = R"sql(
BEGIN;
ALTER TABLE users RENAME TO users_before_keys;
CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL,
    public_key TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT 0,
    last_login_at INTEGER NOT NULL DEFAULT 0
);
INSERT INTO users (username, public_key, created_at, last_login_at)
SELECT username, public_key, created_at, last_login_at FROM users_before_keys;
DROP TABLE users_before_keys;
COMMIT;
)sql";
        char* error_message = nullptr;
        if (sqlite3_exec(db_, upgrade_sql, nullptr, nullptr, &error_message) != SQLITE_OK)
        {
            std::string message = error_message != nullptr ? error_message : "unknown error";
            sqlite3_free(error_message);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            throw std::runtime_error("Failed to upgrade the SSH users table: " + message);
        }
    }

    void Database::UpgradeSavedStationsTable()
    {
        {
            Statement columns(&statements_, "PRAGMA table_info(net_saved_stations);");
            while (columns.Step())
            {
                if (columns.ColumnText(1) == "name")
                {
                    return;
                }
            }
        }
        // A primary key can't be changed in place: rebuilt as kSchemaSql has
        // it, every row kept (with no name of its own).
        WriteTransaction transaction(this);
        Statement rename(&statements_, "ALTER TABLE net_saved_stations RENAME TO net_saved_stations_before_names;");
        rename.Step();
        Statement create(&statements_, R"sql(
        CREATE TABLE net_saved_stations (
            net_id INTEGER NOT NULL REFERENCES nets(id),
            callsign TEXT NOT NULL REFERENCES stations(callsign),
            default_remarks TEXT NOT NULL DEFAULT '',
            name TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
            PRIMARY KEY (net_id, callsign, name)
        );
    )sql");
        create.Step();
        Statement copy(&statements_, R"sql(
        INSERT INTO net_saved_stations (net_id, callsign, default_remarks)
        SELECT net_id, callsign, default_remarks FROM net_saved_stations_before_names;
    )sql");
        copy.Step();
        Statement drop(&statements_, "DROP TABLE net_saved_stations_before_names;");
        drop.Step();
        transaction.Commit();
    }

    void Database::DropOldSeedStations()
    {
        {
            Statement exists(&statements_,
                             "SELECT 1 FROM sqlite_schema WHERE type = 'table' AND "
                             "name = 'net_seed_stations';");
            if (!exists.Step())
            {
                return;
            }
        }
        // Read through a pragma, as the table may predate default_remarks.
        bool has_remarks = false;
        {
            Statement columns(&statements_, "PRAGMA table_info(net_seed_stations);");
            while (columns.Step())
            {
                has_remarks = has_remarks || columns.ColumnText(1) == "default_remarks";
            }
        }
        std::string copy_sql = std::string(
                                   "INSERT OR IGNORE INTO net_saved_stations (net_id, callsign, "
                                   "default_remarks) SELECT s.net_id, s.callsign, ") +
                               (has_remarks ? "s.default_remarks" : "''") +
                               " FROM net_seed_stations s JOIN nets n ON n.id = s.net_id "
                               "JOIN stations t ON t.callsign = s.callsign;";
        WriteTransaction transaction(this);
        Statement copy(db_, copy_sql);
        copy.Step();
        Statement drop(&statements_, "DROP TABLE net_seed_stations;");
        drop.Step();
        transaction.Commit();
    }

    void Database::NormalizeNetModes()
    {
        std::vector<std::pair<std::int64_t, std::string>> fixes;
        {
            Statement select(&statements_, "SELECT id, mode FROM nets;");
            while (select.Step())
            {
                std::string mode = select.ColumnText(1);
                std::string normalized = NormalizeMode(mode);
                if (normalized != mode)
                {
                    fixes.emplace_back(select.ColumnInt64(0), normalized);
                }
            }
        }
        if (fixes.empty())
        {
            return;
        }
        WriteTransaction transaction(this);
        Statement update(&statements_, "UPDATE nets SET mode = ? WHERE id = ?;");
        for (const std::pair<std::int64_t, std::string>& fix : fixes)
        {
            update.BindText(0, fix.second);
            update.BindInt64(1, fix.first);
            update.Step();
            update.Reset();
        }
        transaction.Commit();
    }

    void Database::NormalizeNetZips()
    {
        std::vector<std::pair<std::int64_t, std::string>> fixes;
        {
            Statement select(&statements_, "SELECT id, default_location FROM nets;");
            while (select.Step())
            {
                std::string location = select.ColumnText(1);
                std::string zip = ExtractZipCode(location);
                if (zip != location)
                {
                    fixes.emplace_back(select.ColumnInt64(0), zip);
                }
            }
        }
        if (fixes.empty())
        {
            return;
        }
        WriteTransaction transaction(this);
        Statement update(&statements_, "UPDATE nets SET default_location = ? WHERE id = ?;");
        for (const std::pair<std::int64_t, std::string>& fix : fixes)
        {
            update.BindText(0, fix.second);
            update.BindInt64(1, fix.first);
            update.Step();
            update.Reset();
        }
        transaction.Commit();
    }

    void Database::NormalizeNetFrequencies()
    {
        struct Fix
        {
            std::int64_t id;
            std::string frequency;
            std::string comments;
        };
        std::vector<Fix> fixes;
        {
            // Amateur nets only: a GMRS net's frequency is one of its
            // channels, never free text.
            Statement select(&statements_, "SELECT id, default_frequency, notes FROM nets WHERE service != 'gmrs';");
            while (select.Step())
            {
                Fix fix{select.ColumnInt64(0), select.ColumnText(1), select.ColumnText(2)};
                if (MoveBadFrequencyToComments(&fix.frequency, &fix.comments))
                {
                    fixes.push_back(fix);
                }
            }
        }
        if (fixes.empty())
        {
            return;
        }
        WriteTransaction transaction(this);
        Statement update(&statements_, "UPDATE nets SET default_frequency = ?, notes = ? WHERE id = ?;");
        for (const Fix& fix : fixes)
        {
            update.BindText(0, fix.frequency);
            update.BindText(1, fix.comments);
            update.BindInt64(2, fix.id);
            update.Step();
            update.Reset();
        }
        transaction.Commit();
    }

    void Database::UpdateStationGrid(const std::string& callsign, const std::string& grid)
    {
        if (grid.size() != 6)
        {
            return;
        }
        Statement statement(&statements_, R"sql(
        UPDATE stations SET grid_square = ?
        WHERE callsign = ? AND (grid_square = '' OR
                                (length(grid_square) = 4 AND upper(grid_square) = upper(substr(?, 1, 4))));
    )sql");
        statement.BindText(0, grid);
        statement.BindText(1, callsign);
        statement.BindText(2, grid);
        statement.Step();
    }

    void Database::UpsertStation(const Station& station)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO stations
            (callsign, name, member_id, street_address, city, county, state, zip,
             grid_square, license_class, email, data_source, last_updated)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(callsign) DO UPDATE SET
            name = excluded.name,
            member_id = excluded.member_id,
            street_address = excluded.street_address,
            city = excluded.city,
            county = excluded.county,
            state = excluded.state,
            zip = excluded.zip,
            grid_square = excluded.grid_square,
            license_class = excluded.license_class,
            email = excluded.email,
            data_source = excluded.data_source,
            last_updated = excluded.last_updated;
    )sql");
        statement.BindText(0, ToUpperAscii(station.callsign));
        statement.BindText(1, station.name);
        statement.BindText(2, station.member_id);
        statement.BindText(3, station.street_address);
        statement.BindText(4, station.city);
        statement.BindText(5, station.county);
        statement.BindText(6, station.state);
        statement.BindText(7, station.zip);
        statement.BindText(8, station.grid_square);
        statement.BindText(9, station.license_class);
        statement.BindText(10, station.email);
        statement.BindInt64(11, static_cast<std::int64_t>(station.data_source));
        statement.BindInt64(12, station.last_updated);
        statement.Step();
    }

    void Database::RecordManualCheckInStation(const Station& station, std::int64_t updated_at)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO stations
            (callsign, name, member_id, street_address, city, county, state, zip,
             grid_square, last_updated)
        VALUES (?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(callsign) DO UPDATE SET
            name = CASE WHEN excluded.name != '' THEN excluded.name ELSE stations.name END,
            member_id = CASE WHEN excluded.member_id != '' THEN excluded.member_id
                        ELSE stations.member_id END,
            street_address = CASE WHEN excluded.street_address != '' THEN excluded.street_address
                              ELSE stations.street_address END,
            city = CASE WHEN excluded.city != '' THEN excluded.city ELSE stations.city END,
            county = CASE WHEN excluded.county != '' THEN excluded.county ELSE stations.county END,
            state = CASE WHEN excluded.state != '' THEN excluded.state ELSE stations.state END,
            zip = CASE WHEN excluded.zip != '' THEN excluded.zip ELSE stations.zip END,
            grid_square = CASE WHEN excluded.grid_square != '' THEN excluded.grid_square
                          ELSE stations.grid_square END,
            last_updated = excluded.last_updated;
    )sql");
        statement.BindText(0, ToUpperAscii(station.callsign));
        statement.BindText(1, station.name);
        statement.BindText(2, station.member_id);
        statement.BindText(3, station.street_address);
        statement.BindText(4, station.city);
        statement.BindText(5, station.county);
        statement.BindText(6, station.state);
        statement.BindText(7, station.zip);
        statement.BindText(8, station.grid_square);
        statement.BindInt64(9, updated_at);
        statement.Step();
    }

    void Database::FillStationBlanks(const Station& station, std::int64_t updated_at)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO stations
            (callsign, name, member_id, street_address, city, county, state, zip,
             grid_square, last_updated)
        VALUES (?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(callsign) DO UPDATE SET
            name = CASE WHEN stations.name = '' THEN excluded.name ELSE stations.name END,
            member_id = CASE WHEN stations.member_id = '' THEN excluded.member_id
                        ELSE stations.member_id END,
            street_address = CASE WHEN stations.street_address = '' THEN excluded.street_address
                             ELSE stations.street_address END,
            city = CASE WHEN stations.city = '' THEN excluded.city ELSE stations.city END,
            county = CASE WHEN stations.county = '' THEN excluded.county ELSE stations.county END,
            state = CASE WHEN stations.state = '' THEN excluded.state ELSE stations.state END,
            zip = CASE WHEN stations.zip = '' THEN excluded.zip ELSE stations.zip END,
            grid_square = CASE WHEN stations.grid_square = '' THEN excluded.grid_square
                          ELSE stations.grid_square END;
    )sql");
        statement.BindText(0, ToUpperAscii(station.callsign));
        statement.BindText(1, station.name);
        statement.BindText(2, station.member_id);
        statement.BindText(3, station.street_address);
        statement.BindText(4, station.city);
        statement.BindText(5, station.county);
        statement.BindText(6, station.state);
        statement.BindText(7, station.zip);
        statement.BindText(8, station.grid_square);
        statement.BindInt64(9, updated_at);
        statement.Step();
    }

    bool Database::AddNetSavedStationIfMissing(std::int64_t net_id, const std::string& callsign,
                                               const std::string& default_remarks, const std::string& name)
    {
        Statement statement(&statements_, R"sql(
        INSERT OR IGNORE INTO net_saved_stations (net_id, callsign, default_remarks, name)
        VALUES (?, ?, ?, ?);
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(callsign));
        statement.BindText(2, default_remarks);
        statement.BindText(3, name);
        statement.Step();
        return sqlite3_changes(db_) > 0;
    }

    void Database::UpdateStationFields(const Station& station, std::int64_t updated_at)
    {
        Statement statement(&statements_, R"sql(
        UPDATE stations
        SET name = ?, member_id = ?, street_address = ?, city = ?, county = ?, state = ?,
            zip = ?, grid_square = ?, last_updated = ?
        WHERE callsign = ?;
    )sql");
        statement.BindText(0, station.name);
        statement.BindText(1, station.member_id);
        statement.BindText(2, station.street_address);
        statement.BindText(3, station.city);
        statement.BindText(4, station.county);
        statement.BindText(5, station.state);
        statement.BindText(6, station.zip);
        statement.BindText(7, station.grid_square);
        statement.BindInt64(8, updated_at);
        statement.BindText(9, ToUpperAscii(station.callsign));
        statement.Step();
    }

    std::optional<Station> Database::FindStationByCallsign(const std::string& callsign)
    {
        Statement statement(&statements_, R"sql(
        SELECT callsign, name, member_id, street_address, city, county, state, zip,
               grid_square, license_class, email, data_source, last_updated
        FROM stations WHERE callsign = ?;
    )sql");
        statement.BindText(0, ToUpperAscii(callsign));
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return ReadStationRow(statement);
    }

    std::vector<Station> Database::FindStationsByCallsigns(const std::vector<std::string>& callsigns) const
    {
        // In batches, under SQLite's oldest limit on parameters (999).
        static constexpr std::size_t kBatch = 500;
        std::vector<Station> stations;
        stations.reserve(callsigns.size());
        std::string sql;
        for (std::size_t begin = 0; begin < callsigns.size(); begin += kBatch)
        {
            std::size_t end = std::min(callsigns.size(), begin + kBatch);
            sql.assign(
                "SELECT callsign, name, member_id, street_address, city, county, state, zip, "
                "grid_square, license_class, email, data_source, last_updated "
                "FROM stations WHERE callsign IN (");
            for (std::size_t i = begin; i < end; ++i)
            {
                sql.append(i == begin ? "?" : ",?");
            }
            sql.append(");");
            Statement statement(db_, sql);
            for (std::size_t i = begin; i < end; ++i)
            {
                statement.BindText(static_cast<int>(i - begin), callsigns[i]);
            }
            while (statement.Step())
            {
                stations.push_back(ReadStationRow(statement));
            }
        }
        std::sort(stations.begin(), stations.end(), StationsByCallsign());
        return stations;
    }

    std::vector<Station> Database::GetStationsInNetInstance(std::int64_t instance_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT callsign, name, member_id, street_address, city, county, state, zip,
               grid_square, license_class, email, data_source, last_updated
        FROM stations
        WHERE callsign IN (SELECT callsign FROM check_ins WHERE net_instance_id = ?)
        ORDER BY callsign;
    )sql");
        statement.BindInt64(0, instance_id);
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadStationRow(statement));
        }
        return stations;
    }

    std::vector<Station> Database::SearchStationsByCallsignSubstring(const std::string& substring, int limit)
    {
        // With a "?" typed: the characters in that order (see IsWildcardCallsign).
        bool wildcard = IsWildcardCallsign(substring);
        if (wildcard && !WildcardHasEnough(substring))
        {
            return {};
        }
        Statement statement(&statements_, wildcard ? R"sql(
        SELECT callsign, name, member_id, street_address, city, county, state, zip,
               grid_square, license_class, email, data_source, last_updated
        FROM stations WHERE callsign LIKE ? ORDER BY callsign LIMIT ?;
    )sql"
                                                   : R"sql(
        SELECT callsign, name, member_id, street_address, city, county, state, zip,
               grid_square, license_class, email, data_source, last_updated
        FROM stations WHERE instr(callsign, ?) > 0 ORDER BY callsign LIMIT ?;
    )sql");
        statement.BindText(0, wildcard ? WildcardLikePattern(substring, false) : ToUpperAscii(substring));
        statement.BindInt64(1, limit);
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadStationRow(statement));
        }
        return stations;
    }

    std::vector<Station> Database::SearchNetStationsByCallsignSubstring(std::int64_t net_id,
                                                                        const std::string& substring, int limit)
    {
        // With a "?" typed: the characters in that order (see IsWildcardCallsign).
        bool wildcard = IsWildcardCallsign(substring);
        if (wildcard && !WildcardHasEnough(substring))
        {
            return {};
        }
        Statement statement(&statements_, wildcard ? R"sql(
        SELECT s.callsign, CASE WHEN e.name != '' THEN e.name ELSE s.name END, s.member_id, s.street_address,
               s.city, s.county, s.state, s.zip, s.grid_square, s.license_class, s.email, s.data_source,
               s.last_updated
        FROM stations s
        JOIN (SELECT ns.callsign, ns.name FROM net_saved_stations ns WHERE ns.net_id = ?2 AND ns.callsign LIKE ?1
              UNION
              SELECT c.callsign, c.name FROM check_ins c
              JOIN net_instances ni ON ni.id = c.net_instance_id
              WHERE ni.net_id = ?2 AND c.callsign LIKE ?1) e
          ON e.callsign = s.callsign
        ORDER BY s.callsign, e.name
        LIMIT ?3;
    )sql"
                                                   : R"sql(
        SELECT s.callsign, CASE WHEN e.name != '' THEN e.name ELSE s.name END, s.member_id, s.street_address,
               s.city, s.county, s.state, s.zip, s.grid_square, s.license_class, s.email, s.data_source,
               s.last_updated
        FROM stations s
        JOIN (SELECT ns.callsign, ns.name FROM net_saved_stations ns WHERE ns.net_id = ?2 AND instr(ns.callsign, ?1) > 0
              UNION
              SELECT c.callsign, c.name FROM check_ins c
              JOIN net_instances ni ON ni.id = c.net_instance_id
              WHERE ni.net_id = ?2 AND instr(c.callsign, ?1) > 0) e
          ON e.callsign = s.callsign
        ORDER BY s.callsign, e.name
        LIMIT ?3;
    )sql");
        statement.BindText(0, wildcard ? WildcardLikePattern(substring, false) : ToUpperAscii(substring));
        statement.BindInt64(1, net_id);
        statement.BindInt64(2, limit);
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadStationRow(statement));
        }
        return stations;
    }

    void Database::SaveNetStation(std::int64_t net_id, const Station& station, const std::string& default_remarks,
                                  std::int64_t updated_at, const std::string& name)
    {
        WriteTransaction transaction(this);
        // A named entry (a GMRS family member) leaves the station's name, the
        // licensee's, as it was, if it had one.
        Station shared = station;
        if (!name.empty())
        {
            std::optional<Station> before = FindStationByCallsign(station.callsign);
            if (before.has_value() && !before->name.empty())
            {
                shared.name = before->name;
            }
        }
        RecordManualCheckInStation(shared, updated_at);

        Statement statement(&statements_, R"sql(
        INSERT INTO net_saved_stations (net_id, callsign, default_remarks, name)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(net_id, callsign, name) DO UPDATE SET default_remarks = excluded.default_remarks;
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(station.callsign));
        statement.BindText(2, default_remarks);
        statement.BindText(3, name);
        statement.Step();
        transaction.Commit();
    }

    void Database::UpdateSavedNetStation(std::int64_t net_id, const Station& station,
                                         const std::string& default_remarks, std::int64_t updated_at,
                                         const std::string& old_name, const std::string& new_name)
    {
        WriteTransaction transaction(this);
        // A named entry (a GMRS family member) keeps its own name; the
        // station's is the licensee's.
        if (old_name.empty() && new_name.empty())
        {
            UpdateStationFields(station, updated_at);
        }
        else
        {
            std::optional<Station> before = FindStationByCallsign(station.callsign);
            Station shared = station;
            shared.name = before.has_value() ? before->name : std::string();
            UpdateStationFields(shared, updated_at);
        }

        Statement statement(&statements_, R"sql(
        UPDATE net_saved_stations SET default_remarks = ?, name = ? WHERE net_id = ? AND callsign = ? AND name = ?;
    )sql");
        statement.BindText(0, default_remarks);
        statement.BindText(1, new_name);
        statement.BindInt64(2, net_id);
        statement.BindText(3, ToUpperAscii(station.callsign));
        statement.BindText(4, old_name);
        statement.Step();
        transaction.Commit();
    }

    void Database::RemoveSavedNetStation(std::int64_t net_id, const std::string& callsign, const std::string& name)
    {
        WriteTransaction transaction(this);
        Statement statement(&statements_, R"sql(
        DELETE FROM net_saved_stations WHERE net_id = ? AND callsign = ? AND name = ?;
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(callsign));
        statement.BindText(2, name);
        statement.Step();
        DeleteUnusedStations();
        transaction.Commit();
    }

    void Database::RenameNetSavedStationEntry(std::int64_t net_id, const std::string& callsign,
                                              const std::string& old_name, const std::string& new_name)
    {
        WriteTransaction transaction(this);
        std::string upper = ToUpperAscii(callsign);
        bool in_use = false;
        {
            Statement used(&statements_, R"sql(
            SELECT EXISTS(SELECT 1 FROM check_ins c JOIN net_instances i ON i.id = c.net_instance_id
                          WHERE i.net_id = ?1 AND c.callsign = ?2 AND c.name = ?3 COLLATE NOCASE);
        )sql");
            used.BindInt64(0, net_id);
            used.BindText(1, upper);
            used.BindText(2, old_name);
            used.Step();
            in_use = used.ColumnInt64(0) != 0;
        }
        if (in_use)
        {
            Statement copy(&statements_, R"sql(
            INSERT OR IGNORE INTO net_saved_stations (net_id, callsign, default_remarks, name)
            SELECT net_id, callsign, default_remarks, ?4 FROM net_saved_stations
            WHERE net_id = ?1 AND callsign = ?2 AND name = ?3;
        )sql");
            copy.BindInt64(0, net_id);
            copy.BindText(1, upper);
            copy.BindText(2, old_name);
            copy.BindText(3, new_name);
            copy.Step();
            transaction.Commit();
            return;
        }
        {
            Statement rename(&statements_, R"sql(
            UPDATE OR IGNORE net_saved_stations SET name = ?4 WHERE net_id = ?1 AND callsign = ?2 AND name = ?3;
        )sql");
            rename.BindInt64(0, net_id);
            rename.BindText(1, upper);
            rename.BindText(2, old_name);
            rename.BindText(3, new_name);
            rename.Step();
            if (sqlite3_changes(db_) > 0)
            {
                transaction.Commit();
                return;
            }
        }
        // Already saved under the new name too.
        RemoveSavedNetStation(net_id, callsign, old_name);
        transaction.Commit();
    }

    bool Database::IsStationUsedOutsideNet(const std::string& callsign, std::int64_t net_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT EXISTS(SELECT 1 FROM net_saved_stations WHERE callsign = ?1 AND net_id != ?2)
            OR EXISTS(SELECT 1 FROM check_ins WHERE callsign = ?1);
    )sql");
        statement.BindText(0, ToUpperAscii(callsign));
        statement.BindInt64(1, net_id);
        statement.Step();
        return statement.ColumnInt64(0) != 0;
    }

    int Database::DeleteUnusedStations()
    {
        Statement statement(&statements_, R"sql(
        DELETE FROM stations
        WHERE NOT EXISTS (SELECT 1 FROM net_saved_stations s WHERE s.callsign = stations.callsign)
          AND NOT EXISTS (SELECT 1 FROM check_ins c WHERE c.callsign = stations.callsign);
    )sql");
        statement.Step();
        return sqlite3_changes(db_);
    }

    std::vector<Station> Database::GetSavedStationsForNet(std::int64_t net_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT s.callsign, CASE WHEN ns.name != '' THEN ns.name ELSE s.name END, s.member_id, s.street_address,
               s.city, s.county, s.state, s.zip, s.grid_square, s.license_class, s.email, s.data_source,
               s.last_updated
        FROM stations s
        JOIN net_saved_stations ns ON ns.callsign = s.callsign
        WHERE ns.net_id = ?
        ORDER BY s.callsign, ns.name;
    )sql");
        statement.BindInt64(0, net_id);
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadStationRow(statement));
        }
        return stations;
    }

    std::vector<SavedNetStation> Database::GetSavedNetEntries(std::int64_t net_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT s.callsign, s.name, s.member_id, s.street_address, s.city, s.county, s.state, s.zip, s.grid_square,
               s.license_class, s.email, s.data_source, s.last_updated, ns.name, ns.default_remarks
        FROM stations s
        JOIN net_saved_stations ns ON ns.callsign = s.callsign
        WHERE ns.net_id = ?
        ORDER BY s.callsign, ns.name;
    )sql");
        statement.BindInt64(0, net_id);
        std::vector<SavedNetStation> entries;
        while (statement.Step())
        {
            SavedNetStation entry;
            entry.station = ReadStationRow(statement);
            entry.name = statement.ColumnText(13);
            entry.default_remarks = statement.ColumnText(14);
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    std::string Database::GetSavedNetStationRemarks(std::int64_t net_id, const std::string& callsign,
                                                    const std::string& name)
    {
        Statement statement(&statements_, R"sql(
        SELECT default_remarks FROM net_saved_stations WHERE net_id = ? AND callsign = ? AND name = ?;
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(callsign));
        statement.BindText(2, name);
        if (!statement.Step())
        {
            return "";
        }
        return statement.ColumnText(0);
    }

    std::int64_t Database::CreateNet(const Net& net)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO nets
            (name, mode, default_frequency, default_location,
             default_grid_square, recurrence_description, notes, created_at, imported_at,
             is_ad_hoc, repeater_offset, pl_tone, partial_match_canada, service)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?);
    )sql");
        statement.BindText(0, net.name);
        statement.BindText(1, net.mode);
        statement.BindText(2, net.default_frequency);
        statement.BindText(3, net.default_location);
        statement.BindText(4, net.default_grid_square);
        statement.BindText(5, net.recurrence_description);
        statement.BindText(6, net.comments);
        statement.BindInt64(7, net.created_at);
        statement.BindInt64(8, net.imported_at);
        statement.BindInt64(9, net.is_ad_hoc ? 1 : 0);
        statement.BindText(10, net.repeater_offset);
        statement.BindText(11, net.pl_tone);
        statement.BindInt64(12, net.partial_match_canada ? 1 : 0);
        statement.BindText(13, net.service == NetService::kGmrs ? "gmrs" : "amateur");
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    void Database::UpdateNet(const Net& net)
    {
        Statement statement(&statements_, R"sql(
        UPDATE nets
        SET name = ?, mode = ?, default_frequency = ?, default_location = ?,
            recurrence_description = ?, notes = ?, repeater_offset = ?, pl_tone = ?,
            partial_match_canada = ?
        WHERE id = ?;
    )sql");
        statement.BindText(0, net.name);
        statement.BindText(1, net.mode);
        statement.BindText(2, net.default_frequency);
        statement.BindText(3, net.default_location);
        statement.BindText(4, net.recurrence_description);
        statement.BindText(5, net.comments);
        statement.BindText(6, net.repeater_offset);
        statement.BindText(7, net.pl_tone);
        statement.BindInt64(8, net.partial_match_canada ? 1 : 0);
        statement.BindInt64(9, net.id);
        statement.Step();
    }

    std::vector<Net> Database::GetAllNets()
    {
        Statement statement(&statements_, R"sql(
        SELECT id, name, mode, default_frequency, default_location,
               default_grid_square, recurrence_description, notes, created_at, imported_at,
               is_ad_hoc, repeater_offset, pl_tone, partial_match_canada, service
        FROM nets ORDER BY name COLLATE NOCASE, id;
    )sql");
        std::vector<Net> nets;
        while (statement.Step())
        {
            nets.push_back(ReadNetRow(statement));
        }
        return nets;
    }

    std::optional<Net> Database::GetNetById(std::int64_t net_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT id, name, mode, default_frequency, default_location,
               default_grid_square, recurrence_description, notes, created_at, imported_at,
               is_ad_hoc, repeater_offset, pl_tone, partial_match_canada, service
        FROM nets WHERE id = ?;
    )sql");
        statement.BindInt64(0, net_id);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return ReadNetRow(statement);
    }

    void Database::DeleteNetCompletely(std::int64_t net_id)
    {
        WriteTransaction transaction(this);

        Statement delete_check_ins(&statements_, R"sql(
        DELETE FROM check_ins
        WHERE net_instance_id IN (SELECT id FROM net_instances WHERE net_id = ?);
    )sql");
        delete_check_ins.BindInt64(0, net_id);
        delete_check_ins.Step();

        Statement delete_instances(&statements_, "DELETE FROM net_instances WHERE net_id = ?;");
        delete_instances.BindInt64(0, net_id);
        delete_instances.Step();

        Statement delete_saved(&statements_, "DELETE FROM net_saved_stations WHERE net_id = ?;");
        delete_saved.BindInt64(0, net_id);
        delete_saved.Step();

        Statement delete_net(&statements_, "DELETE FROM nets WHERE id = ?;");
        delete_net.BindInt64(0, net_id);
        delete_net.Step();

        DeleteUnusedStations();
        transaction.Commit();
    }

    std::int64_t Database::CreateNetInstance(const NetInstance& instance)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO net_instances
            (net_id, instance_date, net_control_callsign,
             alternate_net_control_callsign, logger_callsign, created_by,
             frequency, location, status, closed_at, operator_role, started_at, notes)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);
    )sql");
        statement.BindInt64(0, instance.net_id);
        statement.BindText(1, instance.instance_date);
        statement.BindText(2, ToUpperAscii(instance.net_control_callsign));
        statement.BindText(3, ToUpperAscii(instance.alternate_net_control_callsign));
        statement.BindText(4, ToUpperAscii(instance.logger_callsign));
        statement.BindText(5, ToUpperAscii(instance.created_by));
        statement.BindText(6, instance.frequency);
        statement.BindText(7, instance.location);
        statement.BindInt64(8, static_cast<std::int64_t>(instance.status));
        statement.BindInt64(9, instance.closed_at);
        statement.BindInt64(10, instance.operator_role);
        statement.BindInt64(11, instance.started_at);
        statement.BindText(12, instance.notes);
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    std::vector<NetInstance> Database::GetAdHocNetInstances()
    {
        Statement statement(&statements_, R"sql(
        SELECT i.id, i.net_id, i.instance_date, i.net_control_callsign,
               i.alternate_net_control_callsign, i.logger_callsign, i.created_by,
               i.frequency, i.location, i.status, i.closed_at, i.operator_role, i.started_at,
               i.notes, i.pushed_at
        FROM net_instances i JOIN nets n ON n.id = i.net_id
        WHERE n.is_ad_hoc = 1
        ORDER BY i.instance_date DESC, i.started_at DESC, i.id DESC;
    )sql");
        std::vector<NetInstance> instances;
        while (statement.Step())
        {
            instances.push_back(ReadNetInstanceRow(statement));
        }
        return instances;
    }

    bool Database::HasNetInstance(std::int64_t net_id, const std::string& instance_date, std::int64_t started_at)
    {
        Statement statement(&statements_, R"sql(
        SELECT EXISTS(SELECT 1 FROM net_instances WHERE net_id = ? AND instance_date = ? AND started_at = ?);
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindText(1, instance_date);
        statement.BindInt64(2, started_at);
        return statement.Step() && statement.ColumnInt64(0) != 0;
    }

    std::optional<NetInstance> Database::FindAdHocSession(const std::string& net_name, const std::string& instance_date,
                                                          std::int64_t started_at)
    {
        Statement statement(&statements_, R"sql(
        SELECT i.id, i.net_id, i.instance_date, i.net_control_callsign,
               i.alternate_net_control_callsign, i.logger_callsign, i.created_by,
               i.frequency, i.location, i.status, i.closed_at, i.operator_role, i.started_at,
               i.notes, i.pushed_at
        FROM net_instances i JOIN nets n ON n.id = i.net_id
        WHERE n.is_ad_hoc = 1 AND n.name = ? AND i.instance_date = ? AND i.started_at = ?
        ORDER BY i.id LIMIT 1;
    )sql");
        statement.BindText(0, net_name);
        statement.BindText(1, instance_date);
        statement.BindInt64(2, started_at);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return ReadNetInstanceRow(statement);
    }

    std::vector<NetInstance> Database::GetNetInstancesForNet(std::int64_t net_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT id, net_id, instance_date, net_control_callsign,
               alternate_net_control_callsign, logger_callsign, created_by,
               frequency, location, status, closed_at, operator_role, started_at, notes, pushed_at
        FROM net_instances WHERE net_id = ?
        ORDER BY instance_date DESC, started_at DESC, id DESC;
    )sql");
        statement.BindInt64(0, net_id);
        std::vector<NetInstance> instances;
        while (statement.Step())
        {
            instances.push_back(ReadNetInstanceRow(statement));
        }
        return instances;
    }

    std::optional<NetInstance> Database::GetNetInstanceById(std::int64_t instance_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT id, net_id, instance_date, net_control_callsign,
               alternate_net_control_callsign, logger_callsign, created_by,
               frequency, location, status, closed_at, operator_role, started_at, notes, pushed_at
        FROM net_instances WHERE id = ?;
    )sql");
        statement.BindInt64(0, instance_id);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return ReadNetInstanceRow(statement);
    }

    void Database::SetNetInstancePushedAt(std::int64_t instance_id, std::int64_t pushed_at)
    {
        Statement statement(&statements_, "UPDATE net_instances SET pushed_at = ? WHERE id = ?;");
        statement.BindInt64(0, pushed_at);
        statement.BindInt64(1, instance_id);
        statement.Step();
    }

    std::vector<std::int64_t> Database::GetNetIdsWithOpenInstances()
    {
        Statement statement(&statements_,
                            "SELECT DISTINCT net_id FROM net_instances WHERE status = ? ORDER BY net_id;");
        statement.BindInt64(0, static_cast<std::int64_t>(NetInstanceStatus::kOpen));
        std::vector<std::int64_t> net_ids;
        while (statement.Step())
        {
            net_ids.push_back(statement.ColumnInt64(0));
        }
        return net_ids;
    }

    std::optional<std::int64_t> Database::GetNetLastStartedBy(const std::string& callsign)
    {
        Statement statement(&statements_, R"sql(
        SELECT i.net_id
        FROM net_instances i JOIN nets n ON n.id = i.net_id
        WHERE n.is_ad_hoc = 0 AND i.created_by = ?
        ORDER BY i.instance_date DESC, i.started_at DESC, i.id DESC
        LIMIT 1;
    )sql");
        statement.BindText(0, ToUpperAscii(callsign));
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return statement.ColumnInt64(0);
    }

    bool Database::CloseNetInstance(std::int64_t instance_id, std::int64_t closed_at)
    {
        Statement statement(&statements_, R"sql(
        UPDATE net_instances SET status = ?, closed_at = ? WHERE id = ? AND status = ?;
    )sql");
        statement.BindInt64(0, static_cast<std::int64_t>(NetInstanceStatus::kClosed));
        statement.BindInt64(1, closed_at);
        statement.BindInt64(2, instance_id);
        statement.BindInt64(3, static_cast<std::int64_t>(NetInstanceStatus::kOpen));
        statement.Step();
        if (sqlite3_changes(db_) == 0)
        {
            return false;
        }
        RenumberCheckIns(instance_id);
        return true;
    }

    void Database::RenumberCheckIns(std::int64_t instance_id)
    {
        // One statement, so all or none of it, inside a caller's
        // transaction or not.
        Statement update(&statements_, R"sql(
        UPDATE check_ins
        SET sequence_number = (SELECT n FROM (SELECT id, ROW_NUMBER() OVER (ORDER BY sequence_number, id) AS n
                                              FROM check_ins WHERE net_instance_id = ?1) AS numbered
                               WHERE numbered.id = check_ins.id)
        WHERE net_instance_id = ?1;
    )sql");
        update.BindInt64(0, instance_id);
        update.Step();
    }

    void Database::DeleteNetInstance(std::int64_t instance_id, bool remove_unused_stations)
    {
        WriteTransaction transaction(this);

        Statement delete_check_ins(&statements_, "DELETE FROM check_ins WHERE net_instance_id = ?;");
        delete_check_ins.BindInt64(0, instance_id);
        delete_check_ins.Step();

        Statement delete_instance(&statements_, "DELETE FROM net_instances WHERE id = ?;");
        delete_instance.BindInt64(0, instance_id);
        delete_instance.Step();

        if (remove_unused_stations)
        {
            DeleteUnusedStations();
        }
        transaction.Commit();
    }

    void Database::SetNetInstanceRoleCallsign(std::int64_t instance_id, int role, const std::string& callsign)
    {
        const char* column = nullptr;
        switch (role)
        {
            case kRoleNetControl:
                column = "net_control_callsign";
                break;
            case kRoleAlternateNetControl:
                column = "alternate_net_control_callsign";
                break;
            case kRoleLogger:
                column = "logger_callsign";
                break;
            default:
                return;
        }

        std::string sql = std::string("UPDATE net_instances SET ") + column + " = ? WHERE id = ?;";
        Statement statement(db_, sql);
        statement.BindText(0, ToUpperAscii(callsign));
        statement.BindInt64(1, instance_id);
        statement.Step();
    }

    void Database::SetNetInstanceNotes(std::int64_t instance_id, const std::string& notes)
    {
        Statement statement(&statements_, "UPDATE net_instances SET notes = ? WHERE id = ?;");
        statement.BindText(0, notes);
        statement.BindInt64(1, instance_id);
        statement.Step();
    }

    std::int64_t Database::AddCheckIn(const CheckIn& check_in)
    {
        return AddCheckIn(check_in, check_in.net_instance_id);
    }

    std::int64_t Database::AddCheckIn(const CheckIn& check_in, std::int64_t net_instance_id)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO check_ins
            (net_instance_id, callsign, sequence_number, signal_report,
             remarks, comment, checked_in_at, designated_role, name)
        VALUES (?,?,?,?,?,?,?,?,?);
    )sql");
        statement.BindInt64(0, net_instance_id);
        statement.BindText(1, ToUpperAscii(check_in.callsign));
        statement.BindInt64(2, check_in.sequence_number);
        statement.BindText(3, check_in.signal_report);
        statement.BindText(4, check_in.remarks);
        statement.BindText(5, check_in.comment);
        statement.BindInt64(6, check_in.checked_in_at);
        statement.BindInt64(7, check_in.designated_role);
        statement.BindText(8, check_in.name);
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    std::int64_t Database::AddCheckInAtNextSequence(const CheckIn& check_in)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO check_ins
            (net_instance_id, callsign, sequence_number, signal_report,
             remarks, comment, checked_in_at, designated_role, name)
        VALUES (?1, ?2,
                (SELECT COALESCE(MAX(sequence_number), 0) + 1 FROM check_ins
                 WHERE net_instance_id = ?1),
                ?3, ?4, ?5, ?6, ?7, ?8);
    )sql");
        statement.BindInt64(0, check_in.net_instance_id);
        statement.BindText(1, ToUpperAscii(check_in.callsign));
        statement.BindText(2, check_in.signal_report);
        statement.BindText(3, check_in.remarks);
        statement.BindText(4, check_in.comment);
        statement.BindInt64(5, check_in.checked_in_at);
        statement.BindInt64(6, check_in.designated_role);
        statement.BindText(7, check_in.name);
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    std::unordered_map<std::int64_t, std::int64_t> Database::GetCheckInCounts(std::int64_t net_id)
    {
        Statement statement(&statements_, net_id > 0 ? R"sql(
        SELECT c.net_instance_id, COUNT(*) FROM check_ins c JOIN net_instances i ON i.id = c.net_instance_id
        WHERE i.net_id = ? GROUP BY c.net_instance_id;
    )sql"
                                                     : R"sql(
        SELECT c.net_instance_id, COUNT(*) FROM check_ins c JOIN net_instances i ON i.id = c.net_instance_id
        JOIN nets n ON n.id = i.net_id
        WHERE n.is_ad_hoc = 1 GROUP BY c.net_instance_id;
    )sql");
        if (net_id > 0)
        {
            statement.BindInt64(0, net_id);
        }
        std::unordered_map<std::int64_t, std::int64_t> counts;
        while (statement.Step())
        {
            counts[statement.ColumnInt64(0)] = statement.ColumnInt64(1);
        }
        return counts;
    }

    std::vector<std::string> Database::GetCallsignsInOtherSessions(std::int64_t net_id, std::int64_t instance_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT DISTINCT c.callsign FROM check_ins c JOIN net_instances i ON i.id = c.net_instance_id
        WHERE i.net_id = ? AND i.id != ?;
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindInt64(1, instance_id);
        std::vector<std::string> callsigns;
        while (statement.Step())
        {
            callsigns.push_back(statement.ColumnText(0));
        }
        return callsigns;
    }

    std::vector<CheckIn> Database::GetCheckInsForNetInstances(const std::vector<std::int64_t>& instance_ids) const
    {
        std::vector<CheckIn> check_ins;
        if (instance_ids.empty())
        {
            return check_ins;
        }
        // Few: the sessions a window looks at, well under SQLite's limit on
        // parameters.
        std::string sql = "SELECT " QL_CHECK_IN_COLUMNS " FROM check_ins c WHERE c.net_instance_id IN (";
        for (std::size_t i = 0; i < instance_ids.size(); ++i)
        {
            sql.append(i == 0 ? "?" : ",?");
        }
        sql.append(") ORDER BY c.net_instance_id, c.sequence_number;");
        Statement statement(db_, sql);
        for (std::size_t i = 0; i < instance_ids.size(); ++i)
        {
            statement.BindInt64(static_cast<int>(i), instance_ids[i]);
        }
        while (statement.Step())
        {
            check_ins.push_back(ReadCheckInRow(statement));
        }
        return check_ins;
    }

    void Database::GetCheckInSummary(std::int64_t net_instance_id, std::int64_t* count, std::int64_t* newest_id)
    {
        Statement statement(&statements_,
                            "SELECT COUNT(*), COALESCE(MAX(id), 0) FROM check_ins WHERE net_instance_id = ?;");
        statement.BindInt64(0, net_instance_id);
        *count = 0;
        *newest_id = 0;
        if (statement.Step())
        {
            *count = statement.ColumnInt64(0);
            *newest_id = statement.ColumnInt64(1);
        }
    }

    // Rows of: net name, then QL_NET_INSTANCE_COLUMNS, then QL_CHECK_IN_COLUMNS.
    static std::vector<StationCheckInRecord> ReadStationCheckIns(Statement* statement)
    {
        std::vector<StationCheckInRecord> records;
        while (statement->Step())
        {
            StationCheckInRecord record;
            record.net_name = statement->ColumnText(0);
            record.net_is_ad_hoc = statement->ColumnInt64(1) != 0;
            record.instance = ReadNetInstanceColumns(*statement, 2);
            record.check_in = ReadCheckInColumns(*statement, 16);
            records.push_back(record);
        }
        return records;
    }

    std::vector<StationCheckInRecord> Database::GetStationCheckInsForNet(std::int64_t net_id,
                                                                         const std::string& callsign)
    {
        Statement statement(&statements_,
                            "SELECT n.name, n.is_ad_hoc, " QL_NET_INSTANCE_COLUMNS ", " QL_CHECK_IN_COLUMNS
                            " FROM check_ins c"
                            " JOIN net_instances i ON i.id = c.net_instance_id"
                            " JOIN nets n ON n.id = i.net_id"
                            " WHERE i.net_id = ? AND c.callsign = ?"
                            " ORDER BY i.instance_date DESC, i.started_at DESC, i.id DESC;");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(callsign));
        return ReadStationCheckIns(&statement);
    }

    std::vector<StationCheckInRecord> Database::FindCheckInsByCallsign(const std::string& substring, int limit)
    {
        Statement statement(&statements_,
                            "SELECT n.name, n.is_ad_hoc, " QL_NET_INSTANCE_COLUMNS ", " QL_CHECK_IN_COLUMNS
                            " FROM check_ins c"
                            " JOIN net_instances i ON i.id = c.net_instance_id"
                            " JOIN nets n ON n.id = i.net_id"
                            " WHERE instr(c.callsign, ?) > 0"
                            " ORDER BY i.instance_date DESC, i.started_at DESC, i.id DESC"
                            " LIMIT ?;");
        statement.BindText(0, ToUpperAscii(substring));
        statement.BindInt64(1, limit);
        return ReadStationCheckIns(&statement);
    }

    std::vector<CallsignTally> Database::GetTopCallsignsForNet(std::int64_t net_id, int limit)
    {
        Statement statement(&statements_, R"sql(
        SELECT c.callsign, COUNT(*), MAX(i.instance_date)
        FROM check_ins c JOIN net_instances i ON i.id = c.net_instance_id
        WHERE i.net_id = ?
        GROUP BY c.callsign
        ORDER BY COUNT(*) DESC, c.callsign
        LIMIT ?;
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindInt64(1, limit);
        std::vector<CallsignTally> tallies;
        while (statement.Step())
        {
            CallsignTally tally;
            tally.callsign = statement.ColumnText(0);
            tally.count = static_cast<int>(statement.ColumnInt64(1));
            tally.last_date = statement.ColumnText(2);
            tallies.push_back(tally);
        }
        return tallies;
    }

    std::vector<CallsignTally> Database::GetSavedStationActivity(std::int64_t net_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT s.callsign, CASE WHEN s.name != '' THEN s.name ELSE COALESCE(st.name, '') END, COUNT(i.id),
               COALESCE(MAX(i.instance_date), '')
        FROM net_saved_stations s
        LEFT JOIN stations st ON st.callsign = s.callsign
        LEFT JOIN check_ins c ON c.callsign = s.callsign AND (s.name = '' OR c.name = s.name COLLATE NOCASE)
        LEFT JOIN net_instances i ON i.id = c.net_instance_id AND i.net_id = s.net_id
        WHERE s.net_id = ?
        GROUP BY s.callsign, s.name
        ORDER BY COALESCE(MAX(i.instance_date), ''), s.callsign, s.name;
    )sql");
        statement.BindInt64(0, net_id);
        std::vector<CallsignTally> tallies;
        while (statement.Step())
        {
            CallsignTally tally;
            tally.callsign = statement.ColumnText(0);
            tally.name = statement.ColumnText(1);
            tally.count = static_cast<int>(statement.ColumnInt64(2));
            tally.last_date = statement.ColumnText(3);
            tallies.push_back(tally);
        }
        return tallies;
    }

    StationActivity Database::GetStationActivity(const std::string& callsign)
    {
        StationActivity activity;
        {
            Statement statement(&statements_, R"sql(
            SELECT COUNT(*), COALESCE(MIN(checked_in_at), 0), COALESCE(MAX(checked_in_at), 0)
            FROM check_ins WHERE callsign = ?;
        )sql");
            statement.BindText(0, ToUpperAscii(callsign));
            if (statement.Step())
            {
                activity.check_ins = static_cast<int>(statement.ColumnInt64(0));
                activity.first_at = statement.ColumnInt64(1);
                activity.last_at = statement.ColumnInt64(2);
            }
        }
        Statement nets(&statements_, R"sql(
        SELECT n.name FROM net_saved_stations s JOIN nets n ON n.id = s.net_id
        WHERE s.callsign = ? ORDER BY n.name COLLATE NOCASE;
    )sql");
        nets.BindText(0, ToUpperAscii(callsign));
        while (nets.Step())
        {
            activity.saved_to_nets.push_back(nets.ColumnText(0));
        }
        return activity;
    }

    std::vector<CheckIn> Database::GetCheckInsForNetInstance(std::int64_t net_instance_id)
    {
        Statement statement(&statements_, R"sql(
        SELECT id, net_instance_id, callsign, sequence_number, signal_report,
               remarks, comment, checked_in_at, designated_role, name
        FROM check_ins WHERE net_instance_id = ? ORDER BY sequence_number;
    )sql");
        statement.BindInt64(0, net_instance_id);
        std::vector<CheckIn> check_ins;
        while (statement.Step())
        {
            check_ins.push_back(ReadCheckInRow(statement));
        }
        return check_ins;
    }

    std::vector<CheckIn> Database::GetCheckInsForNet(std::int64_t net_id)
    {
        Statement statement(&statements_, "SELECT " QL_CHECK_IN_COLUMNS
                                          " FROM check_ins c JOIN net_instances i ON i.id = c.net_instance_id"
                                          " WHERE i.net_id = ? ORDER BY c.net_instance_id, c.sequence_number;");
        statement.BindInt64(0, net_id);
        std::vector<CheckIn> check_ins;
        while (statement.Step())
        {
            check_ins.push_back(ReadCheckInRow(statement));
        }
        return check_ins;
    }

    void Database::UpdateCheckIn(const CheckIn& check_in)
    {
        Statement statement(&statements_, R"sql(
        UPDATE check_ins
        SET callsign = ?, sequence_number = ?, signal_report = ?,
            remarks = ?, comment = ?, checked_in_at = ?, designated_role = ?, name = ?
        WHERE id = ?;
    )sql");
        statement.BindText(0, ToUpperAscii(check_in.callsign));
        statement.BindInt64(1, check_in.sequence_number);
        statement.BindText(2, check_in.signal_report);
        statement.BindText(3, check_in.remarks);
        statement.BindText(4, check_in.comment);
        statement.BindInt64(5, check_in.checked_in_at);
        statement.BindInt64(6, check_in.designated_role);
        statement.BindText(7, check_in.name);
        statement.BindInt64(8, check_in.id);
        statement.Step();
    }

    void Database::DeleteCheckIn(std::int64_t check_in_id)
    {
        WriteTransaction transaction(this);
        std::int64_t instance_id = 0;
        bool closed = false;
        {
            Statement find(&statements_, R"sql(
            SELECT i.id, i.status FROM check_ins c JOIN net_instances i ON i.id = c.net_instance_id
            WHERE c.id = ?;
        )sql");
            find.BindInt64(0, check_in_id);
            if (find.Step())
            {
                instance_id = find.ColumnInt64(0);
                closed = find.ColumnInt64(1) == static_cast<std::int64_t>(NetInstanceStatus::kClosed);
            }
        }
        Statement statement(&statements_, "DELETE FROM check_ins WHERE id = ?;");
        statement.BindInt64(0, check_in_id);
        statement.Step();
        if (closed)
        {
            RenumberCheckIns(instance_id);
        }
        DeleteUnusedStations();
        transaction.Commit();
    }

    void Database::ClearCheckInRoleForInstance(std::int64_t net_instance_id, int role, std::int64_t except_check_in_id)
    {
        Statement statement(&statements_, R"sql(
        UPDATE check_ins SET designated_role = -1
        WHERE net_instance_id = ? AND designated_role = ? AND id != ?;
    )sql");
        statement.BindInt64(0, net_instance_id);
        statement.BindInt64(1, role);
        statement.BindInt64(2, except_check_in_id);
        statement.Step();
    }

    std::optional<ImportRunStatus> Database::GetImportRunStatus(const std::string& source)
    {
        Statement statement(&statements_, R"sql(
        SELECT source, status, started_at, completed_at, records_imported, last_error,
               phase, percent, heartbeat_at, requested_at
        FROM import_runs WHERE source = ?;
    )sql");
        statement.BindText(0, source);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        ImportRunStatus result;
        result.source = statement.ColumnText(0);
        result.status = statement.ColumnText(1);
        result.started_at = statement.ColumnInt64(2);
        result.completed_at = statement.ColumnInt64(3);
        result.records_imported = statement.ColumnInt64(4);
        result.last_error = statement.ColumnText(5);
        result.phase = statement.ColumnText(6);
        result.percent = static_cast<int>(statement.ColumnInt64(7));
        result.heartbeat_at = statement.ColumnInt64(8);
        result.requested_at = statement.ColumnInt64(9);
        return result;
    }

    bool Database::TryClaimImportRun(const std::string& source, std::int64_t now, std::int64_t stale_after_seconds)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO import_runs
            (source, status, started_at, completed_at, records_imported, last_error,
             phase, percent, heartbeat_at)
        VALUES (?, 'running', ?, 0, 0, '', 'Starting', 0, ?)
        ON CONFLICT(source) DO UPDATE SET
            status = 'running',
            started_at = excluded.started_at,
            completed_at = 0,
            records_imported = 0,
            last_error = '',
            phase = 'Starting',
            percent = 0,
            heartbeat_at = excluded.heartbeat_at
        WHERE import_runs.status != 'running' OR import_runs.heartbeat_at < ?;
    )sql");
        statement.BindText(0, source);
        statement.BindInt64(1, now);
        statement.BindInt64(2, now);
        statement.BindInt64(3, now - stale_after_seconds);
        statement.Step();
        return sqlite3_changes(db_) > 0;
    }

    void Database::UpdateImportProgress(const std::string& source, const std::string& phase, int percent,
                                        std::int64_t records_imported, std::int64_t now)
    {
        Statement statement(&statements_, R"sql(
        UPDATE import_runs
        SET phase = ?, percent = ?, records_imported = ?, heartbeat_at = ?
        WHERE source = ?;
    )sql");
        statement.BindText(0, phase);
        statement.BindInt64(1, percent);
        statement.BindInt64(2, records_imported);
        statement.BindInt64(3, now);
        statement.BindText(4, source);
        statement.Step();
    }

    void Database::RequestImportRun(const std::string& source, std::int64_t now)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO import_runs (source, requested_at) VALUES (?, ?)
        ON CONFLICT(source) DO UPDATE SET requested_at = excluded.requested_at;
    )sql");
        statement.BindText(0, source);
        statement.BindInt64(1, now);
        statement.Step();
    }

    void Database::UpsertImportRunStatus(const ImportRunStatus& status)
    {
        // requested_at is deliberately left alone: it records a request, and
        // only RequestImportRun sets it.
        Statement statement(&statements_, R"sql(
        INSERT INTO import_runs
            (source, status, started_at, completed_at, records_imported, last_error,
             phase, percent, heartbeat_at)
        VALUES (?,?,?,?,?,?,?,?,?)
        ON CONFLICT(source) DO UPDATE SET
            status = excluded.status,
            started_at = excluded.started_at,
            completed_at = excluded.completed_at,
            records_imported = excluded.records_imported,
            last_error = excluded.last_error,
            phase = excluded.phase,
            percent = excluded.percent,
            heartbeat_at = excluded.heartbeat_at;
    )sql");
        statement.BindText(0, status.source);
        statement.BindText(1, status.status);
        statement.BindInt64(2, status.started_at);
        statement.BindInt64(3, status.completed_at);
        statement.BindInt64(4, status.records_imported);
        statement.BindText(5, status.last_error);
        statement.BindText(6, status.phase);
        statement.BindInt64(7, status.percent);
        statement.BindInt64(8, status.heartbeat_at);
        statement.Step();
    }

    void Database::BulkUpsertUlsStations(const std::vector<Station>& stations, std::size_t begin, std::size_t end,
                                         std::int64_t updated_at, LicenseTable table)
    {
        // The WHERE on the update skips rows whose data hasn't changed -- on a
        // weekly refresh that's nearly all of them, and an unchanged row then
        // costs a lookup instead of a rewrite of its table and index pages.
        // (So last_updated means "last changed", not "last seen".)
        Statement statement(&statements_, table == LicenseTable::kGmrs ? R"sql(
        INSERT INTO gmrs_stations
            (callsign, name, street_address, city, state, zip, license_class, last_updated)
        VALUES (?,?,?,?,?,?,?,?)
        ON CONFLICT(callsign) DO UPDATE SET
            name = excluded.name,
            street_address = excluded.street_address,
            city = excluded.city,
            state = excluded.state,
            zip = excluded.zip,
            license_class = excluded.license_class,
            last_updated = excluded.last_updated
        WHERE gmrs_stations.name IS NOT excluded.name
           OR gmrs_stations.street_address IS NOT excluded.street_address
           OR gmrs_stations.city IS NOT excluded.city
           OR gmrs_stations.state IS NOT excluded.state
           OR gmrs_stations.zip IS NOT excluded.zip
           OR gmrs_stations.license_class IS NOT excluded.license_class;
    )sql"
                                                                       : R"sql(
        INSERT INTO uls_stations
            (callsign, name, street_address, city, state, zip, license_class, last_updated)
        VALUES (?,?,?,?,?,?,?,?)
        ON CONFLICT(callsign) DO UPDATE SET
            name = excluded.name,
            street_address = excluded.street_address,
            city = excluded.city,
            state = excluded.state,
            zip = excluded.zip,
            license_class = excluded.license_class,
            last_updated = excluded.last_updated
        WHERE uls_stations.name IS NOT excluded.name
           OR uls_stations.street_address IS NOT excluded.street_address
           OR uls_stations.city IS NOT excluded.city
           OR uls_stations.state IS NOT excluded.state
           OR uls_stations.zip IS NOT excluded.zip
           OR uls_stations.license_class IS NOT excluded.license_class;
    )sql");

        WriteTransaction transaction(this);
        for (std::size_t i = begin; i < end && i < stations.size(); ++i)
        {
            const Station& station = stations[i];
            statement.Reset();
            statement.BindText(0, ToUpperAscii(station.callsign));
            statement.BindText(1, station.name);
            statement.BindText(2, station.street_address);
            statement.BindText(3, station.city);
            statement.BindText(4, station.state);
            statement.BindText(5, station.zip);
            statement.BindText(6, station.license_class);
            statement.BindInt64(7, updated_at);
            statement.Step();
        }
        transaction.Commit();
    }

    std::vector<NearbyUlsStation> Database::SearchNearbyUlsStations(const std::string& substring,
                                                                    const std::vector<NearbyZip>& nearby_zips,
                                                                    const std::vector<std::string>& zip3_prefixes,
                                                                    int limit, LicenseTable table) const
    {
        const char* stations = table == LicenseTable::kGmrs ? "gmrs_stations" : "uls_stations";
        std::vector<NearbyUlsStation> results;
        if (nearby_zips.empty())
        {
            return results;
        }

        // The nearby ZIPs and their distances go in as a VALUES list, and
        // the join is driven from it -- one zip-index lookup per nearby ZIP
        // -- so SQLite reads only nearby stations and sorts just the
        // matches, ~10 ms. (Joining the other way round scans the VALUES
        // list once per station, 20 times slower.) The second half finds
        // stations whose ZIP has no centroid, by ZIP3 range, rather than
        // zip LIKE "374%", which can't use the index.
        std::string sql = "WITH nearby(zip, miles) AS (VALUES ";
        for (std::size_t i = 0; i < nearby_zips.size(); ++i)
        {
            sql += i > 0 ? ",(?,?)" : "(?,?)";
        }
        sql +=
            ") SELECT callsign, name, street_address, city, state, zip, license_class, "
            "last_updated, COALESCE(miles, -1.0) FROM ("
            "SELECT u.*, n.miles AS miles FROM nearby n JOIN ";
        sql += stations;
        sql += " u ON u.zip = n.zip WHERE instr(u.callsign, ?) > 0";
        if (!zip3_prefixes.empty())
        {
            sql += " UNION ALL SELECT u.*, NULL AS miles FROM ";
            sql += stations;
            sql += " u WHERE instr(u.callsign, ?) > 0 AND (";
            for (std::size_t i = 0; i < zip3_prefixes.size(); ++i)
            {
                sql += i > 0 ? " OR (u.zip >= ? AND u.zip < ?)" : "(u.zip >= ? AND u.zip < ?)";
            }
            sql +=
                ") AND NOT EXISTS (SELECT 1 FROM zip_centroids c WHERE c.zip = u.zip)"
                " AND u.zip NOT IN (SELECT zip FROM nearby)";
        }
        sql += ") ORDER BY miles IS NULL, miles, callsign LIMIT ?;";

        Statement statement(db_, sql);
        int index = 0;
        for (const NearbyZip& nearby : nearby_zips)
        {
            statement.BindText(index++, nearby.zip);
            statement.BindDouble(index++, nearby.miles);
        }
        std::string upper = ToUpperAscii(substring);
        statement.BindText(index++, upper);
        if (!zip3_prefixes.empty())
        {
            statement.BindText(index++, upper);
            for (const std::string& prefix : zip3_prefixes)
            {
                // The first string after every one starting with `prefix`:
                // bump its last character ("374" -> "375", "379" -> "37:").
                std::string after_prefix = prefix;
                if (!after_prefix.empty())
                {
                    after_prefix.back() = static_cast<char>(after_prefix.back() + 1);
                }
                statement.BindText(index++, prefix);
                statement.BindText(index++, after_prefix);
            }
        }
        statement.BindInt64(index, limit);

        while (statement.Step())
        {
            NearbyUlsStation found;
            Station& station = found.station;
            station.callsign = statement.ColumnText(0);
            station.name = statement.ColumnText(1);
            station.street_address = statement.ColumnText(2);
            station.city = statement.ColumnText(3);
            station.state = statement.ColumnText(4);
            station.zip = statement.ColumnText(5);
            station.license_class = statement.ColumnText(6);
            station.last_updated = statement.ColumnInt64(7);
            station.data_source = table == LicenseTable::kGmrs ? StationDataSource::kGmrs : StationDataSource::kUls;
            found.miles = statement.ColumnDouble(8);
            results.push_back(std::move(found));
        }
        return results;
    }

    static bool NearbyUlsCallsignComesFirst(const NearbyUlsCallsign& a, const NearbyUlsCallsign& b)
    {
        bool a_unknown = a.miles < 0.0F;
        bool b_unknown = b.miles < 0.0F;
        if (a_unknown != b_unknown)
        {
            return b_unknown;
        }
        if (a.miles != b.miles)
        {
            return a.miles < b.miles;
        }
        return std::strcmp(a.callsign, b.callsign) < 0;
    }

    // Appends `statement`'s rows (a callsign each) to `results`, each at
    // `miles` away.
    static void AppendNearbyCallsigns(Statement* statement, float miles, std::vector<NearbyUlsCallsign>* results)
    {
        while (statement->Step())
        {
            std::string callsign = statement->ColumnText(0);
            if (callsign.size() >= sizeof(NearbyUlsCallsign::callsign))
            {
                continue;  // Not a US call sign; none are this long.
            }
            NearbyUlsCallsign nearby;
            std::memcpy(nearby.callsign, callsign.c_str(), callsign.size() + 1);
            nearby.miles = miles;
            results->push_back(nearby);
        }
    }

    std::vector<NearbyUlsCallsign> Database::ListNearbyUlsCallsigns(const std::vector<NearbyZip>& nearby_zips,
                                                                    const std::vector<std::string>& zip3_prefixes,
                                                                    LicenseTable table)
    {
        std::vector<NearbyUlsCallsign> results;
        if (nearby_zips.empty())
        {
            return results;
        }
        // One small query per ZIP, each read from the (zip, callsign) index
        // alone, rather than one query joining them all: SQLite would build
        // and hold temporary tables for the join, several megabytes at the
        // widest radius, where these hold nothing.
        Statement in_zip(&statements_, table == LicenseTable::kGmrs
                                           ? "SELECT callsign FROM gmrs_stations WHERE zip = ?;"
                                           : "SELECT callsign FROM uls_stations WHERE zip = ?;");
        for (const NearbyZip& nearby : nearby_zips)
        {
            in_zip.Reset();
            in_zip.BindText(0, nearby.zip);
            AppendNearbyCallsigns(&in_zip, static_cast<float>(nearby.miles), &results);
        }
        // Stations whose ZIP has no centroid (so, no distance), in the
        // nearby ZIPs' first three digits -- see SearchNearbyUlsStations.
        // (A nearby ZIP has a centroid, so none of these is one of them.)
        Statement no_centroid(&statements_, table == LicenseTable::kGmrs ? R"sql(
        SELECT u.callsign FROM gmrs_stations u WHERE u.zip >= ? AND u.zip < ?
        AND NOT EXISTS (SELECT 1 FROM zip_centroids c WHERE c.zip = u.zip);
    )sql"
                                                                         : R"sql(
        SELECT u.callsign FROM uls_stations u WHERE u.zip >= ? AND u.zip < ?
        AND NOT EXISTS (SELECT 1 FROM zip_centroids c WHERE c.zip = u.zip);
    )sql");
        for (const std::string& prefix : zip3_prefixes)
        {
            std::string after_prefix = prefix;
            if (!after_prefix.empty())
            {
                after_prefix.back() = static_cast<char>(after_prefix.back() + 1);
            }
            no_centroid.Reset();
            no_centroid.BindText(0, prefix);
            no_centroid.BindText(1, after_prefix);
            AppendNearbyCallsigns(&no_centroid, -1.0F, &results);
        }
        // Nearest first, those of unknown distance last, then by callsign.
        std::sort(results.begin(), results.end(), NearbyUlsCallsignComesFirst);
        return results;
    }

    // A licensed station (uls_stations or ised_stations, which share their
    // columns) from `statement`'s current row.
    static Station ReadLicensedStationRow(const Statement& statement, StationDataSource source)
    {
        Station station;
        station.callsign = statement.ColumnText(0);
        station.name = statement.ColumnText(1);
        station.street_address = statement.ColumnText(2);
        station.city = statement.ColumnText(3);
        station.state = statement.ColumnText(4);
        station.zip = statement.ColumnText(5);
        station.license_class = statement.ColumnText(6);
        station.last_updated = statement.ColumnInt64(7);
        station.data_source = source;
        return station;
    }

    void Database::ReplaceIsedStations(const std::vector<Station>& stations, std::int64_t updated_at)
    {
        WriteTransaction transaction(this);
        sqlite3_exec(db_, "DELETE FROM ised_stations;", nullptr, nullptr, nullptr);
        Statement insert(&statements_, R"sql(
        INSERT OR REPLACE INTO ised_stations
            (callsign, name, street_address, city, state, zip, license_class, last_updated)
        VALUES (?,?,?,?,?,?,?,?);
    )sql");
        for (const Station& station : stations)
        {
            insert.Reset();
            insert.BindText(0, station.callsign);
            insert.BindText(1, station.name);
            insert.BindText(2, station.street_address);
            insert.BindText(3, station.city);
            insert.BindText(4, station.state);
            insert.BindText(5, station.zip);
            insert.BindText(6, station.license_class);
            insert.BindInt64(7, updated_at);
            insert.Step();
        }
        transaction.Commit();
    }

    std::optional<Station> Database::FindIsedStationByCallsign(const std::string& callsign)
    {
        Statement statement(&statements_, R"sql(
        SELECT callsign, name, street_address, city, state, zip, license_class,
               last_updated
        FROM ised_stations WHERE callsign = ?;
    )sql");
        statement.BindText(0, ToUpperAscii(callsign));
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return ReadLicensedStationRow(statement, StationDataSource::kIsed);
    }

    std::vector<Station> Database::SearchIsedStationsByCallsignPrefix(const std::string& prefix, int limit)
    {
        // A range on the primary key rather than LIKE, so it's an index
        // lookup: every callsign from `prefix` up to `prefix` followed by
        // '~', which sorts after every letter and digit.
        // With a "?" typed (see IsWildcardCallsign), the first character
        // typed still starts the callsign and the rest follow in order: the
        // range is that first character's, and LIKE does the rest.
        bool wildcard = IsWildcardCallsign(prefix);
        if (wildcard && !WildcardHasEnough(prefix))
        {
            return {};
        }
        std::string upper = wildcard ? NormalizeCallsign(prefix).substr(0, 1) : ToUpperAscii(prefix);
        Statement statement(&statements_, R"sql(
        SELECT callsign, name, street_address, city, state, zip, license_class,
               last_updated
        FROM ised_stations WHERE callsign >= ? AND callsign < ? AND callsign LIKE ?
        ORDER BY callsign LIMIT ?;
    )sql");
        statement.BindText(0, upper);
        statement.BindText(1, upper + "~");
        statement.BindText(2, wildcard ? WildcardLikePattern(prefix, true) : "%");
        statement.BindInt64(3, limit);
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadLicensedStationRow(statement, StationDataSource::kIsed));
        }
        return stations;
    }

    std::vector<Station> Database::SearchIsedStationsByCallsignSubstring(const std::string& substring, int limit)
    {
        // The inner query reads only the primary key's index, not the rows
        // (about 90,000 call signs); only the matches' rows are read after.
        // With a "?" typed: the characters in that order (see IsWildcardCallsign).
        bool wildcard = IsWildcardCallsign(substring);
        if (wildcard && !WildcardHasEnough(substring))
        {
            return {};
        }
        Statement statement(&statements_, wildcard ? R"sql(
        SELECT callsign, name, street_address, city, state, zip, license_class,
               last_updated
        FROM ised_stations
        WHERE callsign IN (SELECT callsign FROM ised_stations WHERE callsign LIKE ?
                           ORDER BY callsign LIMIT ?)
        ORDER BY callsign;
    )sql"
                                                   : R"sql(
        SELECT callsign, name, street_address, city, state, zip, license_class,
               last_updated
        FROM ised_stations
        WHERE callsign IN (SELECT callsign FROM ised_stations WHERE instr(callsign, ?) > 0
                           ORDER BY callsign LIMIT ?)
        ORDER BY callsign;
    )sql");
        statement.BindText(0, wildcard ? WildcardLikePattern(substring, false) : ToUpperAscii(substring));
        statement.BindInt64(1, limit);
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadLicensedStationRow(statement, StationDataSource::kIsed));
        }
        return stations;
    }

    std::optional<Station> Database::FindLicensedStationByCallsign(const std::string& callsign)
    {
        std::optional<Station> station = FindUlsStationByCallsign(callsign);
        return station.has_value() ? station : FindIsedStationByCallsign(callsign);
    }

    std::optional<Station> Database::FindUlsStationByCallsign(const std::string& callsign, LicenseTable table)
    {
        Statement statement(&statements_, table == LicenseTable::kGmrs ? R"sql(
        SELECT callsign, name, street_address, city, state, zip, license_class,
               last_updated
        FROM gmrs_stations WHERE callsign = ?;
    )sql"
                                                                       : R"sql(
        SELECT callsign, name, street_address, city, state, zip, license_class,
               last_updated
        FROM uls_stations WHERE callsign = ?;
    )sql");
        statement.BindText(0, ToUpperAscii(callsign));
        if (!statement.Step())
        {
            return std::nullopt;
        }
        Station station;
        station.callsign = statement.ColumnText(0);
        station.name = statement.ColumnText(1);
        station.street_address = statement.ColumnText(2);
        station.city = statement.ColumnText(3);
        station.state = statement.ColumnText(4);
        station.zip = statement.ColumnText(5);
        station.license_class = statement.ColumnText(6);
        station.last_updated = statement.ColumnInt64(7);
        station.data_source = table == LicenseTable::kGmrs ? StationDataSource::kGmrs : StationDataSource::kUls;
        return station;
    }

    std::vector<Station> Database::FindUlsStationsByCallsigns(const std::vector<std::string>& callsigns,
                                                              LicenseTable table) const
    {
        std::vector<Station> stations;
        if (callsigns.empty())
        {
            return stations;
        }
        // Few: a screenful of autocomplete matches.
        std::string sql = std::string(
                              "SELECT callsign, name, street_address, city, state, zip, license_class, "
                              "last_updated FROM ") +
                          (table == LicenseTable::kGmrs ? "gmrs_stations" : "uls_stations") + " WHERE callsign IN (";
        for (std::size_t i = 0; i < callsigns.size(); ++i)
        {
            sql.append(i == 0 ? "?" : ",?");
        }
        sql.append(");");
        Statement statement(db_, sql);
        for (std::size_t i = 0; i < callsigns.size(); ++i)
        {
            statement.BindText(static_cast<int>(i), ToUpperAscii(callsigns[i]));
        }
        while (statement.Step())
        {
            Station station;
            station.callsign = statement.ColumnText(0);
            station.name = statement.ColumnText(1);
            station.street_address = statement.ColumnText(2);
            station.city = statement.ColumnText(3);
            station.state = statement.ColumnText(4);
            station.zip = statement.ColumnText(5);
            station.license_class = statement.ColumnText(6);
            station.last_updated = statement.ColumnInt64(7);
            station.data_source = table == LicenseTable::kGmrs ? StationDataSource::kGmrs : StationDataSource::kUls;
            stations.push_back(std::move(station));
        }
        std::sort(stations.begin(), stations.end(), StationsByCallsign());
        return stations;
    }

    std::vector<NearbyUlsCallsign> Database::ListNearbyIsedCallsigns(const std::vector<NearbyZip>& nearby_zips)
    {
        std::vector<NearbyUlsCallsign> results;
        // "K1A 0B1" sorts between "K1A" and "K1A~".
        Statement in_fsa(&statements_, "SELECT callsign FROM ised_stations WHERE zip >= ? AND zip < ?;");
        std::string upper_bound;
        for (const NearbyZip& nearby : nearby_zips)
        {
            if (nearby.zip.size() != 3)
            {
                continue;
            }
            upper_bound.assign(nearby.zip);
            upper_bound.push_back('~');
            in_fsa.Reset();
            in_fsa.BindText(0, nearby.zip);
            in_fsa.BindText(1, upper_bound);
            AppendNearbyCallsigns(&in_fsa, static_cast<float>(nearby.miles), &results);
        }
        std::sort(results.begin(), results.end(), NearbyUlsCallsignComesFirst);
        return results;
    }

    std::vector<Station> Database::FindIsedStationsByCallsigns(const std::vector<std::string>& callsigns) const
    {
        std::vector<Station> stations;
        if (callsigns.empty())
        {
            return stations;
        }
        std::string sql =
            "SELECT callsign, name, street_address, city, state, zip, license_class, last_updated "
            "FROM ised_stations WHERE callsign IN (";
        for (std::size_t i = 0; i < callsigns.size(); ++i)
        {
            sql.append(i == 0 ? "?" : ",?");
        }
        sql.append(");");
        Statement statement(db_, sql);
        for (std::size_t i = 0; i < callsigns.size(); ++i)
        {
            statement.BindText(static_cast<int>(i), ToUpperAscii(callsigns[i]));
        }
        while (statement.Step())
        {
            stations.push_back(ReadLicensedStationRow(statement, StationDataSource::kIsed));
        }
        std::sort(stations.begin(), stations.end(), StationsByCallsign());
        return stations;
    }

    void Database::BulkUpsertZipCentroids(const std::vector<ZipCentroid>& batch)
    {
        Statement statement(&statements_, R"sql(
        INSERT INTO zip_centroids (zip, lat, lon) VALUES (?, ?, ?)
        ON CONFLICT(zip) DO UPDATE SET lat = excluded.lat, lon = excluded.lon;
    )sql");

        WriteTransaction transaction(this);
        for (const ZipCentroid& centroid : batch)
        {
            statement.Reset();
            statement.BindText(0, centroid.zip);
            statement.BindDouble(1, centroid.lat);
            statement.BindDouble(2, centroid.lon);
            statement.Step();
        }
        transaction.Commit();
    }

    int Database::FillBlankGridSquaresFromZip()
    {
        // A US ZIP matches on its first five digits (so a ZIP+4 works), a
        // Canadian postal code on its three-character FSA (ZipCentroidKey).
        // One join (a digit starts a US ZIP, a letter a postal code), not a
        // lookup per station.
        Statement blanks(&statements_, R"sql(
        SELECT s.callsign, c.lat, c.lon FROM stations s
        JOIN zip_centroids c ON c.zip = CASE WHEN s.zip GLOB '[0-9]*' THEN substr(s.zip, 1, 5)
                                             ELSE upper(substr(s.zip, 1, 3)) END
        WHERE s.grid_square = '';
    )sql");
        std::vector<std::pair<std::string, std::string>> grids;
        while (blanks.Step())
        {
            std::string grid = MaidenheadGrid4(blanks.ColumnDouble(1), blanks.ColumnDouble(2));
            if (!grid.empty())
            {
                grids.emplace_back(blanks.ColumnText(0), std::move(grid));
            }
        }
        if (grids.empty())
        {
            return 0;
        }

        Statement update(&statements_,
                         "UPDATE stations SET grid_square = ? WHERE callsign = ? "
                         "AND grid_square = '';");
        WriteTransaction transaction(this);
        for (const std::pair<std::string, std::string>& grid : grids)
        {
            update.Reset();
            update.BindText(0, grid.second);
            update.BindText(1, grid.first);
            update.Step();
        }
        transaction.Commit();
        return static_cast<int>(grids.size());
    }

    bool Database::HasAnyZipCentroids()
    {
        Statement statement(&statements_, "SELECT EXISTS(SELECT 1 FROM zip_centroids LIMIT 1);");
        statement.Step();
        return statement.ColumnInt64(0) != 0;
    }

    std::vector<ZipCentroid> Database::GetAllZipCentroids()
    {
        Statement statement(&statements_, "SELECT zip, lat, lon FROM zip_centroids;");
        std::vector<ZipCentroid> results;
        while (statement.Step())
        {
            ZipCentroid centroid;
            centroid.zip = statement.ColumnText(0);
            centroid.lat = statement.ColumnDouble(1);
            centroid.lon = statement.ColumnDouble(2);
            results.push_back(std::move(centroid));
        }
        return results;
    }

    std::optional<ZipCentroid> Database::FindZipCentroid(const std::string& zip)
    {
        Statement statement(&statements_, "SELECT zip, lat, lon FROM zip_centroids WHERE zip = ?;");
        statement.BindText(0, zip);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        ZipCentroid centroid;
        centroid.zip = statement.ColumnText(0);
        centroid.lat = statement.ColumnDouble(1);
        centroid.lon = statement.ColumnDouble(2);
        return centroid;
    }

    std::vector<ZipCentroid> Database::GetZipCentroidsInBox(double min_lat, double max_lat, double min_lon,
                                                            double max_lon)
    {
        Statement statement(&statements_, R"sql(
        SELECT zip, lat, lon FROM zip_centroids
        WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?;
    )sql");
        statement.BindDouble(0, min_lat);
        statement.BindDouble(1, max_lat);
        statement.BindDouble(2, min_lon);
        statement.BindDouble(3, max_lon);
        std::vector<ZipCentroid> results;
        while (statement.Step())
        {
            ZipCentroid centroid;
            centroid.zip = statement.ColumnText(0);
            centroid.lat = statement.ColumnDouble(1);
            centroid.lon = statement.ColumnDouble(2);
            results.push_back(std::move(centroid));
        }
        return results;
    }

    std::string Database::FindZipCounty(const std::string& zip)
    {
        Statement statement(&statements_, "SELECT county FROM zip_counties WHERE zip = ?;");
        statement.BindText(0, zip);
        return statement.Step() ? statement.ColumnText(0) : std::string();
    }

    std::string Database::FindZipPlaceCounty(const std::string& zip, const std::string& place)
    {
        Statement statement(&statements_, "SELECT county FROM zip_place_counties WHERE zip = ? AND place = ?;");
        statement.BindText(0, zip);
        statement.BindText(1, place);
        return statement.Step() ? statement.ColumnText(0) : std::string();
    }

    int Database::DeleteUlsStationsNotIn(const std::vector<Station>& current)
    {
        std::vector<std::string_view> callsigns;
        callsigns.reserve(current.size());
        for (const Station& station : current)
        {
            callsigns.emplace_back(station.callsign);
        }
        return DeleteUlsStationsNotIn(callsigns);
    }

    int Database::DeleteUlsStationsNotIn(const std::vector<std::string_view>& current_callsigns, LicenseTable table)
    {
        WriteTransaction transaction(this);
        sqlite3_exec(db_,
                     "CREATE TEMP TABLE IF NOT EXISTS current_uls_callsigns "
                     "(callsign TEXT PRIMARY KEY);"
                     "DELETE FROM current_uls_callsigns;",
                     nullptr, nullptr, nullptr);
        {
            Statement insert(&statements_, "INSERT OR IGNORE INTO current_uls_callsigns (callsign) VALUES (?);");
            for (std::string_view callsign : current_callsigns)
            {
                insert.Reset();
                insert.BindText(0, ToUpperAscii(std::string(callsign)));
                insert.Step();
            }
        }
        {
            Statement remove(&statements_, table == LicenseTable::kGmrs
                                               ? "DELETE FROM gmrs_stations WHERE callsign NOT IN "
                                                 "(SELECT callsign FROM current_uls_callsigns);"
                                               : "DELETE FROM uls_stations WHERE callsign NOT IN "
                                                 "(SELECT callsign FROM current_uls_callsigns);");
            remove.Step();
        }
        int deleted = sqlite3_changes(db_);
        sqlite3_exec(db_, "DROP TABLE current_uls_callsigns;", nullptr, nullptr, nullptr);
        transaction.Commit();
        return deleted;
    }

    void Database::ReplaceZipCountyData(const std::vector<ZipCounty>& zip_counties,
                                        const std::vector<ZipPlaceCounty>& zip_place_counties)
    {
        WriteTransaction transaction(this);
        sqlite3_exec(db_, "DELETE FROM zip_counties;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "DELETE FROM zip_place_counties;", nullptr, nullptr, nullptr);

        Statement insert_zip(&statements_, "INSERT INTO zip_counties (zip, county) VALUES (?, ?);");
        for (const ZipCounty& zip_county : zip_counties)
        {
            insert_zip.Reset();
            insert_zip.BindText(0, zip_county.zip);
            insert_zip.BindText(1, zip_county.county);
            insert_zip.Step();
        }

        Statement insert_place(&statements_, R"sql(
        INSERT OR REPLACE INTO zip_place_counties (zip, place, county) VALUES (?, ?, ?);
    )sql");
        for (const ZipPlaceCounty& zip_place : zip_place_counties)
        {
            insert_place.Reset();
            insert_place.BindText(0, zip_place.zip);
            insert_place.BindText(1, zip_place.place);
            insert_place.BindText(2, zip_place.county);
            insert_place.Step();
        }
        transaction.Commit();
    }

    std::vector<ZipCounty> Database::GetAllZipCounties()
    {
        Statement statement(&statements_, "SELECT zip, county FROM zip_counties;");
        std::vector<ZipCounty> results;
        while (statement.Step())
        {
            ZipCounty zip_county;
            zip_county.zip = statement.ColumnText(0);
            zip_county.county = statement.ColumnText(1);
            results.push_back(std::move(zip_county));
        }
        return results;
    }

    std::vector<ZipPlaceCounty> Database::GetAllZipPlaceCounties()
    {
        Statement statement(&statements_, "SELECT zip, place, county FROM zip_place_counties;");
        std::vector<ZipPlaceCounty> results;
        while (statement.Step())
        {
            ZipPlaceCounty zip_place;
            zip_place.zip = statement.ColumnText(0);
            zip_place.place = statement.ColumnText(1);
            zip_place.county = statement.ColumnText(2);
            results.push_back(std::move(zip_place));
        }
        return results;
    }

    bool Database::HasAnyUlsStations(LicenseTable table)
    {
        Statement statement(&statements_, table == LicenseTable::kGmrs
                                              ? "SELECT EXISTS(SELECT 1 FROM gmrs_stations LIMIT 1);"
                                              : "SELECT EXISTS(SELECT 1 FROM uls_stations LIMIT 1);");
        statement.Step();
        return statement.ColumnInt64(0) != 0;
    }

    std::int64_t Database::DataVersion()
    {
        Statement statement(&statements_, "PRAGMA data_version;");
        statement.Step();
        return statement.ColumnInt64(0);
    }

    static User ReadUserRow(const Statement& row)
    {
        User user;
        user.id = row.ColumnInt64(0);
        user.username = row.ColumnText(1);
        user.public_key = row.ColumnText(2);
        user.created_at = row.ColumnInt64(3);
        user.last_login_at = row.ColumnInt64(4);
        user.view_only = row.ColumnInt64(5) != 0;
        user.amateur_callsign = row.ColumnText(6);
        user.gmrs_callsign = row.ColumnText(7);
        user.transfer_method = static_cast<int>(row.ColumnInt64(8));
        return user;
    }

    bool Database::CreateUser(const User& user)
    {
        std::vector<User> existing_keys = GetUserKeys(user.username);
        // Another key is filed under the username as it's already written.
        std::string username = existing_keys.empty() ? user.username : existing_keys[0].username;
        for (const User& existing : existing_keys)
        {
            if (SamePublicKey(existing.public_key, user.public_key))
            {
                Statement update(&statements_, "UPDATE users SET public_key = ? WHERE id = ?;");
                update.BindText(0, user.public_key);
                update.BindInt64(1, existing.id);
                update.Step();
                return false;
            }
        }
        // Another key for an existing username takes that username's
        // access and call signs, whatever `user` says.
        Statement statement(&statements_, R"sql(
        INSERT INTO users (username, public_key, created_at, last_login_at, view_only, amateur_callsign,
                           gmrs_callsign)
        VALUES (?,?,?,0, COALESCE((SELECT view_only FROM users WHERE username = ? LIMIT 1), ?),
                COALESCE((SELECT amateur_callsign FROM users WHERE username = ? LIMIT 1), ?),
                COALESCE((SELECT gmrs_callsign FROM users WHERE username = ? LIMIT 1), ?));
    )sql");
        statement.BindText(0, username);
        statement.BindText(1, user.public_key);
        statement.BindInt64(2, user.created_at);
        statement.BindText(3, username);
        statement.BindInt64(4, user.view_only ? 1 : 0);
        statement.BindText(5, username);
        statement.BindText(6, ToUpperAscii(user.amateur_callsign));
        statement.BindText(7, username);
        statement.BindText(8, ToUpperAscii(user.gmrs_callsign));
        statement.Step();
        return true;
    }

    void Database::SetUserViewOnly(const std::string& username, bool view_only)
    {
        Statement statement(&statements_, "UPDATE users SET view_only = ? WHERE username = ? COLLATE NOCASE;");
        statement.BindInt64(0, view_only ? 1 : 0);
        statement.BindText(1, username);
        statement.Step();
    }

    void Database::SetUserCallsigns(const std::string& username, const std::string& amateur_callsign,
                                    const std::string& gmrs_callsign)
    {
        Statement statement(&statements_,
                            "UPDATE users SET amateur_callsign = ?, gmrs_callsign = ? "
                            "WHERE username = ? COLLATE NOCASE;");
        statement.BindText(0, ToUpperAscii(amateur_callsign));
        statement.BindText(1, ToUpperAscii(gmrs_callsign));
        statement.BindText(2, username);
        statement.Step();
    }

    bool Database::IsUserViewOnly(const std::string& username)
    {
        Statement statement(&statements_,
                            "SELECT EXISTS(SELECT 1 FROM users WHERE username = ? COLLATE NOCASE AND "
                            "view_only);");
        statement.BindText(0, username);
        statement.Step();
        return statement.ColumnInt64(0) != 0;
    }

    std::vector<User> Database::GetUserKeys(const std::string& username)
    {
        Statement statement(&statements_, R"sql(
        SELECT id, username, public_key, created_at, last_login_at, view_only, amateur_callsign, gmrs_callsign,
               transfer_method
        FROM users WHERE username = ? COLLATE NOCASE ORDER BY id;
    )sql");
        statement.BindText(0, username);
        std::vector<User> keys;
        while (statement.Step())
        {
            keys.push_back(ReadUserRow(statement));
        }
        return keys;
    }

    std::vector<User> Database::ListUsers()
    {
        Statement statement(&statements_, R"sql(
        SELECT id, username, public_key, created_at, last_login_at, view_only, amateur_callsign, gmrs_callsign,
               transfer_method
        FROM users ORDER BY username COLLATE NOCASE, username, id;
    )sql");
        std::vector<User> users;
        while (statement.Step())
        {
            users.push_back(ReadUserRow(statement));
        }
        return users;
    }

    void Database::DeleteUserKey(std::int64_t id)
    {
        Statement statement(&statements_, "DELETE FROM users WHERE id = ?;");
        statement.BindInt64(0, id);
        statement.Step();
    }

    void Database::DeleteUser(const std::string& username)
    {
        Statement statement(&statements_, "DELETE FROM users WHERE username = ? COLLATE NOCASE;");
        statement.BindText(0, username);
        statement.Step();
    }

    bool Database::RenameUser(const std::string& old_username, const std::string& new_username)
    {
        for (const User& key : GetUserKeys(new_username))
        {
            if (ToUpperAscii(key.username) != ToUpperAscii(old_username))
            {
                return false;
            }
        }
        Statement statement(&statements_, "UPDATE users SET username = ? WHERE username = ? COLLATE NOCASE;");
        statement.BindText(0, new_username);
        statement.BindText(1, old_username);
        statement.Step();
        return true;
    }

    void Database::UpdateUserLastLogin(std::int64_t id, std::int64_t last_login_at)
    {
        Statement statement(&statements_, "UPDATE users SET last_login_at = ? WHERE id = ?;");
        statement.BindInt64(0, last_login_at);
        statement.BindInt64(1, id);
        statement.Step();
    }

    void Database::UpdateUserKeyTransfer(std::int64_t id, int transfer_method)
    {
        Statement statement(&statements_, "UPDATE users SET transfer_method = ? WHERE id = ?;");
        statement.BindInt64(0, transfer_method);
        statement.BindInt64(1, id);
        statement.Step();
    }

    void Database::UpdateUserKeyLine(std::int64_t id, const std::string& public_key)
    {
        Statement statement(&statements_, "UPDATE users SET public_key = ? WHERE id = ?;");
        statement.BindText(0, public_key);
        statement.BindInt64(1, id);
        statement.Step();
    }

}  // namespace ql
