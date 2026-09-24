#include "text_utils.hpp"

#include <cctype>

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

    // The plain letter for a UTF-8 character from the Latin-1 Supplement
    // (encoded as 0xC3 followed by `second_byte`), or 0 if it isn't an
    // accented letter this cares about.
    static char UnaccentedLatin1Letter(unsigned char second_byte)
    {
        // 0xC3 0x80-0x9F are the uppercase letters, 0xA0-0xBF lowercase;
        // folding the case bit first lets one table cover both.
        unsigned char upper =
            second_byte >= 0xA0 ? static_cast<unsigned char>(second_byte - 0x20) : second_byte;
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

}  // namespace ql
