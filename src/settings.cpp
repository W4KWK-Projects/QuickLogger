#include "settings.hpp"

#include <cctype>
#include <fstream>

namespace ql
{

    static bool IsFiveDigitZip(const std::string& value)
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

    static std::string Trim(const std::string& value)
    {
        std::size_t start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
        {
            return "";
        }
        std::size_t end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, end - start + 1);
    }

    AppSettings LoadSettings(const std::string& path)
    {
        AppSettings settings;

        std::ifstream file(path);
        if (!file.is_open())
        {
            return settings;
        }

        std::string line;
        while (std::getline(file, line))
        {
            std::size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                continue;
            }

            std::string key = Trim(line.substr(0, separator));
            std::string value = Trim(line.substr(separator + 1));

            if (key == "callsign")
            {
                settings.callsign = value;
            }
            else if (key == "qrz_username")
            {
                settings.qrz_username = value;
            }
            else if (key == "qrz_password")
            {
                settings.qrz_password = value;
            }
            else if (key == "location")
            {
                settings.location = value;
            }
        }

        return settings;
    }

    void SaveSettings(const std::string& path, const AppSettings& settings)
    {
        std::ofstream file(path, std::ios::trunc);
        file << "callsign=" << settings.callsign << "\n";
        file << "qrz_username=" << settings.qrz_username << "\n";
        file << "qrz_password=" << settings.qrz_password << "\n";
        file << "location=" << settings.location << "\n";
    }

    bool SettingsAreComplete(const AppSettings& settings)
    {
        return !settings.callsign.empty() && IsFiveDigitZip(settings.location);
    }

}  // namespace ql
