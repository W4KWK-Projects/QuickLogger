#include "text_utils.hpp"

#include <algorithm>
#include <cctype>

namespace ql
{

    std::string ToUpperAscii(const std::string& value)
    {
        std::string result = value;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return result;
    }

}  // namespace ql
