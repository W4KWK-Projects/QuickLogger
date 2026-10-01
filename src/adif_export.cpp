#include "adif_export.hpp"

#include <cstdlib>
#include <ctime>

#include "date_utils.hpp"
#include "mode_rules.hpp"
#include "version.hpp"

namespace ql
{

    // U+00C0 to U+00FF, and U+0100 to U+017F, without their accents. A few
    // become two letters ("AE", "ss"), marked here by '*' and handled in
    // FoldLatin1.
    static const char kLatin1Folded[] =
        "AAAAAA*CEEEEIIIIDNOOOOOxOUUUUY**aaaaaa*ceeeeiiiidnooooo/ouuuuy*y";
    static const char kLatinExtendedAFolded[] =
        "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRr"
        "SsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

    static std::string FoldLatin1(unsigned int code_point)
    {
        switch (code_point)
        {
            case 0xC6:
                return "AE";
            case 0xDE:
                return "TH";
            case 0xDF:
                return "ss";
            case 0xE6:
                return "ae";
            case 0xFE:
                return "th";
            default:
                return std::string(1, kLatin1Folded[code_point - 0xC0]);
        }
    }

    std::string FoldToAscii(const std::string& text)
    {
        std::string folded;
        folded.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size())
        {
            unsigned char lead = static_cast<unsigned char>(text[i]);
            if (lead < 0x80)
            {
                folded += static_cast<char>(lead);
                ++i;
                continue;
            }
            // The code point and how many bytes it takes.
            int length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            unsigned int code_point = length == 4   ? lead & 0x07U
                                      : length == 3 ? lead & 0x0FU
                                                    : lead & 0x1FU;
            bool valid = length > 1 && i + static_cast<std::size_t>(length) <= text.size();
            for (int k = 1; valid && k < length; ++k)
            {
                unsigned char next =
                    static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
                valid = (next & 0xC0) == 0x80;
                code_point = (code_point << 6) | (next & 0x3FU);
            }
            i += valid ? static_cast<std::size_t>(length) : 1;
            if (!valid)
            {
                continue;
            }
            if (code_point >= 0xC0 && code_point <= 0xFF)
            {
                folded += FoldLatin1(code_point);
            }
            else if (code_point >= 0x100 && code_point <= 0x17F)
            {
                folded += kLatinExtendedAFolded[code_point - 0x100];
            }
            else if (code_point == 0x2018 || code_point == 0x2019)
            {
                folded += '\'';
            }
            else if (code_point == 0x201C || code_point == 0x201D)
            {
                folded += '"';
            }
            else if (code_point == 0x2013 || code_point == 0x2014)
            {
                folded += '-';
            }
            else if (code_point == 0xA0)
            {
                folded += ' ';
            }
        }
        return folded;
    }

    // `text` without spaces at either end.
    static std::string TrimSpaces(const std::string& text)
    {
        std::string::size_type first = text.find_first_not_of(' ');
        if (first == std::string::npos)
        {
            return "";
        }
        return text.substr(first, text.find_last_not_of(' ') - first + 1);
    }

    std::string FirstNameFirst(const std::string& name)
    {
        std::string::size_type comma = name.find(',');
        if (comma == std::string::npos || name.find(',', comma + 1) != std::string::npos)
        {
            return name;
        }
        std::string last = TrimSpaces(name.substr(0, comma));
        std::string first = TrimSpaces(name.substr(comma + 1));
        if (last.empty() || first.empty())
        {
            return TrimSpaces(last + first);
        }
        return first + " " + last;
    }

    struct AdifBandRange
    {
        double low;
        double high;
        const char* band;
    };

    std::string AdifBand(const std::string& frequency)
    {
        // ADIF 3.1's band table, amateur bands only.
        static const AdifBandRange kBands[] = {
            {0.1357, 0.1378, "2190m"},   {0.472, 0.479, "630m"},      {1.8, 2.0, "160m"},
            {3.5, 4.0, "80m"},           {5.06, 5.45, "60m"},         {7.0, 7.3, "40m"},
            {10.1, 10.15, "30m"},        {14.0, 14.35, "20m"},        {18.068, 18.168, "17m"},
            {21.0, 21.45, "15m"},        {24.89, 24.99, "12m"},       {28.0, 29.7, "10m"},
            {50.0, 54.0, "6m"},          {70.0, 71.0, "4m"},          {144.0, 148.0, "2m"},
            {222.0, 225.0, "1.25m"},     {420.0, 450.0, "70cm"},      {902.0, 928.0, "33cm"},
            {1240.0, 1300.0, "23cm"},    {2300.0, 2450.0, "13cm"},    {3300.0, 3500.0, "9cm"},
            {5650.0, 5925.0, "6cm"},     {10000.0, 10500.0, "3cm"},   {24000.0, 24250.0, "1.25cm"},
            {47000.0, 47200.0, "6mm"},   {75500.0, 81000.0, "4mm"},   {119980.0, 123000.0, "2.5mm"},
            {134000.0, 149000.0, "2mm"}, {241000.0, 250000.0, "1mm"},
        };
        if (frequency.empty())
        {
            return "";
        }
        char* end = nullptr;
        double mhz = std::strtod(frequency.c_str(), &end);
        if (end == frequency.c_str())
        {
            return "";
        }
        for (const AdifBandRange& range : kBands)
        {
            if (mhz >= range.low && mhz <= range.high)
            {
                return range.band;
            }
        }
        return "";
    }

    // One field, "<NAME:length>value ", or nothing for an empty value. The
    // length is in bytes, so it's taken after folding to ASCII.
    static void AppendField(std::string* out, const char* name, const std::string& value)
    {
        std::string ascii = FoldToAscii(value);
        if (ascii.empty())
        {
            return;
        }
        *out += "<";
        *out += name;
        *out += ":" + std::to_string(ascii.size()) + ">" + ascii + " ";
    }

    static std::string FormatUtc(std::int64_t unix_time, const char* format)
    {
        std::tm utc = UtcTime(static_cast<std::time_t>(unix_time));
        char buffer[32];
        std::size_t length = std::strftime(buffer, sizeof(buffer), format, &utc);
        return std::string(buffer, length);
    }

    std::string BuildAdif(const std::vector<AdifContact>& contacts, const std::string& mode,
                          const std::string& frequency, const std::string& station_callsign,
                          const std::string& session_date, std::int64_t created_at)
    {
        std::string out = "QuickLogger ADIF export\n";
        AppendField(&out, "ADIF_VER", "3.1.4");
        AppendField(&out, "PROGRAMID", "QuickLogger");
        AppendField(&out, "PROGRAMVERSION", QuickLoggerVersion());
        AppendField(&out, "CREATED_TIMESTAMP", FormatUtc(created_at, "%Y%m%d %H%M%S"));
        out += "<EOH>\n";

        std::string adif_mode;
        std::string adif_submode;
        AdifMode(mode, &adif_mode, &adif_submode);
        std::string band = AdifBand(frequency);
        // Without a check-in time (from an older export), the session's
        // date, with no time.
        std::string fallback_date;
        for (char c : session_date)
        {
            if (c != '-')
            {
                fallback_date += c;
            }
        }

        for (const AdifContact& contact : contacts)
        {
            const Station& station = contact.station;
            std::int64_t at = contact.check_in.checked_in_at;
            std::string record;
            AppendField(&record, "CALL", contact.check_in.callsign);
            AppendField(&record, "QSO_DATE", at > 0 ? FormatUtc(at, "%Y%m%d") : fallback_date);
            AppendField(&record, "TIME_ON", at > 0 ? FormatUtc(at, "%H%M%S") : std::string());
            AppendField(&record, "FREQ", band.empty() ? std::string() : frequency);
            AppendField(&record, "BAND", band);
            AppendField(&record, "MODE", adif_mode);
            AppendField(&record, "SUBMODE", adif_submode);
            AppendField(&record, "STATION_CALLSIGN", station_callsign);
            AppendField(&record, "RST_RCVD", contact.check_in.signal_report);
            AppendField(&record, "NAME", FirstNameFirst(station.name));
            AppendField(&record, "QTH", station.city);
            AppendField(&record, "STATE", station.state);
            AppendField(&record, "CNTY",
                        station.state.empty() || station.county.empty()
                            ? std::string()
                            : station.state + "," + station.county);
            AppendField(&record, "GRIDSQUARE", station.grid_square);
            AppendField(&record, "COMMENT", contact.check_in.remarks);
            AppendField(&record, "NOTES", contact.check_in.comment);
            out += record + "<EOR>\n";
        }
        return out;
    }

}  // namespace ql
