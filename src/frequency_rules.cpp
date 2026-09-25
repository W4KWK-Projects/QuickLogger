#include "frequency_rules.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>

namespace ql
{

    // A band's edges, in Hz.
    struct Band
    {
        std::int64_t low;
        std::int64_t high;
    };

    static constexpr std::int64_t kKHz = 1000;
    static constexpr std::int64_t kMHz = 1000 * kKHz;

    // See IsAmateurFrequency.
    static const Band kBands[] = {
        {135700, 137800},
        {472 * kKHz, 479 * kKHz},
        {1800 * kKHz, 2000 * kKHz},
        {3500 * kKHz, 4000 * kKHz},
        {5330500, 5406500},
        {7000 * kKHz, 7300 * kKHz},
        {10100 * kKHz, 10150 * kKHz},
        {14000 * kKHz, 14350 * kKHz},
        {18068 * kKHz, 18168 * kKHz},
        {21000 * kKHz, 21450 * kKHz},
        {24890 * kKHz, 24990 * kKHz},
        {28000 * kKHz, 29700 * kKHz},
        {50 * kMHz, 54 * kMHz},
        {144 * kMHz, 148 * kMHz},
        {219 * kMHz, 225 * kMHz},
        {420 * kMHz, 450 * kMHz},
        {902 * kMHz, 928 * kMHz},
        {1240 * kMHz, 1300 * kMHz},
        {2300 * kMHz, 2310 * kMHz},
        {2390 * kMHz, 2450 * kMHz},
        {3300 * kMHz, 3500 * kMHz},
        {5650 * kMHz, 5925 * kMHz},
        {10000 * kMHz, 10500 * kMHz},
        {24000 * kMHz, 24250 * kMHz},
        {47000 * kMHz, 47200 * kMHz},
        {76000 * kMHz, 81000 * kMHz},
        {122250 * kMHz, 123000 * kMHz},
        {134000 * kMHz, 141000 * kMHz},
        {241000 * kMHz, 250000 * kMHz},
        {275000 * kMHz, 3000000 * kMHz},
    };

    // The CTCSS tones, in tenths of a hertz (see IsCtcssTone).
    static const int kCtcssTones[] = {
        670,  693,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000,
        1035, 1072, 1109, 1148, 1188, 1230, 1273, 1318, 1365, 1413, 1462, 1500, 1514,
        1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928,
        1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
    };

    // `text` as MHz ("146.940") in Hz, or false if it isn't digits with
    // at most one point and six decimals.
    static bool ParseMhz(const std::string& text, std::int64_t* hz)
    {
        std::size_t point = text.find('.');
        std::string whole = text.substr(0, point);
        std::string fraction = point == std::string::npos ? "" : text.substr(point + 1);
        if (whole.empty() || whole.size() > 7 || fraction.size() > 6)
        {
            return false;
        }
        std::int64_t value = 0;
        for (char c : whole)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        std::int64_t fraction_hz = 0;
        std::int64_t place = kMHz;
        for (char c : fraction)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                return false;
            }
            place /= 10;
            fraction_hz += (c - '0') * place;
        }
        *hz = value * kMHz + fraction_hz;
        return true;
    }

    bool IsAmateurFrequency(const std::string& text)
    {
        std::int64_t hz = 0;
        if (!ParseMhz(text, &hz))
        {
            return false;
        }
        for (const Band& band : kBands)
        {
            if (hz >= band.low && hz <= band.high)
            {
                return true;
            }
        }
        return false;
    }

    std::string FrequencyProblem(const std::string& text)
    {
        if (text.empty() || IsAmateurFrequency(text))
        {
            return "";
        }
        std::int64_t hz = 0;
        if (!ParseMhz(text, &hz))
        {
            return "Frequency must be in MHz, e.g. 146.940, or left blank.";
        }
        return text + " MHz isn't in a US or Canadian amateur band.";
    }

    std::string ExtractAmateurFrequency(const std::string& text)
    {
        std::size_t i = 0;
        while (i < text.size())
        {
            if (!std::isdigit(static_cast<unsigned char>(text[i])))
            {
                ++i;
                continue;
            }
            std::size_t start = i;
            bool seen_point = false;
            while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) ||
                                       (text[i] == '.' && !seen_point)))
            {
                seen_point = seen_point || text[i] == '.';
                ++i;
            }
            std::string number = text.substr(start, i - start);
            if (!number.empty() && number.back() == '.')
            {
                number.pop_back();
            }
            // Only a number with a decimal point: "Repeater 7" isn't 7 MHz.
            if (number.find('.') != std::string::npos && IsAmateurFrequency(number))
            {
                return number;
            }
        }
        return "";
    }

    std::string OffsetProblem(const std::string& offset, const std::string& frequency)
    {
        if (offset.empty())
        {
            return "";
        }
        const std::string kHowTo = "Offset is in MHz with a + or - sign, e.g. -0.6 or +5";
        std::int64_t hz = 0;
        if ((offset[0] != '+' && offset[0] != '-') || !ParseMhz(offset.substr(1), &hz))
        {
            return kHowTo + ", or left blank.";
        }
        if (hz == 0)
        {
            return "Offset can't be zero; leave it blank for none.";
        }
        if (hz >= 100 * kMHz)
        {
            // Most likely kHz ("-600"): say what that is in MHz.
            std::string digits = offset.substr(1);
            if (digits.find('.') != std::string::npos)
            {
                return kHowTo + ".";
            }
            std::int64_t khz = hz / kMHz;
            std::string mhz = std::to_string(khz / 1000);
            std::string fraction = std::to_string(1000 + khz % 1000).substr(1);
            while (!fraction.empty() && fraction.back() == '0')
            {
                fraction.pop_back();
            }
            mhz += fraction.empty() ? "" : "." + fraction;
            return kHowTo + ": for " + digits + " kHz, type " + offset.substr(0, 1) + mhz + ".";
        }
        std::int64_t frequency_hz = 0;
        if (IsAmateurFrequency(frequency) && ParseMhz(frequency, &frequency_hz))
        {
            std::int64_t listen = frequency_hz + (offset[0] == '-' ? -hz : hz);
            bool in_band = false;
            for (const Band& band : kBands)
            {
                in_band = in_band || (listen >= band.low && listen <= band.high);
            }
            if (!in_band)
            {
                return frequency + " MHz " + offset + " is outside the amateur bands.";
            }
        }
        return "";
    }

    // `tone` in tenths of a hertz, or -1 if it isn't a number with at most
    // one decimal.
    static int ToneTenths(const std::string& tone)
    {
        std::size_t point = tone.find('.');
        std::string whole = tone.substr(0, point);
        std::string tenth = point == std::string::npos ? "0" : tone.substr(point + 1);
        if (whole.empty() || whole.size() > 3 || tenth.size() != 1)
        {
            return -1;
        }
        int value = 0;
        for (char c : whole + tenth)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                return -1;
            }
            value = value * 10 + (c - '0');
        }
        return value;
    }

    bool IsCtcssTone(const std::string& tone)
    {
        int tenths = ToneTenths(tone);
        for (int standard : kCtcssTones)
        {
            if (standard == tenths)
            {
                return true;
            }
        }
        return false;
    }

    std::string ToneProblem(const std::string& tone)
    {
        if (tone.empty() || IsCtcssTone(tone))
        {
            return "";
        }
        return "PL tone must be a standard CTCSS tone, e.g. 100.0 (67.0 to 254.1), or left "
               "blank.";
    }

    std::string NormalizeTone(const std::string& tone)
    {
        if (!IsCtcssTone(tone))
        {
            return tone;
        }
        int tenths = ToneTenths(tone);
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
    }

    bool MoveBadFrequencyToComments(std::string* frequency, std::string* comments)
    {
        if (frequency->empty() || IsAmateurFrequency(*frequency))
        {
            return false;
        }
        *comments += (comments->empty() ? "" : "; ") + std::string("Frequency: ") + *frequency;
        *frequency = ExtractAmateurFrequency(*frequency);
        return true;
    }

}  // namespace ql
