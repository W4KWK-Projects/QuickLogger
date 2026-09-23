#pragma once

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

}  // namespace ql
