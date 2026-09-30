#include "date_utils.hpp"

#include <cstdio>
#include <ctime>

namespace ql
{

    std::string CurrentDateIso8601()
    {
        std::tm local_time = LocalTime(std::time(nullptr));

        char buffer[11];  // "YYYY-MM-DD" + '\0'
        int length =
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", local_time.tm_year + 1900,
                          local_time.tm_mon + 1, local_time.tm_mday);
        // The length is known: no strlen to find it again.
        return std::string(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
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
        std::size_t length = std::strftime(buffer, sizeof(buffer),
                                           g_use_24_hour_clock ? "%H:%M" : "%I:%M %p", &local_time);
        return std::string(buffer, length);
    }

    std::string FormatLocalDateTime(std::int64_t unix_time)
    {
        if (unix_time <= 0)
        {
            return "";
        }
        std::string text = FormatLocalDate(unix_time);
        text += ' ';
        text += FormatLocalTimeOfDay(unix_time);
        return text;
    }

    std::string FormatLocalDate(std::int64_t unix_time)
    {
        if (unix_time <= 0)
        {
            return "";
        }
        std::tm local_time = LocalTime(static_cast<std::time_t>(unix_time));
        char buffer[16];
        std::size_t length = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local_time);
        return std::string(buffer, length);
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
