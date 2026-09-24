#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace ql
{

    // Today's date in the system's local time zone, as "YYYY-MM-DD".
    std::string CurrentDateIso8601();

    // Broken-down local time for `time_value`. A portable, thread-safe
    // wrapper: POSIX systems have localtime_r() but Windows has localtime_s()
    // (with its arguments in the opposite order), and plain std::localtime
    // returns a pointer into a shared static buffer.
    std::tm LocalTime(std::time_t time_value);

    // The local time of day of a Unix timestamp as "03:42 PM", in the same
    // 12-hour style as the other timestamps shown around the app. Returns an
    // empty string for a timestamp of 0 or less, which is how "not recorded"
    // is stored (see NetInstance::started_at).
    std::string FormatLocalTimeOfDay(std::int64_t unix_time);

    // The local date of a Unix timestamp as "2026-09-24" (the same form as
    // CurrentDateIso8601), or an empty string for 0 or less ("not recorded").
    std::string FormatLocalDate(std::int64_t unix_time);

}  // namespace ql
