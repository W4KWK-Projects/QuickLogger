#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "models.hpp"

namespace ql
{

    // One check-in for an ADIF export, with what's known about its station
    // (null if nothing is): pointers to the caller's, not copies, which
    // must outlive BuildAdif.
    struct AdifContact
    {
        const CheckIn* check_in = nullptr;
        const Station* station = nullptr;
    };

    // One session's contacts as an ADIF 3.1 file (.adi), for loading into a
    // logging program: one record per contact, with CALL, QSO_DATE and
    // TIME_ON (UTC, from when they checked in), FREQ and BAND (from
    // `frequency`, the session's or its net's, in MHz), MODE and SUBMODE
    // (from the net's `mode`; see AdifMode), STATION_CALLSIGN
    // (`station_callsign`, the exporting operator's), RST_RCVD (the signal
    // report), NAME, QTH (the city), STATE, CNTY ("TN,Hamilton"),
    // GRIDSQUARE, COMMENT (the remarks) and NOTES (the check-in's comment).
    // Empty fields are left out. ADIF is ASCII only, so accented letters
    // lose their accents (see FoldToAscii). `created_at` is the header's
    // CREATED_TIMESTAMP. The contacts are given in order, and the caller
    // leaves out the operator's own check-in.
    std::string BuildAdif(const std::vector<AdifContact>& contacts, const std::string& mode,
                          const std::string& frequency, const std::string& station_callsign,
                          const std::string& session_date, std::int64_t created_at);

    // `name` as first name first, for ADIF's NAME: the FCC's "Last, First
    // Middle" ("Shults, Roger D", "Stansberry Jr, Roger L") becomes "Roger
    // D Shults" and "Roger L Stansberry Jr". A name with no comma, or more
    // than one, is left as it is.
    std::string FirstNameFirst(const std::string& name);

    // ADIF's BAND for a frequency in MHz ("146.940" is "2m"), or "" if it's
    // in none of them.
    std::string AdifBand(const std::string& frequency);

    // UTF-8 `text` in plain ASCII: accented Latin letters without their
    // accents ("é" is "e", "ß" is "ss"), curly quotes and dashes as plain
    // ones, and anything else left out.
    std::string FoldToAscii(std::string_view text);

}  // namespace ql
