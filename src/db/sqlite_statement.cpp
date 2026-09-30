#include "sqlite_statement.hpp"

#include <sqlite3.h>

#include <stdexcept>

namespace ql
{

    // Prepares `sql` (`bytes` long, or -1 if NUL-terminated) on `db`, marked
    // persistent when it's to be kept for reuse. Throws on failure.
    static sqlite3_stmt* PrepareStatement(sqlite3* db, const char* sql, int bytes, bool persistent)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v3(db, sql, bytes, persistent ? SQLITE_PREPARE_PERSISTENT : 0, &stmt,
                               nullptr) != SQLITE_OK)
        {
            throw std::runtime_error(std::string("Failed to prepare statement: ") +
                                     sqlite3_errmsg(db));
        }
        return stmt;
    }

    StatementCache::~StatementCache()
    {
        Clear();
    }

    void StatementCache::Attach(sqlite3* db)
    {
        db_ = db;
    }

    void StatementCache::Clear()
    {
        for (std::pair<const char* const, Entry>& entry : entries_)
        {
            sqlite3_finalize(entry.second.stmt);
        }
        entries_.clear();
    }

    Statement::Statement(StatementCache* cache, const char* sql)
    {
        std::pair<std::unordered_map<const char*, StatementCache::Entry>::iterator, bool> found =
            cache->entries_.try_emplace(sql);
        StatementCache::Entry& entry = found.first->second;
        if (found.second)
        {
            try
            {
                entry.stmt = PrepareStatement(cache->db_, sql, -1, true);
            }
            catch (...)
            {
                cache->entries_.erase(found.first);
                throw;
            }
        }
        else if (entry.in_use)
        {
            // Nested use of the same statement: a one-off copy.
            stmt_ = PrepareStatement(cache->db_, sql, -1, false);
            return;
        }
        entry.in_use = true;
        stmt_ = entry.stmt;
        cached_ = &entry;
    }

    Statement::Statement(sqlite3* db, const std::string& sql)
        : stmt_(PrepareStatement(db, sql.c_str(), static_cast<int>(sql.size()) + 1, false))
    {
    }

    Statement::~Statement()
    {
        if (cached_ == nullptr)
        {
            sqlite3_finalize(stmt_);
            return;
        }
        // Ready for the next use, and not holding its transaction open.
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
        cached_->in_use = false;
    }

    void Statement::BindText(int index, const std::string& value)
    {
        sqlite3_bind_text(stmt_, index + 1, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT);
    }

    void Statement::BindInt64(int index, std::int64_t value)
    {
        sqlite3_bind_int64(stmt_, index + 1, value);
    }

    void Statement::BindDouble(int index, double value)
    {
        sqlite3_bind_double(stmt_, index + 1, value);
    }

    bool Statement::Step()
    {
        int result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW)
        {
            return true;
        }
        if (result == SQLITE_DONE)
        {
            return false;
        }
        throw std::runtime_error(std::string("Failed to step statement: ") +
                                 sqlite3_errmsg(sqlite3_db_handle(stmt_)));
    }

    void Statement::Reset()
    {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }

    std::string Statement::ColumnText(int index) const
    {
        const unsigned char* text = sqlite3_column_text(stmt_, index);
        if (text == nullptr)
        {
            return std::string();
        }
        // SQLite already knows the length (sqlite3_column_bytes, valid right
        // after sqlite3_column_text), so build the string from it rather than
        // making std::string scan for the terminator with strlen.
        return std::string(reinterpret_cast<const char*>(text),
                           static_cast<std::size_t>(sqlite3_column_bytes(stmt_, index)));
    }

    std::int64_t Statement::ColumnInt64(int index) const
    {
        return sqlite3_column_int64(stmt_, index);
    }

    double Statement::ColumnDouble(int index) const
    {
        return sqlite3_column_double(stmt_, index);
    }

}  // namespace ql
