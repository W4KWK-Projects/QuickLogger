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

    // Whether times are shown on the 24-hour clock ("15:42") rather than the
    // 12-hour one ("03:42 PM", the default). Set from the operator's
    // settings (AppSettings::use_24_hour_clock) when their session starts
    // and whenever they save Settings; every time shown or exported goes
    // through the formatters below. One setting per process, which is one
    // per session (each SSH session is its own process).
    void SetUse24HourClock(bool use_24_hour_clock);
    bool Use24HourClock();

    // The local time of day of a Unix timestamp as "03:42 PM" (or "15:42",
    // see SetUse24HourClock). Returns an empty string for a timestamp of 0 or
    // less, which is how "not recorded" is stored (see
    // NetInstance::started_at).
    std::string FormatLocalTimeOfDay(std::int64_t unix_time);

    // The local date and time of a Unix timestamp as "2026-09-24 03:42 PM"
    // (or "2026-09-24 15:42"), or an empty string for 0 or less.
    std::string FormatLocalDateTime(std::int64_t unix_time);

    // The local date of a Unix timestamp as "2026-09-24" (the same form as
    // CurrentDateIso8601), or an empty string for 0 or less ("not recorded").
    std::string FormatLocalDate(std::int64_t unix_time);

}  // namespace ql
