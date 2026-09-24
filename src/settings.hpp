#pragma once

#include <string>

namespace ql
{

    // The operator's own persistent settings: their callsign and home ZIP.
    // Deliberately kept in its own file rather than the shared SQLite
    // database, since this data must never be included when that database is
    // exported to share with another user.
    struct AppSettings
    {
        std::string callsign;
        // The operator's own home ZIP code: where nearby-station (ULS)
        // autocomplete measures from when the net has no ZIP of its own (see
        // Net::default_location and geo_utils.hpp) -- not shown or used
        // anywhere else. A plain 5-digit ZIP, since it's looked up directly
        // in the ZIP-centroid table.
        std::string location;
        // Show times on the 24-hour clock ("15:42") instead of the 12-hour
        // one ("03:42 PM"). Stored as time_format=24h/12h; 12-hour if absent.
        bool use_24_hour_clock = false;
    };

    // Reads settings from `path`. Returns a default (empty) AppSettings if the
    // file doesn't exist yet, e.g. on first run. A file written by an older
    // version that still holds QRZ credentials (a feature since removed) is
    // rewritten without them, so a stored password doesn't linger on disk.
    AppSettings LoadSettings(const std::string& path);

    // Writes `settings` to `path`, overwriting anything already there.
    void SaveSettings(const std::string& path, const AppSettings& settings);

    // True if `settings` has every field QuickLogger requires before the
    // operator can use the rest of the app: a callsign and a well-formed
    // 5-digit home ZIP code (AppSettings::location -- needed for the
    // nearby-station autocomplete). Checked at startup to
    // force a first-run trip to Settings, and again before letting Settings be
    // left without saving.
    bool SettingsAreComplete(const AppSettings& settings);

}  // namespace ql
