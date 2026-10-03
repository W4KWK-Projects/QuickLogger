#include "settings.hpp"

#include <cctype>
#include <fstream>

#include "text_utils.hpp"

namespace ql
{

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

    // A stored Nearby Radius, clamped to the allowed range; the default if
    // it isn't a number.
    static int ParseNearbyRadius(const std::string& value)
    {
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        {
            return kDefaultNearbyRadiusMiles;
        }
        if (value.size() > 6)
        {
            return kMaxNearbyRadiusMiles;
        }
        int miles = std::stoi(value);
        if (miles < kMinNearbyRadiusMiles)
        {
            return kMinNearbyRadiusMiles;
        }
        if (miles > kMaxNearbyRadiusMiles)
        {
            return kMaxNearbyRadiusMiles;
        }
        return miles;
    }

    // A port number from the settings file; the default if it isn't one.
    static int ParseUpstreamPort(const std::string& value)
    {
        if (value.empty() || value.size() > 5 || value.find_first_not_of("0123456789") != std::string::npos)
        {
            return kDefaultUpstreamPort;
        }
        int port = std::stoi(value);
        return port >= 1 && port <= 65535 ? port : kDefaultUpstreamPort;
    }

    AppSettings LoadSettings(const std::string& path)
    {
        AppSettings settings;

        std::ifstream file(path);
        if (!file.is_open())
        {
            return settings;
        }

        bool has_obsolete_credentials = false;
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
            else if (key == "gmrs_callsign")
            {
                settings.gmrs_callsign = value;
            }
            else if (key == "location")
            {
                settings.location = value;
            }
            else if (key == "time_format")
            {
                settings.use_24_hour_clock = value == "24h";
            }
            else if (key == "update_check")
            {
                settings.check_for_updates = value != "off";
            }
            else if (key == "nearby_radius_miles")
            {
                settings.nearby_radius_miles = ParseNearbyRadius(value);
            }
            else if (key == "upstream_host")
            {
                settings.upstream_host = value;
            }
            else if (key == "upstream_user")
            {
                settings.upstream_user = value;
            }
            else if (key == "upstream_port")
            {
                settings.upstream_port = ParseUpstreamPort(value);
            }
            else if (key == "qrz_username" || key == "qrz_password")
            {
                has_obsolete_credentials = true;
            }
        }
        file.close();

        if (has_obsolete_credentials)
        {
            SaveSettings(path, settings);
        }
        return settings;
    }

    void SaveSettings(const std::string& path, const AppSettings& settings)
    {
        std::ofstream file(path, std::ios::trunc);
        file << "callsign=" << settings.callsign << "\n";
        file << "gmrs_callsign=" << settings.gmrs_callsign << "\n";
        file << "location=" << settings.location << "\n";
        file << "time_format=" << (settings.use_24_hour_clock ? "24h" : "12h") << "\n";
        file << "nearby_radius_miles=" << settings.nearby_radius_miles << "\n";
        file << "update_check=" << (settings.check_for_updates ? "on" : "off") << "\n";
        file << "upstream_host=" << settings.upstream_host << "\n";
        file << "upstream_user=" << settings.upstream_user << "\n";
        file << "upstream_port=" << settings.upstream_port << "\n";
    }

    bool SettingsAreComplete(const AppSettings& settings)
    {
        return (!settings.callsign.empty() || !settings.gmrs_callsign.empty()) && IsZipOrPostalCode(settings.location);
    }

}  // namespace ql
