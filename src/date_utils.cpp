#include "date_utils.hpp"

#include <cstdio>
#include <ctime>

namespace ql
{

    std::string CurrentDateIso8601()
    {
        std::tm local_time = LocalTime(std::time(nullptr));

        char buffer[11];  // "YYYY-MM-DD" + '\0'
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", local_time.tm_year + 1900,
                      local_time.tm_mon + 1, local_time.tm_mday);
        return std::string(buffer);
    }

    std::tm LocalTime(std::time_t time_value)
    {
        std::tm result{};
#if defined(_WIN32)
        localtime_s(&result, &time_value);
#else
        localtime_r(&time_value, &result);
#endif
        return result;
    }

}  // namespace ql
