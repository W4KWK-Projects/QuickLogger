#include "callsign_rules.hpp"

#include <cstddef>
#include <string_view>

namespace ql
{

    // A call sign (or the prefix of one) split into its leading letters, its
    // numeral and the letters after it: "VE3ABC" is "VE", "3", "ABC". Views
    // into the text being checked, so validating allocates nothing.
    struct CallsignParts
    {
        std::string_view letters;
        std::string_view digits;
        std::string_view suffix;
    };

    static bool IsUpperLetter(char c)
    {
        return c >= 'A' && c <= 'Z';
    }

    static bool IsDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    // Splits `text` into letters, digits and letters, each part non-empty
    // except the suffix. False if `text` isn't shaped that way at all.
    static bool SplitCallsign(std::string_view text, CallsignParts* parts)
    {
        std::size_t i = 0;
        while (i < text.size() && IsUpperLetter(text[i]))
        {
            ++i;
        }
        parts->letters = text.substr(0, i);
        std::size_t digits_start = i;
        while (i < text.size() && IsDigit(text[i]))
        {
            ++i;
        }
        parts->digits = text.substr(digits_start, i - digits_start);
        std::size_t suffix_start = i;
        while (i < text.size() && IsUpperLetter(text[i]))
        {
            ++i;
        }
        parts->suffix = text.substr(suffix_start, i - suffix_start);
        return i == text.size() && !parts->letters.empty() && !parts->digits.empty();
    }

    // ---- United States -------------------------------------------------------

    static bool IsUsPrefix(std::string_view letters)
    {
        if (letters.size() == 1)
        {
            return letters[0] == 'K' || letters[0] == 'N' || letters[0] == 'W';
        }
        if (letters.size() == 2)
        {
            if (letters[0] == 'A')
            {
                return letters[1] >= 'A' && letters[1] <= 'L';
            }
            return letters[0] == 'K' || letters[0] == 'N' || letters[0] == 'W';
        }
        return false;
    }

    static bool IsUsCallsign(const CallsignParts& parts)
    {
        if (parts.digits.size() != 1 || !IsUsPrefix(parts.letters))
        {
            return false;
        }
        std::size_t suffix_length = parts.suffix.size();
        if (suffix_length == 1)
        {
            // 1x1 special event calls never end in X (reserved for
            // experimental stations).
            return parts.letters.size() == 2 || parts.suffix[0] != 'X';
        }
        return suffix_length == 2 || suffix_length == 3;
    }

    // ---- Canada --------------------------------------------------------------

    // A Canadian amateur prefix and the district digits it's used with.
    struct CanadianPrefix
    {
        const char* letters;
        const char* districts;
    };

    static const CanadianPrefix kCanadianPrefixes[] = {
        // Regular prefixes.
        {"VE", "0123456789"},
        {"VA", "1234567"},
        {"VO", "12"},
        {"VY", "0129"},
        // CY0 Sable Island and CY9 St. Paul Island; CY1-CY2 are special
        // event prefixes for Newfoundland and Labrador.
        {"CY", "0129"},
        // Special event prefixes (RIC-9 Table 1).
        {"CG", "123456789"},
        {"CK", "123456789"},
        {"VX", "123456789"},
        {"XM", "123456789"},
        {"VC", "123456789"},
        {"CH", "12"},
        {"XJ", "12"},
        {"XN", "12"},
        {"VD", "12"},
        {"CI", "012"},
        {"CZ", "012"},
        {"XK", "012"},
        {"XO", "012"},
        {"VF", "012"},
    };

    // True if `letters` is a Canadian prefix used with district `digit`.
    static bool IsCanadianPrefix(std::string_view letters, char digit)
    {
        for (const CanadianPrefix& prefix : kCanadianPrefixes)
        {
            if (letters == prefix.letters)
            {
                for (const char* district = prefix.districts; *district != '\0'; ++district)
                {
                    if (*district == digit)
                    {
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    }

    static bool IsCanadianCallsign(const CallsignParts& parts)
    {
        // A special event numeral of up to four digits (VE2008VQ, CG200I)
        // still starts with the district digit.
        if (parts.digits.size() > 4 || !IsCanadianPrefix(parts.letters, parts.digits[0]))
        {
            return false;
        }
        return !parts.suffix.empty() && parts.suffix.size() <= 5;
    }

    // ---- Whole call signs ----------------------------------------------------

    static bool IsBaseCallsign(std::string_view text)
    {
        CallsignParts parts;
        if (!SplitCallsign(text, &parts))
        {
            return false;
        }
        return IsUsCallsign(parts) || IsCanadianCallsign(parts);
    }

    // A US or Canadian prefix with its digit and nothing after it ("W4",
    // "KH6", "VE3"), as in VE3/W4KWK or VE3ABC/W4.
    static bool IsLocationPrefix(std::string_view text)
    {
        CallsignParts parts;
        if (!SplitCallsign(text, &parts) || parts.digits.size() != 1 || !parts.suffix.empty())
        {
            return false;
        }
        return IsUsPrefix(parts.letters) || IsCanadianPrefix(parts.letters, parts.digits[0]);
    }

    // What may follow a call sign after a slash: a call district digit
    // (W4KWK/4), one to three letters (/M, /P, /MM, /AM, /AE, /AG, /QRP), or
    // a location prefix (VE3ABC/W4).
    static bool IsTrailingIndicator(std::string_view text)
    {
        if (text.size() == 1 && IsDigit(text[0]))
        {
            return true;
        }
        if (!text.empty() && text.size() <= 3)
        {
            bool all_letters = true;
            for (char c : text)
            {
                if (!IsUpperLetter(c))
                {
                    all_letters = false;
                    break;
                }
            }
            if (all_letters)
            {
                return true;
            }
        }
        return IsLocationPrefix(text);
    }

    bool IsValidCallsign(const std::string& callsign)
    {
        // At most three slash-separated pieces: [prefix/]call[/indicator].
        std::string_view text = callsign;
        std::string_view pieces[3];
        std::size_t count = 0;
        std::size_t start = 0;
        while (true)
        {
            if (count == 3)
            {
                return false;
            }
            std::size_t slash = text.find('/', start);
            if (slash == std::string_view::npos)
            {
                pieces[count++] = text.substr(start);
                break;
            }
            pieces[count++] = text.substr(start, slash - start);
            start = slash + 1;
        }

        if (count == 1)
        {
            return IsBaseCallsign(pieces[0]);
        }
        if (count == 2)
        {
            return (IsBaseCallsign(pieces[0]) && IsTrailingIndicator(pieces[1])) ||
                   (IsLocationPrefix(pieces[0]) && IsBaseCallsign(pieces[1]));
        }
        return IsLocationPrefix(pieces[0]) && IsBaseCallsign(pieces[1]) &&
               IsTrailingIndicator(pieces[2]);
    }

}  // namespace ql
