#include "database.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

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
    is_ad_hoc INTEGER NOT NULL DEFAULT 0
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
    started_at INTEGER NOT NULL DEFAULT 0
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
    designated_role INTEGER NOT NULL DEFAULT -1
);
CREATE INDEX IF NOT EXISTS idx_check_ins_net_instance ON check_ins(net_instance_id);
CREATE INDEX IF NOT EXISTS idx_check_ins_callsign ON check_ins(callsign);

CREATE TABLE IF NOT EXISTS net_saved_stations (
    net_id INTEGER NOT NULL REFERENCES nets(id),
    callsign TEXT NOT NULL REFERENCES stations(callsign),
    default_remarks TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (net_id, callsign)
);

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
CREATE INDEX IF NOT EXISTS idx_uls_stations_zip ON uls_stations(zip);

CREATE TABLE IF NOT EXISTS zip_centroids (
    zip TEXT PRIMARY KEY,
    lat REAL NOT NULL,
    lon REAL NOT NULL
);

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
    username TEXT PRIMARY KEY,
    public_key TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT 0,
    last_login_at INTEGER NOT NULL DEFAULT 0
);
)sql";

    // The version of the upgrades CreateSchema has applied to this
    // database; see the comment there.
    static constexpr int kSchemaVersion = 4;

    static int ReadUserVersion(sqlite3* db)
    {
        Statement statement(db, "PRAGMA user_version;");
        return statement.Step() ? static_cast<int>(statement.ColumnInt64(0)) : 0;
    }

    // Adds `column` to `table` if it isn't already there, for evolving the
    // schema of a database created by an older version of QuickLogger without
    // forcing the user to delete it. kSchemaSql's CREATE TABLE only covers
    // brand-new databases.
    static void EnsureColumnExists(sqlite3* db, const char* table, const char* column,
                                   const char* column_declaration)
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
            std::string alter_sql = std::string("ALTER TABLE ") + table + " ADD COLUMN " + column +
                                    " " + column_declaration + ";";
            sqlite3_exec(db, alter_sql.c_str(), nullptr, nullptr, nullptr);
        }
    }

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
        net.notes = row.ColumnText(7);
        net.created_at = row.ColumnInt64(8);
        net.imported_at = row.ColumnInt64(9);
        net.is_ad_hoc = row.ColumnInt64(10) != 0;
        return net;
    }

    // A NetInstance from the 13 columns starting at `first` (see
    // kNetInstanceColumns for their order).
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
        return instance;
    }

    static NetInstance ReadNetInstanceRow(const Statement& row)
    {
        return ReadNetInstanceColumns(row, 0);
    }

    // A CheckIn from the 9 columns starting at `first` (see
    // kCheckInColumns).
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
        return check_in;
    }

    static CheckIn ReadCheckInRow(const Statement& row)
    {
        return ReadCheckInColumns(row, 0);
    }

    // The column lists the two readers above expect, for queries that join
    // net_instances (as i) and check_ins (as c).
    static const char* const kNetInstanceColumns =
        "i.id, i.net_id, i.instance_date, i.net_control_callsign, "
        "i.alternate_net_control_callsign, i.logger_callsign, i.created_by, i.frequency, "
        "i.location, i.status, i.closed_at, i.operator_role, i.started_at";
    static const char* const kCheckInColumns =
        "c.id, c.net_instance_id, c.callsign, c.sequence_number, c.signal_report, c.remarks, "
        "c.comment, c.checked_in_at, c.designated_role";

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
        sqlite3_busy_timeout(db_, 5000);
        sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
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
        CreateSchema();
    }

    Database::~Database()
    {
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
        EnsureColumnExists(db_, "net_saved_stations", "default_remarks",
                           "TEXT NOT NULL DEFAULT ''");
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
        sqlite3_exec(db_, "UPDATE uls_stations SET zip = substr(zip, 1, 5) WHERE length(zip) > 5;",
                     nullptr, nullptr, nullptr);

        // One-time fix-up for nets saved before a net's location became a
        // 5-digit ZIP field: keep the ZIP if the old free text has one
        // ("Chattanooga, TN 37415"), otherwise blank it ("Hamilton County"),
        // so an older net can still be saved from Edit Net without first
        // having to clear a value the form now rejects. A blank ZIP just
        // means autocomplete measures from the operator's home ZIP.
        NormalizeNetZips();

        std::string set_version = "PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";";
        sqlite3_exec(db_, set_version.c_str(), nullptr, nullptr, nullptr);
    }

    void Database::NormalizeNetZips()
    {
        std::vector<std::pair<std::int64_t, std::string>> fixes;
        {
            Statement select(db_, "SELECT id, default_location FROM nets;");
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
        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
        Statement update(db_, "UPDATE nets SET default_location = ? WHERE id = ?;");
        for (const std::pair<std::int64_t, std::string>& fix : fixes)
        {
            update.BindText(0, fix.second);
            update.BindInt64(1, fix.first);
            update.Step();
            update.Reset();
        }
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    }

    void Database::UpsertStation(const Station& station)
    {
        Statement statement(db_, R"sql(
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
        Statement statement(db_, R"sql(
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

    void Database::UpdateStationFields(const Station& station, std::int64_t updated_at)
    {
        Statement statement(db_, R"sql(
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
        Statement statement(db_, R"sql(
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

    std::vector<Station> Database::SearchStationsByCallsignSubstring(const std::string& substring)
    {
        Statement statement(db_, R"sql(
        SELECT callsign, name, member_id, street_address, city, county, state, zip,
               grid_square, license_class, email, data_source, last_updated
        FROM stations WHERE callsign LIKE '%' || ? || '%' ORDER BY callsign;
    )sql");
        statement.BindText(0, ToUpperAscii(substring));
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadStationRow(statement));
        }
        return stations;
    }

    std::vector<Station> Database::SearchNetStationsByCallsignSubstring(
        std::int64_t net_id, const std::string& substring)
    {
        Statement statement(db_, R"sql(
        SELECT DISTINCT s.callsign, s.name, s.member_id, s.street_address, s.city, s.county,
               s.state, s.zip, s.grid_square, s.license_class, s.email, s.data_source,
               s.last_updated
        FROM stations s
        WHERE s.callsign LIKE '%' || ? || '%'
          AND (
            EXISTS (SELECT 1 FROM check_ins c
                    JOIN net_instances ni ON ni.id = c.net_instance_id
                    WHERE ni.net_id = ? AND c.callsign = s.callsign)
            OR EXISTS (SELECT 1 FROM net_saved_stations ns
                       WHERE ns.net_id = ? AND ns.callsign = s.callsign)
          )
        ORDER BY s.callsign;
    )sql");
        statement.BindText(0, ToUpperAscii(substring));
        statement.BindInt64(1, net_id);
        statement.BindInt64(2, net_id);
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadStationRow(statement));
        }
        return stations;
    }

    void Database::SaveNetStation(std::int64_t net_id, const Station& station,
                                  const std::string& default_remarks, std::int64_t updated_at)
    {
        RecordManualCheckInStation(station, updated_at);

        Statement statement(db_, R"sql(
        INSERT INTO net_saved_stations (net_id, callsign, default_remarks)
        VALUES (?, ?, ?)
        ON CONFLICT(net_id, callsign) DO UPDATE SET default_remarks = excluded.default_remarks;
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(station.callsign));
        statement.BindText(2, default_remarks);
        statement.Step();
    }

    void Database::UpdateSavedNetStation(std::int64_t net_id, const Station& station,
                                         const std::string& default_remarks,
                                         std::int64_t updated_at)
    {
        UpdateStationFields(station, updated_at);

        Statement statement(db_, R"sql(
        UPDATE net_saved_stations SET default_remarks = ? WHERE net_id = ? AND callsign = ?;
    )sql");
        statement.BindText(0, default_remarks);
        statement.BindInt64(1, net_id);
        statement.BindText(2, ToUpperAscii(station.callsign));
        statement.Step();
    }

    void Database::RemoveSavedNetStation(std::int64_t net_id, const std::string& callsign)
    {
        Statement statement(db_, R"sql(
        DELETE FROM net_saved_stations WHERE net_id = ? AND callsign = ?;
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(callsign));
        statement.Step();
        DeleteUnusedStations();
    }

    bool Database::IsStationUsedOutsideNet(const std::string& callsign, std::int64_t net_id)
    {
        Statement statement(db_, R"sql(
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
        Statement statement(db_, R"sql(
        DELETE FROM stations
        WHERE NOT EXISTS (SELECT 1 FROM net_saved_stations s WHERE s.callsign = stations.callsign)
          AND NOT EXISTS (SELECT 1 FROM check_ins c WHERE c.callsign = stations.callsign);
    )sql");
        statement.Step();
        return sqlite3_changes(db_);
    }

    std::vector<Station> Database::GetSavedStationsForNet(std::int64_t net_id)
    {
        Statement statement(db_, R"sql(
        SELECT s.callsign, s.name, s.member_id, s.street_address, s.city, s.county, s.state,
               s.zip, s.grid_square, s.license_class, s.email, s.data_source, s.last_updated
        FROM stations s
        JOIN net_saved_stations ns ON ns.callsign = s.callsign
        WHERE ns.net_id = ?
        ORDER BY s.callsign;
    )sql");
        statement.BindInt64(0, net_id);
        std::vector<Station> stations;
        while (statement.Step())
        {
            stations.push_back(ReadStationRow(statement));
        }
        return stations;
    }

    std::string Database::GetSavedNetStationRemarks(std::int64_t net_id,
                                                    const std::string& callsign)
    {
        Statement statement(db_, R"sql(
        SELECT default_remarks FROM net_saved_stations WHERE net_id = ? AND callsign = ?;
    )sql");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(callsign));
        if (!statement.Step())
        {
            return "";
        }
        return statement.ColumnText(0);
    }

    std::int64_t Database::CreateNet(const Net& net)
    {
        Statement statement(db_, R"sql(
        INSERT INTO nets
            (name, mode, default_frequency, default_location,
             default_grid_square, recurrence_description, notes, created_at, imported_at,
             is_ad_hoc)
        VALUES (?,?,?,?,?,?,?,?,?,?);
    )sql");
        statement.BindText(0, net.name);
        statement.BindText(1, net.mode);
        statement.BindText(2, net.default_frequency);
        statement.BindText(3, net.default_location);
        statement.BindText(4, net.default_grid_square);
        statement.BindText(5, net.recurrence_description);
        statement.BindText(6, net.notes);
        statement.BindInt64(7, net.created_at);
        statement.BindInt64(8, net.imported_at);
        statement.BindInt64(9, net.is_ad_hoc ? 1 : 0);
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    void Database::UpdateNet(const Net& net)
    {
        Statement statement(db_, R"sql(
        UPDATE nets
        SET name = ?, mode = ?, default_frequency = ?, default_location = ?,
            recurrence_description = ?
        WHERE id = ?;
    )sql");
        statement.BindText(0, net.name);
        statement.BindText(1, net.mode);
        statement.BindText(2, net.default_frequency);
        statement.BindText(3, net.default_location);
        statement.BindText(4, net.recurrence_description);
        statement.BindInt64(5, net.id);
        statement.Step();
    }

    std::vector<Net> Database::GetAllNets()
    {
        Statement statement(db_, R"sql(
        SELECT id, name, mode, default_frequency, default_location,
               default_grid_square, recurrence_description, notes, created_at, imported_at,
               is_ad_hoc
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
        Statement statement(db_, R"sql(
        SELECT id, name, mode, default_frequency, default_location,
               default_grid_square, recurrence_description, notes, created_at, imported_at,
               is_ad_hoc
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
        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

        Statement delete_check_ins(db_, R"sql(
        DELETE FROM check_ins
        WHERE net_instance_id IN (SELECT id FROM net_instances WHERE net_id = ?);
    )sql");
        delete_check_ins.BindInt64(0, net_id);
        delete_check_ins.Step();

        Statement delete_instances(db_, "DELETE FROM net_instances WHERE net_id = ?;");
        delete_instances.BindInt64(0, net_id);
        delete_instances.Step();

        Statement delete_saved(db_, "DELETE FROM net_saved_stations WHERE net_id = ?;");
        delete_saved.BindInt64(0, net_id);
        delete_saved.Step();

        Statement delete_net(db_, "DELETE FROM nets WHERE id = ?;");
        delete_net.BindInt64(0, net_id);
        delete_net.Step();

        DeleteUnusedStations();
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    }

    std::int64_t Database::CreateNetInstance(const NetInstance& instance)
    {
        Statement statement(db_, R"sql(
        INSERT INTO net_instances
            (net_id, instance_date, net_control_callsign,
             alternate_net_control_callsign, logger_callsign, created_by,
             frequency, location, status, closed_at, operator_role, started_at)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?);
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
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    std::vector<NetInstance> Database::GetAdHocNetInstances()
    {
        Statement statement(db_, R"sql(
        SELECT i.id, i.net_id, i.instance_date, i.net_control_callsign,
               i.alternate_net_control_callsign, i.logger_callsign, i.created_by,
               i.frequency, i.location, i.status, i.closed_at, i.operator_role, i.started_at
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

    std::vector<NetInstance> Database::GetNetInstancesForNet(std::int64_t net_id)
    {
        Statement statement(db_, R"sql(
        SELECT id, net_id, instance_date, net_control_callsign,
               alternate_net_control_callsign, logger_callsign, created_by,
               frequency, location, status, closed_at, operator_role, started_at
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
        Statement statement(db_, R"sql(
        SELECT id, net_id, instance_date, net_control_callsign,
               alternate_net_control_callsign, logger_callsign, created_by,
               frequency, location, status, closed_at, operator_role, started_at
        FROM net_instances WHERE id = ?;
    )sql");
        statement.BindInt64(0, instance_id);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return ReadNetInstanceRow(statement);
    }

    std::vector<std::int64_t> Database::GetNetIdsWithOpenInstances()
    {
        Statement statement(
            db_, "SELECT DISTINCT net_id FROM net_instances WHERE status = ? ORDER BY net_id;");
        statement.BindInt64(0, static_cast<std::int64_t>(NetInstanceStatus::kOpen));
        std::vector<std::int64_t> net_ids;
        while (statement.Step())
        {
            net_ids.push_back(statement.ColumnInt64(0));
        }
        return net_ids;
    }

    bool Database::CloseNetInstance(std::int64_t instance_id, std::int64_t closed_at)
    {
        Statement statement(db_, R"sql(
        UPDATE net_instances SET status = ?, closed_at = ? WHERE id = ? AND status = ?;
    )sql");
        statement.BindInt64(0, static_cast<std::int64_t>(NetInstanceStatus::kClosed));
        statement.BindInt64(1, closed_at);
        statement.BindInt64(2, instance_id);
        statement.BindInt64(3, static_cast<std::int64_t>(NetInstanceStatus::kOpen));
        statement.Step();
        return sqlite3_changes(db_) > 0;
    }

    void Database::DeleteNetInstance(std::int64_t instance_id)
    {
        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

        Statement delete_check_ins(db_, "DELETE FROM check_ins WHERE net_instance_id = ?;");
        delete_check_ins.BindInt64(0, instance_id);
        delete_check_ins.Step();

        Statement delete_instance(db_, "DELETE FROM net_instances WHERE id = ?;");
        delete_instance.BindInt64(0, instance_id);
        delete_instance.Step();

        DeleteUnusedStations();
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    }

    void Database::SetNetInstanceRoleCallsign(std::int64_t instance_id, int role,
                                              const std::string& callsign)
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

    std::int64_t Database::AddCheckIn(const CheckIn& check_in)
    {
        Statement statement(db_, R"sql(
        INSERT INTO check_ins
            (net_instance_id, callsign, sequence_number, signal_report,
             remarks, comment, checked_in_at, designated_role)
        VALUES (?,?,?,?,?,?,?,?);
    )sql");
        statement.BindInt64(0, check_in.net_instance_id);
        statement.BindText(1, ToUpperAscii(check_in.callsign));
        statement.BindInt64(2, check_in.sequence_number);
        statement.BindText(3, check_in.signal_report);
        statement.BindText(4, check_in.remarks);
        statement.BindText(5, check_in.comment);
        statement.BindInt64(6, check_in.checked_in_at);
        statement.BindInt64(7, check_in.designated_role);
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    std::int64_t Database::AddCheckInAtNextSequence(const CheckIn& check_in)
    {
        Statement statement(db_, R"sql(
        INSERT INTO check_ins
            (net_instance_id, callsign, sequence_number, signal_report,
             remarks, comment, checked_in_at, designated_role)
        VALUES (?1, ?2,
                (SELECT COALESCE(MAX(sequence_number), 0) + 1 FROM check_ins
                 WHERE net_instance_id = ?1),
                ?3, ?4, ?5, ?6, ?7);
    )sql");
        statement.BindInt64(0, check_in.net_instance_id);
        statement.BindText(1, ToUpperAscii(check_in.callsign));
        statement.BindText(2, check_in.signal_report);
        statement.BindText(3, check_in.remarks);
        statement.BindText(4, check_in.comment);
        statement.BindInt64(5, check_in.checked_in_at);
        statement.BindInt64(6, check_in.designated_role);
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    void Database::GetCheckInSummary(std::int64_t net_instance_id, std::int64_t* count,
                                     std::int64_t* newest_id)
    {
        Statement statement(
            db_, "SELECT COUNT(*), COALESCE(MAX(id), 0) FROM check_ins WHERE net_instance_id = ?;");
        statement.BindInt64(0, net_instance_id);
        *count = 0;
        *newest_id = 0;
        if (statement.Step())
        {
            *count = statement.ColumnInt64(0);
            *newest_id = statement.ColumnInt64(1);
        }
    }

    // Rows of: net name, then kNetInstanceColumns, then kCheckInColumns.
    static std::vector<StationCheckInRecord> ReadStationCheckIns(Statement* statement)
    {
        std::vector<StationCheckInRecord> records;
        while (statement->Step())
        {
            StationCheckInRecord record;
            record.net_name = statement->ColumnText(0);
            record.net_is_ad_hoc = statement->ColumnInt64(1) != 0;
            record.instance = ReadNetInstanceColumns(*statement, 2);
            record.check_in = ReadCheckInColumns(*statement, 15);
            records.push_back(record);
        }
        return records;
    }

    std::vector<StationCheckInRecord> Database::GetStationCheckInsForNet(
        std::int64_t net_id, const std::string& callsign)
    {
        Statement statement(
            db_, std::string("SELECT n.name, n.is_ad_hoc, ") + kNetInstanceColumns + ", " +
                     kCheckInColumns +
                     " FROM check_ins c"
                     " JOIN net_instances i ON i.id = c.net_instance_id"
                     " JOIN nets n ON n.id = i.net_id"
                     " WHERE i.net_id = ? AND c.callsign = ?"
                     " ORDER BY i.instance_date DESC, i.started_at DESC, i.id DESC;");
        statement.BindInt64(0, net_id);
        statement.BindText(1, ToUpperAscii(callsign));
        return ReadStationCheckIns(&statement);
    }

    std::vector<StationCheckInRecord> Database::FindCheckInsByCallsign(const std::string& substring,
                                                                       int limit)
    {
        Statement statement(db_, std::string("SELECT n.name, n.is_ad_hoc, ") + kNetInstanceColumns +
                                     ", " + kCheckInColumns +
                                     " FROM check_ins c"
                                     " JOIN net_instances i ON i.id = c.net_instance_id"
                                     " JOIN nets n ON n.id = i.net_id"
                                     " WHERE c.callsign LIKE '%' || ? || '%'"
                                     " ORDER BY i.instance_date DESC, i.started_at DESC, i.id DESC"
                                     " LIMIT ?;");
        statement.BindText(0, ToUpperAscii(substring));
        statement.BindInt64(1, limit);
        return ReadStationCheckIns(&statement);
    }

    std::vector<CallsignTally> Database::GetTopCallsignsForNet(std::int64_t net_id, int limit)
    {
        Statement statement(db_, R"sql(
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
        Statement statement(db_, R"sql(
        SELECT s.callsign, COUNT(i.id), COALESCE(MAX(i.instance_date), '')
        FROM net_saved_stations s
        LEFT JOIN check_ins c ON c.callsign = s.callsign
        LEFT JOIN net_instances i ON i.id = c.net_instance_id AND i.net_id = s.net_id
        WHERE s.net_id = ?
        GROUP BY s.callsign
        ORDER BY COALESCE(MAX(i.instance_date), ''), s.callsign;
    )sql");
        statement.BindInt64(0, net_id);
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

    StationActivity Database::GetStationActivity(const std::string& callsign)
    {
        StationActivity activity;
        {
            Statement statement(db_, R"sql(
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
        Statement nets(db_, R"sql(
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
        Statement statement(db_, R"sql(
        SELECT id, net_instance_id, callsign, sequence_number, signal_report,
               remarks, comment, checked_in_at, designated_role
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

    void Database::UpdateCheckIn(const CheckIn& check_in)
    {
        Statement statement(db_, R"sql(
        UPDATE check_ins
        SET callsign = ?, sequence_number = ?, signal_report = ?,
            remarks = ?, comment = ?, checked_in_at = ?, designated_role = ?
        WHERE id = ?;
    )sql");
        statement.BindText(0, ToUpperAscii(check_in.callsign));
        statement.BindInt64(1, check_in.sequence_number);
        statement.BindText(2, check_in.signal_report);
        statement.BindText(3, check_in.remarks);
        statement.BindText(4, check_in.comment);
        statement.BindInt64(5, check_in.checked_in_at);
        statement.BindInt64(6, check_in.designated_role);
        statement.BindInt64(7, check_in.id);
        statement.Step();
    }

    void Database::DeleteCheckIn(std::int64_t check_in_id)
    {
        Statement statement(db_, "DELETE FROM check_ins WHERE id = ?;");
        statement.BindInt64(0, check_in_id);
        statement.Step();
        DeleteUnusedStations();
    }

    void Database::ClearCheckInRoleForInstance(std::int64_t net_instance_id, int role,
                                               std::int64_t except_check_in_id)
    {
        Statement statement(db_, R"sql(
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
        Statement statement(db_, R"sql(
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

    bool Database::TryClaimImportRun(const std::string& source, std::int64_t now,
                                     std::int64_t stale_after_seconds)
    {
        Statement statement(db_, R"sql(
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

    void Database::UpdateImportProgress(const std::string& source, const std::string& phase,
                                        int percent, std::int64_t records_imported,
                                        std::int64_t now)
    {
        Statement statement(db_, R"sql(
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
        Statement statement(db_, R"sql(
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
        Statement statement(db_, R"sql(
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

    void Database::BulkUpsertUlsStations(const std::vector<Station>& stations, std::size_t begin,
                                         std::size_t end, std::int64_t updated_at)
    {
        // The WHERE on the update skips rows whose data hasn't changed -- on a
        // weekly refresh that's nearly all of them, and an unchanged row then
        // costs a lookup instead of a rewrite of its table and index pages.
        // (So last_updated means "last changed", not "last seen".)
        Statement statement(db_, R"sql(
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

        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
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
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    }

    std::vector<NearbyUlsStation> Database::SearchNearbyUlsStations(
        const std::string& substring, const std::vector<NearbyZip>& nearby_zips,
        const std::vector<std::string>& zip3_prefixes, int limit)
    {
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
            "SELECT u.*, n.miles AS miles FROM nearby n JOIN uls_stations u ON u.zip = n.zip "
            "WHERE u.callsign LIKE '%' || ? || '%'";
        if (!zip3_prefixes.empty())
        {
            sql +=
                " UNION ALL SELECT u.*, NULL AS miles FROM uls_stations u "
                "WHERE u.callsign LIKE '%' || ? || '%' AND (";
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
            station.data_source = StationDataSource::kUls;
            found.miles = statement.ColumnDouble(8);
            results.push_back(std::move(found));
        }
        return results;
    }

    std::optional<Station> Database::FindUlsStationByCallsign(const std::string& callsign)
    {
        Statement statement(db_, R"sql(
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
        station.data_source = StationDataSource::kUls;
        return station;
    }

    void Database::BulkUpsertZipCentroids(const std::vector<ZipCentroid>& batch)
    {
        Statement statement(db_, R"sql(
        INSERT INTO zip_centroids (zip, lat, lon) VALUES (?, ?, ?)
        ON CONFLICT(zip) DO UPDATE SET lat = excluded.lat, lon = excluded.lon;
    )sql");

        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
        for (const ZipCentroid& centroid : batch)
        {
            statement.Reset();
            statement.BindText(0, centroid.zip);
            statement.BindDouble(1, centroid.lat);
            statement.BindDouble(2, centroid.lon);
            statement.Step();
        }
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    }

    bool Database::HasAnyZipCentroids()
    {
        Statement statement(db_, "SELECT EXISTS(SELECT 1 FROM zip_centroids LIMIT 1);");
        statement.Step();
        return statement.ColumnInt64(0) != 0;
    }

    std::vector<ZipCentroid> Database::GetAllZipCentroids()
    {
        Statement statement(db_, "SELECT zip, lat, lon FROM zip_centroids;");
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

    int Database::DeleteUlsStationsNotIn(const std::vector<Station>& current)
    {
        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_,
                     "CREATE TEMP TABLE IF NOT EXISTS current_uls_callsigns "
                     "(callsign TEXT PRIMARY KEY);"
                     "DELETE FROM current_uls_callsigns;",
                     nullptr, nullptr, nullptr);
        {
            Statement insert(db_,
                             "INSERT OR IGNORE INTO current_uls_callsigns (callsign) VALUES (?);");
            for (const Station& station : current)
            {
                insert.Reset();
                insert.BindText(0, ToUpperAscii(station.callsign));
                insert.Step();
            }
        }
        sqlite3_exec(db_,
                     "DELETE FROM uls_stations WHERE callsign NOT IN "
                     "(SELECT callsign FROM current_uls_callsigns);",
                     nullptr, nullptr, nullptr);
        int deleted = sqlite3_changes(db_);
        sqlite3_exec(db_, "DROP TABLE current_uls_callsigns;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        return deleted;
    }

    void Database::ReplaceZipCountyData(const std::vector<ZipCounty>& zip_counties,
                                        const std::vector<ZipPlaceCounty>& zip_place_counties)
    {
        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "DELETE FROM zip_counties;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "DELETE FROM zip_place_counties;", nullptr, nullptr, nullptr);

        Statement insert_zip(db_, "INSERT INTO zip_counties (zip, county) VALUES (?, ?);");
        for (const ZipCounty& zip_county : zip_counties)
        {
            insert_zip.Reset();
            insert_zip.BindText(0, zip_county.zip);
            insert_zip.BindText(1, zip_county.county);
            insert_zip.Step();
        }

        Statement insert_place(db_, R"sql(
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
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    }

    std::vector<ZipCounty> Database::GetAllZipCounties()
    {
        Statement statement(db_, "SELECT zip, county FROM zip_counties;");
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
        Statement statement(db_, "SELECT zip, place, county FROM zip_place_counties;");
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

    bool Database::HasAnyUlsStations()
    {
        Statement statement(db_, "SELECT EXISTS(SELECT 1 FROM uls_stations LIMIT 1);");
        statement.Step();
        return statement.ColumnInt64(0) != 0;
    }

    void Database::CreateUser(const User& user)
    {
        Statement statement(db_, R"sql(
        INSERT INTO users (username, public_key, created_at, last_login_at)
        VALUES (?,?,?,0)
        ON CONFLICT(username) DO UPDATE SET public_key = excluded.public_key;
    )sql");
        statement.BindText(0, user.username);
        statement.BindText(1, user.public_key);
        statement.BindInt64(2, user.created_at);
        statement.Step();
    }

    std::optional<User> Database::GetUserByUsername(const std::string& username)
    {
        Statement statement(db_, R"sql(
        SELECT username, public_key, created_at, last_login_at
        FROM users WHERE username = ?;
    )sql");
        statement.BindText(0, username);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        User user;
        user.username = statement.ColumnText(0);
        user.public_key = statement.ColumnText(1);
        user.created_at = statement.ColumnInt64(2);
        user.last_login_at = statement.ColumnInt64(3);
        return user;
    }

    std::vector<User> Database::ListUsers()
    {
        Statement statement(db_, R"sql(
        SELECT username, public_key, created_at, last_login_at
        FROM users ORDER BY username COLLATE NOCASE;
    )sql");
        std::vector<User> users;
        while (statement.Step())
        {
            User user;
            user.username = statement.ColumnText(0);
            user.public_key = statement.ColumnText(1);
            user.created_at = statement.ColumnInt64(2);
            user.last_login_at = statement.ColumnInt64(3);
            users.push_back(std::move(user));
        }
        return users;
    }

    void Database::DeleteUser(const std::string& username)
    {
        Statement statement(db_, "DELETE FROM users WHERE username = ?;");
        statement.BindText(0, username);
        statement.Step();
    }

    void Database::UpdateUserLastLogin(const std::string& username, std::int64_t last_login_at)
    {
        Statement statement(db_, "UPDATE users SET last_login_at = ? WHERE username = ?;");
        statement.BindInt64(0, last_login_at);
        statement.BindText(1, username);
        statement.Step();
    }

}  // namespace ql
