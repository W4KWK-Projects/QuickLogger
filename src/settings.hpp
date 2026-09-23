#pragma once

#include <string>

namespace ql
{

    // The operator's own persistent settings: their callsign and optional QRZ
    // credentials. Deliberately kept in its own file rather than the shared
    // SQLite database, since this data must never be included when that
    // database is exported to share with another user.
    struct AppSettings
    {
        std::string callsign;
        std::string qrz_username;
        std::string qrz_password;
        // The operator's own home ZIP code. Used to estimate distance for
        // the saved-station form's ULS proximity autocomplete (see
        // geo_utils.hpp) -- not shown or used anywhere else. A plain 5-digit
        // ZIP, not free text like Net::default_location, since it's looked
        // up directly in the ZIP-centroid table.
        std::string location;
    };

    // Reads settings from `path`. Returns a default (empty) AppSettings if the
    // file doesn't exist yet, e.g. on first run.
    AppSettings LoadSettings(const std::string& path);

    // Writes `settings` to `path`, overwriting anything already there.
    void SaveSettings(const std::string& path, const AppSettings& settings);

    // True if `settings` has every field QuickLogger requires before the
    // operator can use the rest of the app: a callsign and a well-formed
    // 5-digit home ZIP code (AppSettings::location -- needed for the
    // saved-station form's ULS proximity autocomplete). QRZ credentials stay
    // optional. Checked at startup to force a first-run trip to Settings,
    // and again before letting Settings be left without saving.
    bool SettingsAreComplete(const AppSettings& settings);

}  // namespace ql
