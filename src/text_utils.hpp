#pragma once

#include <string>

namespace ql
{

    // Uppercases ASCII letters only (callsigns are always ASCII). Shared by
    // Database (so every callsign is normalized at the SQL boundary regardless
    // of how it got there) and the UI layer (so a callsign Input shows upper
    // case as the operator types, rather than only once saved).
    std::string ToUpperAscii(const std::string& value);

    // A place name reduced to a form that compares equal however it was
    // written: uppercase, periods dropped, and the accented letters Census
    // uses (e.g. Puerto Rico's "Loíza") folded to plain ASCII the way FCC
    // and USPS spell them ("LOIZA"). Used to match a station's city against
    // Census town names -- see ZipPlaceCounty.
    std::string NormalizePlaceName(const std::string& value);

}  // namespace ql
