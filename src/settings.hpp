#pragma once

#include <string>

namespace ql
{

    // The Nearby Radius setting's default and allowed range, in miles.
    constexpr int kDefaultNearbyRadiusMiles = 70;
    constexpr int kMinNearbyRadiusMiles = 1;
    constexpr int kMaxNearbyRadiusMiles = 250;

    // ssh's own port, the Upstream Server window's default.
    constexpr int kDefaultUpstreamPort = 22;

    // The operator's own persistent settings: their callsign and home ZIP.
    // Deliberately kept in its own file rather than the shared SQLite
    // database, since this data must never be included when that database is
    // exported to share with another user.
    struct AppSettings
    {
        // The operator's amateur call sign, for Amateur Radio nets.
        std::string callsign;
        // And GMRS call sign, for GMRS nets. At least one of the two is
        // needed (see SettingsAreComplete). An SSH user's both come from
        // Manage Users, not from this file.
        std::string gmrs_callsign;
        // The operator's own home ZIP code: where nearby-station (ULS)
        // autocomplete measures from when the net has no ZIP of its own (see
        // Net::default_location and geo_utils.hpp) -- not shown or used
        // anywhere else. A plain 5-digit ZIP, since it's looked up directly
        // in the ZIP-centroid table.
        std::string location;
        // Show times on the 24-hour clock ("15:42") instead of the 12-hour
        // one ("03:42 PM"). Stored as time_format=24h/12h; 12-hour if absent.
        bool use_24_hour_clock = false;
        // How far from the net's ZIP (or the home ZIP) a licensed station can
        // be and still be suggested by autocomplete. Stored as
        // nearby_radius_miles=N; kDefaultNearbyRadiusMiles if absent, and
        // clamped to kMin/kMaxNearbyRadiusMiles when read.
        int nearby_radius_miles = kDefaultNearbyRadiusMiles;
        // At the console: check GitHub now and then for a newer release,
        // and say so in the top bar (see update_check.hpp). Stored as
        // update_check=on/off; on if absent.
        bool check_for_updates = true;
        // At the console: the upstream QuickLogger closed sessions are
        // pushed to (Federated Logging; see upstream_push.hpp), set in the
        // Upstream Server window. A blank host is no upstream. Stored as
        // upstream_host, upstream_user and upstream_port (22 if absent or
        // not a port number).
        std::string upstream_host;
        std::string upstream_user;
        int upstream_port = kDefaultUpstreamPort;
    };

    // Reads settings from `path`. Returns a default (empty) AppSettings if the
    // file doesn't exist yet, e.g. on first run. A file written by an older
    // version that still holds QRZ credentials (a feature since removed) is
    // rewritten without them, so a stored password doesn't linger on disk.
    AppSettings LoadSettings(const std::string& path);

    // Writes `settings` to `path`, overwriting anything already there.
    void SaveSettings(const std::string& path, const AppSettings& settings);

    // True if `settings` has every field QuickLogger requires before the
    // operator can use the rest of the app: a call sign (amateur or GMRS,
    // or both) and a well-formed
    // home ZIP or Canadian postal code (AppSettings::location -- needed for the
    // nearby-station autocomplete). Checked at startup to
    // force a first-run trip to Settings, and again before letting Settings be
    // left without saving.
    bool SettingsAreComplete(const AppSettings& settings);

}  // namespace ql
