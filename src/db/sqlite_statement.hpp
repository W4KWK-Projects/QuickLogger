#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

struct sqlite3;
struct sqlite3_stmt;

namespace ql
{

    // A connection's prepared statements, kept for reuse so each SQL text is
    // parsed and planned once per connection rather than on every call (see
    // Statement's cached constructor). Keyed by the address of the SQL
    // string literal: the same call site always passes the same address, so
    // no SQL text is hashed or compared. Owned by Database, which calls
    // Clear() before closing the connection.
    class StatementCache
    {
    public:
        StatementCache() = default;
        ~StatementCache();

        StatementCache(const StatementCache&) = delete;
        StatementCache& operator=(const StatementCache&) = delete;

        // The connection statements are prepared on. Set once, after the
        // connection is opened.
        void Attach(sqlite3* db);
        [[nodiscard]] sqlite3* db() const
        {
            return db_;
        }

        // Finalizes every cached statement (none may be in use).
        void Clear();

    private:
        friend class Statement;

        struct Entry
        {
            sqlite3_stmt* stmt = nullptr;
            bool in_use = false;
        };

        sqlite3* db_ = nullptr;
        std::unordered_map<const char*, Entry> entries_;
    };

    // RAII wrapper around a single prepared sqlite3 statement.
    //
    // Exists so Database's methods don't each have to hand-roll
    // prepare/bind/step/finalize and its error handling. Column/parameter
    // indices are 0-based to match sqlite3_bind_*'s 1-based convention being
    // hidden from callers (this class adds 1 internally).
    class Statement
    {
    public:
        // A statement from `cache`, for SQL that's a string literal (so its
        // address identifies it). Prepared the first time, then reused:
        // going out of scope resets it (ending its implicit read or write
        // transaction, so no lock is held) instead of finalizing it. If
        // that same statement is already in use further up the stack, a
        // fresh one is prepared just for this use.
        Statement(StatementCache* cache, const char* sql);
        // A one-off statement, for SQL built at run time; finalized when it
        // goes out of scope.
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

        [[nodiscard]] std::string ColumnText(int index) const;
        [[nodiscard]] std::int64_t ColumnInt64(int index) const;
        [[nodiscard]] double ColumnDouble(int index) const;

    private:
        sqlite3_stmt* stmt_ = nullptr;
        // Set when stmt_ belongs to a StatementCache, which keeps it.
        StatementCache::Entry* cached_ = nullptr;
    };

}  // namespace ql
