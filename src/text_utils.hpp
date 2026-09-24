#pragma once

#include <string>

namespace ql
{

    // Uppercases ASCII letters only (callsigns are always ASCII). Shared by
    // Database (so every callsign is normalized at the SQL boundary regardless
    // of how it got there) and the UI layer (so a callsign Input shows upper
    // case as the operator types, rather than only once saved).
    std::string ToUpperAscii(const std::string& value);

    // A callsign as typed, reduced to what a callsign can contain: ASCII
    // letters (uppercased), digits and '/' (for portable/prefix forms like
    // "W4KWK/M" or "VE3/W4KWK"). Everything else -- a stray space, a pasted
    // tab, punctuation -- is dropped, so "w4kwk " and "W4KWK" are the same
    // station rather than two.
    std::string NormalizeCallsign(const std::string& value);

    // A place name reduced to a form that compares equal however it was
    // written: uppercase, periods dropped, and the accented letters Census
    // uses (e.g. Puerto Rico's "Loíza") folded to plain ASCII the way FCC
    // and USPS spell them ("LOIZA"). Used to match a station's city against
    // Census town names -- see ZipPlaceCounty.
    std::string NormalizePlaceName(const std::string& value);

    // True if `value` is exactly five ASCII digits -- the form a ZIP code
    // takes everywhere QuickLogger asks for one (the operator's home ZIP in
    // Settings, a net's ZIP).
    bool IsFiveDigitZip(const std::string& value);

    // The ZIP code in free text such as "Chattanooga, TN 37415-2623": the
    // last run of exactly five digits, or the first five of a run of nine (a
    // ZIP+4 written without its dash). Empty if there's none -- "Hamilton
    // County" or "1234 Main St" have no ZIP.
    std::string ExtractZipCode(const std::string& text);

}  // namespace ql
