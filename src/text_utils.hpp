#pragma once

#include <string>

namespace ql
{

    // Uppercases ASCII letters only (callsigns are always ASCII). Shared by
    // Database (so every callsign is normalized at the SQL boundary regardless
    // of how it got there) and the UI layer (so a callsign Input shows upper
    // case as the operator types, rather than only once saved).
    std::string ToUpperAscii(const std::string& value);

}  // namespace ql
