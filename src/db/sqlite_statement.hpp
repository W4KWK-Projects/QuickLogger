#pragma once

#include <cstdint>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace ql
{

    // RAII wrapper around a single prepared sqlite3 statement.
    //
    // Exists so Database's methods don't each have to hand-roll
    // prepare/bind/step/finalize and its error handling. Column/parameter
    // indices are 0-based to match sqlite3_bind_*'s 1-based convention being
    // hidden from callers (this class adds 1 internally).
    class Statement
    {
    public:
        Statement(sqlite3* db, const std::string& sql);
        ~Statement();

        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        void BindText(int index, const std::string& value);
        void BindInt64(int index, std::int64_t value);
        void BindDouble(int index, double value);

        // Advances to the next result row. Returns false once there are no
        // more rows (including for statements that never produce rows).
        bool Step();

        // Clears bindings and rewinds to before the first row, so the same
        // prepared statement can be Step()'d again with new BindText/BindInt64
        // calls instead of re-preparing the SQL text. Used by bulk-insert
        // loops (e.g. Database::BulkUpsertStationsFromUls) to avoid
        // re-parsing the same INSERT statement once per row.
        void Reset();

        std::string ColumnText(int index) const;
        std::int64_t ColumnInt64(int index) const;
        double ColumnDouble(int index) const;

    private:
        sqlite3_stmt* stmt_ = nullptr;
    };

}  // namespace ql
