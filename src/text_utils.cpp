#include "text_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <vector>

namespace ql
{

    std::string ToUpperAscii(const std::string& value)
    {
        std::string result = value;
        for (char& c : result)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return result;
    }

    bool IsFiveDigitZip(const std::string& value)
    {
        if (value.size() != 5)
        {
            return false;
        }
        for (char c : value)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                return false;
            }
        }
        return true;
    }

    static bool IsLetter(char c)
    {
        return std::isalpha(static_cast<unsigned char>(c)) != 0;
    }

    static bool IsDigit(char c)
    {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    }

    // `value` without spaces, or "" if it has any other character that is
    // not a letter or digit.
    static std::string WithoutSpaces(const std::string& value)
    {
        std::string result;
        for (char c : value)
        {
            if (c != ' ')
            {
                result.push_back(c);
            }
        }
        return result;
    }

    bool IsCanadianPostalCode(const std::string& value)
    {
        std::string code = WithoutSpaces(value);
        if (code.size() != 3 && code.size() != 6)
        {
            return false;
        }
        for (std::size_t i = 0; i < code.size(); ++i)
        {
            if (i % 2 == 0 ? !IsLetter(code[i]) : !IsDigit(code[i]))
            {
                return false;
            }
        }
        return true;
    }

    bool IsZipOrPostalCode(const std::string& value)
    {
        return IsFiveDigitZip(value) || IsCanadianPostalCode(value);
    }

    std::string NormalizeZipOrPostalCode(const std::string& value)
    {
        if (!IsCanadianPostalCode(value))
        {
            return value;
        }
        std::string code = ToUpperAscii(WithoutSpaces(value));
        if (code.size() == 6)
        {
            code.insert(3, " ");
        }
        return code;
    }

    std::string ExtractZipCode(const std::string& text)
    {
        std::string zip;
        std::size_t i = 0;
        while (i < text.size())
        {
            if (!std::isdigit(static_cast<unsigned char>(text[i])))
            {
                ++i;
                continue;
            }
            std::size_t start = i;
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
            {
                ++i;
            }
            std::size_t length = i - start;
            if (length == 5 || length == 9)
            {
                zip = text.substr(start, 5);
            }
        }
        if (!zip.empty())
        {
            return zip;
        }
        // No ZIP: a Canadian postal code, "K1A 0B1" or "K1A0B1".
        for (std::size_t i = 0; i + 6 <= text.size(); ++i)
        {
            std::size_t length = i + 7 <= text.size() && text[i + 3] == ' ' ? 7 : 6;
            if (i + length <= text.size() && IsCanadianPostalCode(text.substr(i, length)) &&
                WithoutSpaces(text.substr(i, length)).size() == 6 && (i == 0 || !IsLetter(text[i - 1])) &&
                (i + length == text.size() || !IsLetter(text[i + length])))
            {
                return NormalizeZipOrPostalCode(text.substr(i, length));
            }
        }
        return zip;
    }

    std::string NormalizeCallsign(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());
        for (char c : value)
        {
            unsigned char byte = static_cast<unsigned char>(c);
            if (std::isalnum(byte) != 0 && byte < 0x80)
            {
                result.push_back(static_cast<char>(std::toupper(byte)));
            }
            else if (c == '/')
            {
                result.push_back(c);
            }
        }
        return result;
    }

    std::string NormalizeTypedCallsign(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());
        for (char c : value)
        {
            if (c == '?')
            {
                result.push_back(c);
            }
            else
            {
                result += NormalizeCallsign(std::string(1, c));
            }
        }
        return result;
    }

    bool IsWildcardCallsign(const std::string& typed)
    {
        return typed.find('?') != std::string::npos;
    }

    bool WildcardHasEnough(const std::string& typed)
    {
        return IsWildcardCallsign(typed) && NormalizeCallsign(typed).size() >= 2;
    }

    std::string WildcardLikePattern(const std::string& typed, bool anchored)
    {
        std::string pattern = anchored ? "" : "%";
        for (char c : NormalizeCallsign(typed))
        {
            pattern.push_back(c);
            pattern.push_back('%');
        }
        return pattern;
    }

    int WildcardSpan(const std::string& letters, const char* callsign, std::size_t length, bool anchored)
    {
        if (letters.empty())
        {
            return -1;
        }
        int best = -1;
        for (std::size_t start = 0; start < length; ++start)
        {
            if (anchored && start > 0)
            {
                break;
            }
            if (callsign[start] != letters[0])
            {
                continue;
            }
            // The rest, each at its first place after the one before.
            std::size_t at = start;
            std::size_t matched = 1;
            while (matched < letters.size() && ++at < length)
            {
                if (callsign[at] == letters[matched])
                {
                    ++matched;
                }
            }
            if (matched < letters.size())
            {
                // Nothing starting later can fit them in either.
                break;
            }
            int span = static_cast<int>(at - start + 1);
            best = best < 0 || span < best ? span : best;
        }
        return best;
    }

    // The plain letter for a UTF-8 character from the Latin-1 Supplement
    // (encoded as 0xC3 followed by `second_byte`), or 0 if it isn't an
    // accented letter this cares about.
    static char UnaccentedLatin1Letter(unsigned char second_byte)
    {
        // 0xC3 0x80-0x9F are the uppercase letters, 0xA0-0xBF lowercase;
        // folding the case bit first lets one table cover both.
        unsigned char upper = second_byte >= 0xA0 ? static_cast<unsigned char>(second_byte - 0x20) : second_byte;
        if (upper >= 0x80 && upper <= 0x85)
        {
            return 'A';
        }
        if (upper == 0x87)
        {
            return 'C';
        }
        if (upper >= 0x88 && upper <= 0x8B)
        {
            return 'E';
        }
        if (upper >= 0x8C && upper <= 0x8F)
        {
            return 'I';
        }
        if (upper == 0x91)
        {
            return 'N';
        }
        if ((upper >= 0x92 && upper <= 0x96) || upper == 0x98)
        {
            return 'O';
        }
        if (upper >= 0x99 && upper <= 0x9C)
        {
            return 'U';
        }
        if (upper == 0x9D)
        {
            return 'Y';
        }
        return 0;
    }

    std::string NormalizePlaceName(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());
        for (std::string::size_type i = 0; i < value.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(value[i]);
            if (c == 0xC3 && i + 1 < value.size())
            {
                char plain = UnaccentedLatin1Letter(static_cast<unsigned char>(value[i + 1]));
                if (plain != 0)
                {
                    result += plain;
                    ++i;
                    continue;
                }
            }
            if (c == '.')
            {
                continue;
            }
            result += static_cast<char>(std::toupper(c));
        }
        return result;
    }

    // A name's words: runs of letters and digits, uppercase, accents folded
    // (see NormalizePlaceName). Common abbreviations are spelled out, so
    // "Co." and "County" are the same word.
    static std::vector<std::string> NameWords(const std::string& name)
    {
        std::string normalized = NormalizePlaceName(name);
        std::vector<std::string> words;
        std::string word;
        for (std::size_t i = 0; i <= normalized.size(); ++i)
        {
            char c = i < normalized.size() ? normalized[i] : ' ';
            if (std::isalnum(static_cast<unsigned char>(c)) != 0)
            {
                word += c;
                continue;
            }
            if (word.empty())
            {
                continue;
            }
            if (word == "CO" || word == "CTY" || word == "CNTY")
            {
                word = "COUNTY";
            }
            else if (word == "WX")
            {
                word = "WEATHER";
            }
            else if (word == "EMERG" || word == "EMRG")
            {
                word = "EMERGENCY";
            }
            words.push_back(word);
            word.clear();
        }
        return words;
    }

    // Words that say nothing about which net it is.
    static bool IsCommonNetWord(const std::string& word)
    {
        static const std::set<std::string> kCommon = {
            "A",        "AN",          "AND",    "AT",      "FOR",       "IN",       "OF",      "ON",
            "THE",      "NET",         "NETS",   "SESSION", "AMATEUR",   "RADIO",    "HAM",     "HAMS",
            "CLUB",     "ASSOCIATION", "ASSN",   "SOCIETY", "GROUP",     "COUNTY",   "CITY",    "AREA",
            "REGIONAL", "DISTRICT",    "WEEKLY", "DAILY",   "NIGHTLY",   "MONTHLY",  "MORNING", "AFTERNOON",
            "EVENING",  "NIGHT",       "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY",  "SATURDAY",
            "SUNDAY",   "FM",          "SSB",    "HF",      "VHF",       "UHF",      "DMR",     "REPEATER"};
        return kCommon.count(word) != 0;
    }

    static bool IsAllDigits(const std::string& word)
    {
        for (char c : word)
        {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0)
            {
                return false;
            }
        }
        return !word.empty();
    }

    // Edit distance, counting a swap of two neighboring letters as one edit.
    static std::size_t TypoDistance(const std::string& a, const std::string& b)
    {
        std::vector<std::vector<std::size_t>> d(a.size() + 1, std::vector<std::size_t>(b.size() + 1, 0));
        for (std::size_t i = 0; i <= a.size(); ++i)
        {
            d[i][0] = i;
        }
        for (std::size_t j = 0; j <= b.size(); ++j)
        {
            d[0][j] = j;
        }
        for (std::size_t i = 1; i <= a.size(); ++i)
        {
            for (std::size_t j = 1; j <= b.size(); ++j)
            {
                std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
                d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
                if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                {
                    d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
                }
            }
        }
        return d[a.size()][b.size()];
    }

    // Two words alike: the same, one inside the other ("SKY", "SKYWARN"), or
    // a typo apart. Numbers ("220", "146") only match exactly.
    static bool WordsLookAlike(const std::string& a, const std::string& b)
    {
        if (a == b)
        {
            return true;
        }
        if (IsAllDigits(a) || IsAllDigits(b))
        {
            return false;
        }
        const std::string& shorter = a.size() <= b.size() ? a : b;
        const std::string& longer = a.size() <= b.size() ? b : a;
        if (shorter.size() >= 3 && longer.find(shorter) != std::string::npos)
        {
            return true;
        }
        if (shorter.size() >= 7)
        {
            return TypoDistance(a, b) <= 2;
        }
        if (shorter.size() >= 4)
        {
            return TypoDistance(a, b) <= 1;
        }
        return false;
    }

    // The first letters of `words`, e.g. "TAG" for Tennessee Alabama
    // Georgia; with `skip_filler`, leaving out "the", "of", "and" and "net".
    static std::string Initials(const std::vector<std::string>& words, bool skip_filler)
    {
        std::string initials;
        for (const std::string& word : words)
        {
            if (skip_filler && (word == "THE" || word == "OF" || word == "AND" || word == "NET"))
            {
                continue;
            }
            initials += word[0];
        }
        return initials;
    }

    // Whether a word of `words` (two letters or more) is the initials of
    // `other`.
    static bool HasAcronymOf(const std::vector<std::string>& words, const std::vector<std::string>& other)
    {
        std::string all = Initials(other, false);
        std::string trimmed = Initials(other, true);
        for (const std::string& word : words)
        {
            if (word.size() >= 2 && !IsAllDigits(word) && (word == all || word == trimmed))
            {
                return true;
            }
        }
        return false;
    }

    static std::string JoinWords(const std::vector<std::string>& words)
    {
        std::string joined;
        for (const std::string& word : words)
        {
            joined += word;
        }
        return joined;
    }

    // `name` uppercased, without leading or trailing spaces and with each
    // run of spaces inside it made one.
    static std::string NetNameKey(const std::string& name)
    {
        std::string key;
        bool pending_space = false;
        for (char c : name)
        {
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                pending_space = !key.empty();
                continue;
            }
            if (pending_space)
            {
                key += ' ';
                pending_space = false;
            }
            key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return key;
    }

    bool NetNamesAreTheSame(const std::string& a, const std::string& b)
    {
        return NetNameKey(a) == NetNameKey(b);
    }

    bool NetNamesLookAlike(const std::string& a, const std::string& b, bool alike_if_unsure)
    {
        std::vector<std::string> words_a = NameWords(a);
        std::vector<std::string> words_b = NameWords(b);

        // Written with different spacing: "220 EOR" and "220EOR".
        std::string joined_a = JoinWords(words_a);
        std::string joined_b = JoinWords(words_b);
        if (joined_a == joined_b)
        {
            return true;
        }

        std::vector<std::string> key_a;
        std::vector<std::string> key_b;
        for (const std::string& word : words_a)
        {
            if (!IsCommonNetWord(word))
            {
                key_a.push_back(word);
            }
        }
        for (const std::string& word : words_b)
        {
            if (!IsCommonNetWord(word))
            {
                key_b.push_back(word);
            }
        }
        if (key_a.empty() || key_b.empty())
        {
            return alike_if_unsure;
        }

        for (const std::string& word_a : key_a)
        {
            for (const std::string& word_b : key_b)
            {
                if (WordsLookAlike(word_a, word_b))
                {
                    return true;
                }
            }
        }
        if (HasAcronymOf(key_a, words_b) || HasAcronymOf(key_b, words_a))
        {
            return true;
        }
        // One name's words run together inside the other's: "SKYWARN" in
        // "TAG SKY WARN".
        std::string key_joined_a = JoinWords(key_a);
        std::string key_joined_b = JoinWords(key_b);
        for (const std::string& word : key_a)
        {
            if (word.size() >= 4 && key_joined_b.find(word) != std::string::npos)
            {
                return true;
            }
        }
        for (const std::string& word : key_b)
        {
            if (word.size() >= 4 && key_joined_a.find(word) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

}  // namespace ql
