#include "sqlite_statement.hpp"

#include <sqlite3.h>

#include <stdexcept>

namespace ql
{

    Statement::Statement(sqlite3* db, const std::string& sql)
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()) + 1, &stmt_,
                               nullptr) != SQLITE_OK)
        {
            throw std::runtime_error(std::string("Failed to prepare statement: ") +
                                     sqlite3_errmsg(db));
        }
    }

    Statement::~Statement()
    {
        sqlite3_finalize(stmt_);
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
        return text == nullptr ? std::string() : reinterpret_cast<const char*>(text);
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
