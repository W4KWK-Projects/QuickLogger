#include "date_utils.hpp"

#include <cstdio>
#include <ctime>

namespace ql
{

    std::string CurrentDateIso8601()
    {
        std::time_t now = std::time(nullptr);
        std::tm* local_time = std::localtime(&now);

        char buffer[11];  // "YYYY-MM-DD" + '\0'
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", local_time->tm_year + 1900,
                      local_time->tm_mon + 1, local_time->tm_mday);
        return std::string(buffer);
    }

}  // namespace ql
