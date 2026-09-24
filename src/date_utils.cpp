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

    static bool g_use_24_hour_clock = false;

    void SetUse24HourClock(bool use_24_hour_clock)
    {
        g_use_24_hour_clock = use_24_hour_clock;
    }

    bool Use24HourClock()
    {
        return g_use_24_hour_clock;
    }

    std::string FormatLocalTimeOfDay(std::int64_t unix_time)
    {
        if (unix_time <= 0)
        {
            return "";
        }
        std::tm local_time = LocalTime(static_cast<std::time_t>(unix_time));
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), g_use_24_hour_clock ? "%H:%M" : "%I:%M %p",
                      &local_time);
        return std::string(buffer);
    }

    std::string FormatLocalDateTime(std::int64_t unix_time)
    {
        if (unix_time <= 0)
        {
            return "";
        }
        return FormatLocalDate(unix_time) + " " + FormatLocalTimeOfDay(unix_time);
    }

    std::string FormatLocalDate(std::int64_t unix_time)
    {
        if (unix_time <= 0)
        {
            return "";
        }
        std::tm local_time = LocalTime(static_cast<std::time_t>(unix_time));
        char buffer[16];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local_time);
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
