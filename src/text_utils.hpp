#pragma once

#include <cstddef>
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

    // NormalizeCallsign, but a "?" is kept: the callsign field as typed,
    // where a "?" turns on wildcard matching (see IsWildcardCallsign).
    std::string NormalizeTypedCallsign(const std::string& value);

    // True if what was typed in a callsign field has a "?": the operator's
    // sign that they're unsure of some of it. Autocomplete then matches
    // every other character typed, in that order, with anything between
    // them, wherever the "?" is: "?4VW", "4VW?" and "4?V?W" all find
    // KQ4EVW. The "?" itself matches nothing. Without one, what's typed
    // must be a contiguous part of the callsign.
    bool IsWildcardCallsign(const std::string& typed);

    // True if wildcard matching has enough to go on: a "?" and at least two
    // other characters (a lone "4?" would match half the licensees).
    bool WildcardHasEnough(const std::string& typed);

    // The SQL LIKE pattern for wildcard matching: the characters typed with
    // "%" between them, "%4%V%W%". With `anchored` the first must start the
    // callsign ("V%E%3%"), as a prefix match does.
    std::string WildcardLikePattern(const std::string& typed, bool anchored);

    // How many characters of the `length` bytes at `callsign` the typed
    // characters `letters` (NormalizeCallsign of what was typed) span, in
    // order, from the first one matched to the last, taking the tightest
    // place they fit; -1 if they don't fit in order. A contiguous match
    // spans as many characters as were typed, so a smaller span is a better
    // match. With `anchored` the first must be the callsign's first.
    int WildcardSpan(const std::string& letters, const char* callsign, std::size_t length, bool anchored);

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

    // True if `value` is a Canadian postal code, "K1A 0B1" or "K1A0B1" in
    // either case, or just its first three characters (the FSA, "K1A").
    bool IsCanadianPostalCode(const std::string& value);

    // True if `value` is a 5-digit ZIP or a Canadian postal code: what
    // Settings and a net's location accept.
    bool IsZipOrPostalCode(const std::string& value);

    // `value` as it's stored: a Canadian postal code upper-cased with its
    // space ("k1a0b1" -> "K1A 0B1"); anything else unchanged.
    std::string NormalizeZipOrPostalCode(const std::string& value);

    // The ZIP code in free text such as "Chattanooga, TN 37415-2623": the
    // last run of exactly five digits, or the first five of a run of nine (a
    // ZIP+4 written without its dash). Failing that, a Canadian postal code
    // ("Ottawa ON K1A 0B1"), normalized. Empty if there's none -- "Hamilton
    // County" or "1234 Main St" have no ZIP.
    std::string ExtractZipCode(const std::string& text);

    // True if two net names have something in common, however loosely, so
    // "Hamilton Co. ARES Net" and "Hamilton County ARES" look alike, as do
    // "TAG Skywarn" and "Tennessee Alabama Georgia Net", but "TAG Skywarn"
    // and "Hamilton County ARES" don't. Generous on purpose: it's there to
    // catch a session being imported into the wrong net, so it errs toward
    // "alike". Words every net name might have ("net", "amateur", "radio",
    // "county", ...) don't count; if either name has nothing else, there's
    // no telling, and the answer is `alike_if_unsure` -- true where a false
    // "alike" is the safe answer (no needless question), false where it
    // would raise one.
    bool NetNamesLookAlike(const std::string& a, const std::string& b, bool alike_if_unsure = true);

    // True if two net names are the same once capitals and spaces are set
    // aside: "Skywarn", "SKYWARN" and " Skywarn " are one name, as are
    // "TAG  Skywarn" and "TAG Skywarn". No two recurring nets may share a
    // name in this sense (see ExistingNetNamed in app_state.hpp).
    bool NetNamesAreTheSame(const std::string& a, const std::string& b);

}  // namespace ql
