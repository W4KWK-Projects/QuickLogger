#include "database.hpp"

#include <sqlite3.h>

#include <stdexcept>

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
    notes TEXT NOT NULL DEFAULT ''
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
    operator_role INTEGER NOT NULL DEFAULT 0
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
)sql";

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
        return net;
    }

    static NetInstance ReadNetInstanceRow(const Statement& row)
    {
        NetInstance instance;
        instance.id = row.ColumnInt64(0);
        instance.net_id = row.ColumnInt64(1);
        instance.instance_date = row.ColumnText(2);
        instance.net_control_callsign = row.ColumnText(3);
        instance.alternate_net_control_callsign = row.ColumnText(4);
        instance.logger_callsign = row.ColumnText(5);
        instance.created_by = row.ColumnText(6);
        instance.frequency = row.ColumnText(7);
        instance.location = row.ColumnText(8);
        instance.status = static_cast<NetInstanceStatus>(row.ColumnInt64(9));
        instance.closed_at = row.ColumnInt64(10);
        instance.operator_role = static_cast<int>(row.ColumnInt64(11));
        return instance;
    }

    static CheckIn ReadCheckInRow(const Statement& row)
    {
        CheckIn check_in;
        check_in.id = row.ColumnInt64(0);
        check_in.net_instance_id = row.ColumnInt64(1);
        check_in.callsign = row.ColumnText(2);
        check_in.sequence_number = static_cast<int>(row.ColumnInt64(3));
        check_in.signal_report = row.ColumnText(4);
        check_in.remarks = row.ColumnText(5);
        check_in.comment = row.ColumnText(6);
        check_in.checked_in_at = row.ColumnInt64(7);
        check_in.designated_role = static_cast<int>(row.ColumnInt64(8));
        return check_in;
    }

    Database::Database(const std::string& path)
    {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
        {
            std::string message = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            throw std::runtime_error("Failed to open database '" + path + "': " + message);
        }
        sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
        // WAL lets the UI thread's reads proceed while a background import
        // (see uls_import.hpp) holds a writer transaction open on a second
        // connection to this same file.
        sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
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

        EnsureColumnExists(db_, "stations", "street_address", "TEXT NOT NULL DEFAULT ''");
        EnsureColumnExists(db_, "net_saved_stations", "default_remarks",
                           "TEXT NOT NULL DEFAULT ''");
        EnsureColumnExists(db_, "net_instances", "operator_role", "INTEGER NOT NULL DEFAULT 0");
        EnsureColumnExists(db_, "check_ins", "designated_role", "INTEGER NOT NULL DEFAULT -1");

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
    }

    bool Database::DeleteStationCompletely(const std::string& callsign)
    {
        std::string upper_callsign = ToUpperAscii(callsign);

        Statement count_statement(db_, "SELECT COUNT(*) FROM check_ins WHERE callsign = ?;");
        count_statement.BindText(0, upper_callsign);
        count_statement.Step();
        if (count_statement.ColumnInt64(0) > 0)
        {
            return false;
        }

        Statement delete_saved(db_, "DELETE FROM net_saved_stations WHERE callsign = ?;");
        delete_saved.BindText(0, upper_callsign);
        delete_saved.Step();

        Statement delete_station(db_, "DELETE FROM stations WHERE callsign = ?;");
        delete_station.BindText(0, upper_callsign);
        delete_station.Step();
        return true;
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
             default_grid_square, recurrence_description, notes)
        VALUES (?,?,?,?,?,?,?);
    )sql");
        statement.BindText(0, net.name);
        statement.BindText(1, net.mode);
        statement.BindText(2, net.default_frequency);
        statement.BindText(3, net.default_location);
        statement.BindText(4, net.default_grid_square);
        statement.BindText(5, net.recurrence_description);
        statement.BindText(6, net.notes);
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
               default_grid_square, recurrence_description, notes
        FROM nets ORDER BY name;
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
               default_grid_square, recurrence_description, notes
        FROM nets WHERE id = ?;
    )sql");
        statement.BindInt64(0, net_id);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return ReadNetRow(statement);
    }

    std::int64_t Database::CreateNetInstance(const NetInstance& instance)
    {
        Statement statement(db_, R"sql(
        INSERT INTO net_instances
            (net_id, instance_date, net_control_callsign,
             alternate_net_control_callsign, logger_callsign, created_by,
             frequency, location, status, closed_at, operator_role)
        VALUES (?,?,?,?,?,?,?,?,?,?,?);
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
        statement.Step();
        return sqlite3_last_insert_rowid(db_);
    }

    std::vector<NetInstance> Database::GetNetInstancesForNet(std::int64_t net_id)
    {
        Statement statement(db_, R"sql(
        SELECT id, net_id, instance_date, net_control_callsign,
               alternate_net_control_callsign, logger_callsign, created_by,
               frequency, location, status, closed_at, operator_role
        FROM net_instances WHERE net_id = ? ORDER BY instance_date DESC;
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
               frequency, location, status, closed_at, operator_role
        FROM net_instances WHERE id = ?;
    )sql");
        statement.BindInt64(0, instance_id);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        return ReadNetInstanceRow(statement);
    }

    void Database::CloseNetInstance(std::int64_t instance_id, std::int64_t closed_at)
    {
        Statement statement(db_, R"sql(
        UPDATE net_instances SET status = ?, closed_at = ? WHERE id = ?;
    )sql");
        statement.BindInt64(0, static_cast<std::int64_t>(NetInstanceStatus::kClosed));
        statement.BindInt64(1, closed_at);
        statement.BindInt64(2, instance_id);
        statement.Step();
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
        SELECT source, status, started_at, completed_at, records_imported, last_error
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
        return result;
    }

    void Database::UpsertImportRunStatus(const ImportRunStatus& status)
    {
        Statement statement(db_, R"sql(
        INSERT INTO import_runs (source, status, started_at, completed_at, records_imported, last_error)
        VALUES (?,?,?,?,?,?)
        ON CONFLICT(source) DO UPDATE SET
            status = excluded.status,
            started_at = excluded.started_at,
            completed_at = excluded.completed_at,
            records_imported = excluded.records_imported,
            last_error = excluded.last_error;
    )sql");
        statement.BindText(0, status.source);
        statement.BindText(1, status.status);
        statement.BindInt64(2, status.started_at);
        statement.BindInt64(3, status.completed_at);
        statement.BindInt64(4, status.records_imported);
        statement.BindText(5, status.last_error);
        statement.Step();
    }

    void Database::BulkUpsertUlsStations(const std::vector<Station>& batch, std::int64_t updated_at)
    {
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
            last_updated = excluded.last_updated;
    )sql");

        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
        for (const Station& station : batch)
        {
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

    std::vector<Station> Database::SearchUlsStationsByCallsignAndZip3Prefixes(
        const std::string& substring, const std::vector<std::string>& zip3_prefixes)
    {
        std::vector<Station> results;
        if (zip3_prefixes.empty())
        {
            return results;
        }

        std::string sql =
            "SELECT callsign, name, street_address, city, state, zip, license_class, "
            "last_updated FROM uls_stations WHERE callsign LIKE '%' || ? || '%' AND (";
        for (std::size_t i = 0; i < zip3_prefixes.size(); ++i)
        {
            if (i > 0)
            {
                sql += " OR ";
            }
            sql += "zip LIKE ? || '%'";
        }
        sql += ") ORDER BY callsign LIMIT 50;";

        Statement statement(db_, sql);
        statement.BindText(0, ToUpperAscii(substring));
        for (std::size_t i = 0; i < zip3_prefixes.size(); ++i)
        {
            statement.BindText(static_cast<int>(i) + 1, zip3_prefixes[i]);
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
            station.data_source = StationDataSource::kUls;
            results.push_back(station);
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
            results.push_back(centroid);
        }
        return results;
    }

}  // namespace ql
