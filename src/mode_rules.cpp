#include "mode_rules.hpp"

#include <cctype>

namespace ql
{

    const std::vector<std::string>& NetModes()
    {
        static const std::vector<std::string> modes{"FM",     "SSB", "AM",    "CW",
                                                    "D-STAR", "DMR", "Fusion"};
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

    struct ModeSpelling
    {
        const char* key;
        const char* mode;
    };

    std::string NormalizeMode(const std::string& text)
    {
        static const ModeSpelling kSpellings[] = {
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
                return spelling.mode;
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
        adif_mode->clear();
        adif_submode->clear();
        if (mode == "FM" || mode == "SSB" || mode == "AM" || mode == "CW")
        {
            *adif_mode = mode;
        }
        else if (mode == "D-STAR" || mode == "DMR" || mode == "Fusion")
        {
            *adif_mode = "DIGITALVOICE";
            *adif_submode = mode == "D-STAR" ? "DSTAR" : mode == "DMR" ? "DMR" : "C4FM";
        }
    }

}  // namespace ql
