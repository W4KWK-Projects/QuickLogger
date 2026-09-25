#pragma once

#include <string>

namespace ql
{

    // True if `callsign` (already normalized -- see NormalizeCallsign) is a
    // call sign the US or Canada could issue, optionally with a portable
    // indicator.
    //
    // United States (FCC sequential and vanity systems, 47 CFR 97.3 / 97.21):
    // a prefix of K, N or W, or of AA-AL, KA-KZ, NA-NZ or WA-WZ; one digit;
    // then a suffix of letters:
    //   1x1  K, N or W + digit + one letter other than X (special event)
    //   1x2  K, N or W + digit + two letters                    (W1AW)
    //   1x3  K, N or W + digit + three letters                  (N1NJA)
    //   2x1  any two-letter prefix + digit + one letter         (AB0C)
    //   2x2  any two-letter prefix + digit + two letters        (AK6SE)
    //   2x3  any two-letter prefix + digit + three letters      (KA2DOG)
    //        The sequential system only issues these from KA-KZ, WA-WZ and
    //        NH/NL/NP, but the FCC has issued others (AF1EMA, NF1EMA), so
    //        any two-letter prefix is accepted.
    //
    // Canada (ISED RIC-9): a prefix and district digit, then letters:
    //   VE0-VE9, VA1-VA7, VO1-VO2, VY0-VY2, VY9 (government), CY0 (Sable
    //   Island), CY9 (St. Paul Island), and the special event prefixes CG,
    //   CK, VX, XM, VC (districts 1-9), CH, CY, XJ, XN, VD (1-2) and CI, CZ,
    //   XK, XO, VF (0-2). Suffixes are normally two or three letters; special
    //   event calls may have one, four or five, and a few have carried a
    //   multi-digit numeral in place of the district digit (VE2008VQ,
    //   CG200I).
    //
    // Portable indicators: a trailing /digit or /one-to-three letters
    // (W4KWK/M, W4KWK/P, W4KWK/MM, W4KWK/AE, W4KWK/QRP, W4KWK/4) and/or a
    // leading US or Canadian prefix with its digit (VE3/W4KWK, KH6/VE3ABC).
    bool IsValidCallsign(const std::string& callsign);

    // `callsign` without its portable indicators: "W4KWK" for W4KWK/M,
    // VE3/W4KWK or VE3/W4KWK/P. Anything it can't make sense of comes back
    // as it is.
    std::string BaseCallsign(const std::string& callsign);

}  // namespace ql
