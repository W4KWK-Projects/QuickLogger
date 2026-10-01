#include "mode_rules.hpp"

#include <cctype>
#include <string_view>

namespace ql
{

    const std::vector<std::string>& NetModes()
    {
        static const std::vector<std::string> modes{"FM", "SSB", "AM", "CW", "D-STAR", "DMR", "Fusion"};
        return modes;
    }

    // Uppercase, without spaces, hyphens, slashes, underscores or dots.
    static std::string ModeKey(const std::string& text)
    {
        std::string key;
        for (char c : text)
        {
            unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u) != 0)
            {
                key += static_cast<char>(std::toupper(u));
            }
            else if (c != ' ' && c != '-' && c != '/' && c != '_' && c != '.' && c != '\t')
            {
                // Anything else ("FM & DMR", "FM, SSB") isn't one mode.
                return std::string();
            }
        }
        return key;
    }

    // Lengths known at compile time: comparing against these needs no strlen.
    struct ModeSpelling
    {
        std::string_view key;
        std::string_view mode;
    };

    std::string NormalizeMode(const std::string& text)
    {
        static constexpr ModeSpelling kSpellings[] = {
            {"FM", "FM"},
            {"NFM", "FM"},
            {"SSB", "SSB"},
            {"USB", "SSB"},
            {"LSB", "SSB"},
            {"AM", "AM"},
            {"CW", "CW"},
            {"DSTAR", "D-STAR"},
            {"DMR", "DMR"},
            {"FUSION", "Fusion"},
            {"YSF", "Fusion"},
            {"C4FM", "Fusion"},
            {"SYSTEMFUSION", "Fusion"},
        };
        std::string key = ModeKey(text);
        for (const ModeSpelling& spelling : kSpellings)
        {
            if (key == spelling.key)
            {
                return std::string(spelling.mode);
            }
        }
        return std::string();
    }

    int NetModeIndex(const std::string& mode)
    {
        const std::vector<std::string>& modes = NetModes();
        for (std::size_t i = 0; i < modes.size(); ++i)
        {
            if (modes[i] == mode)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void AdifMode(const std::string& mode, std::string* adif_mode, std::string* adif_submode)
    {
        // By position in NetModes(): FM, SSB, AM, CW, D-STAR, DMR, Fusion.
        static constexpr std::string_view kSubmodes[] = {"DSTAR", "DMR", "C4FM"};
        adif_mode->clear();
        adif_submode->clear();
        int index = NetModeIndex(mode);
        if (index < 0)
        {
            return;
        }
        if (index < 4)
        {
            *adif_mode = mode;
            return;
        }
        adif_mode->assign("DIGITALVOICE");
        adif_submode->assign(kSubmodes[index - 4]);
    }

}  // namespace ql
