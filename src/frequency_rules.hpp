#pragma once

#include <string>

namespace ql
{

    // True if `text` is a frequency in MHz ("146.940", "7.235", "1296") --
    // digits, optionally a decimal point and up to six more (1 Hz) -- that
    // falls in an amateur band in the United States or Canada: the union of
    // FCC Part 97 (47 CFR 97.301) and ISED RBR-4 allocations.
    //
    //   2200 m   0.1357-0.1378      6 m      50-54
    //    630 m   0.472-0.479        2 m      144-148
    //    160 m   1.8-2.0            1.25 m   219-225
    //     80 m   3.5-4.0            70 cm    420-450
    //     60 m   5.3305-5.4065      33 cm    902-928
    //     40 m   7.0-7.3            23 cm    1240-1300
    //     30 m   10.1-10.15         13 cm    2300-2310, 2390-2450
    //     20 m   14.0-14.35         9 cm     3300-3500
    //     17 m   18.068-18.168      5 cm     5650-5925
    //     15 m   21.0-21.45         3 cm     10000-10500
    //     12 m   24.89-24.99        1.2 cm   24000-24250
    //     10 m   28.0-29.7          and 47000-47200, 76000-81000,
    //                               122250-123000, 134000-141000,
    //                               241000-250000, 275000 and up
    //
    // 60 m is channelized in the US and partly in Canada; the whole span
    // from the lowest channel to the highest is accepted. 3300-3500 and
    // part of 1.25 m (220-222) are Canadian only now; they're accepted
    // since either country's operators may use this.
    bool IsAmateurFrequency(const std::string& text);

    // Why `text` isn't acceptable as a net's frequency, or "" if it is:
    // blank (it's optional) or an amateur frequency in MHz.
    std::string FrequencyProblem(const std::string& text);

    // The first amateur frequency (see IsAmateurFrequency) written with a
    // decimal point in free text, e.g. "146.940" from "146.940 -600 PL
    // 100", or "" if there's none. Whole numbers are passed over, since in
    // free text they're rarely frequencies ("Repeater 7").
    std::string ExtractAmateurFrequency(const std::string& text);

    // Why `offset` isn't acceptable as a repeater offset for a net on
    // `frequency` (which may be blank), or "" if it is. Blank is fine;
    // otherwise it's MHz with a sign, the way radios, CHIRP and
    // RepeaterBook give it: "-0.6", "+5", "-1.6". The other ways people
    // write offsets aren't accepted, so every net reads the same way: a
    // bare "+" or "-" (the band's standard offset, which isn't the same
    // everywhere on 6 m, 70 cm and up), kHz ("-600"), or a unit. It can't be
    // zero or 100 MHz or more, and with a frequency, the frequency plus the
    // offset (where the repeater listens) must be an amateur frequency too.
    std::string OffsetProblem(const std::string& offset, const std::string& frequency);

    // True if `tone` is one of the 50 standard CTCSS ("PL") tones, 67.0 to
    // 254.1 Hz, written with one decimal or none ("100.0", "100"), or
    // 150.0, which some radios also offer.
    bool IsCtcssTone(const std::string& tone);

    // Why `tone` isn't acceptable as a net's PL tone, or "" if it is:
    // blank or IsCtcssTone.
    std::string ToneProblem(const std::string& tone);

    // `tone` in the usual one-decimal form ("100" -> "100.0"); anything
    // that isn't a CTCSS tone comes back as it is.
    std::string NormalizeTone(const std::string& tone);

    // For a net saved before the frequency was checked (or imported from
    // such a file): if `*frequency` isn't blank or an amateur frequency,
    // keeps the old text in `*comments` ("Frequency: ...", after anything
    // already there) and sets `*frequency` to the amateur frequency found
    // in it, if any. Returns whether anything changed.
    bool MoveBadFrequencyToComments(std::string* frequency, std::string* comments);

}  // namespace ql
