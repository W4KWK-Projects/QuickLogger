#pragma once

#include <cstdint>
#include <string>

namespace ql
{

    // Where a Station's contact data came from, so the UI can decide whether
    // it's safe to overwrite with a fresh lookup or whether the user's own
    // edits should be preserved. (1 was once reserved for QRZ lookups, which
    // were never built; it's left unused so stored values keep their meaning.)
    enum class StationDataSource
    {
        kManual = 0,
        kUls = 2,
    };

    // A callsign the logger has ever seen, independent of any particular net.
    // Most fields are normally populated from the FCC ULS database, but
    // user-entered overrides are preserved when that data isn't available.
    struct Station
    {
        std::string callsign;
        std::string name;
        std::string member_id;  // Also used as "Spotter ID" by Skywarn nets.
        std::string street_address;
        std::string city;
        std::string county;
        std::string state;
        std::string zip;
        std::string grid_square;
        std::string license_class;
        std::string email;
        StationDataSource data_source = StationDataSource::kManual;
        std::int64_t last_updated = 0;  // Unix timestamp.
    };

    // Which of a NetInstance's three role-callsign fields a callsign fills.
    // Used both for the operator's own role (NetInstance::operator_role,
    // chosen once when the net is started) and for a check-in's optional
    // designation as one of the *other* two roles (CheckIn::designated_role,
    // set from the New Station modal or later via Edit Check-in). kRoleNone
    // only makes sense for the latter -- a NetInstance always has an
    // operator, but a check-in usually holds no extra role at all.
    constexpr int kRoleNetControl = 0;
    constexpr int kRoleAlternateNetControl = 1;
    constexpr int kRoleLogger = 2;
    constexpr int kRoleNone = -1;

    // A recurring net definition, e.g. "Skywarn Net, Tuesdays 8pm ET".
    // Holds the defaults that seed each new NetInstance.
    struct Net
    {
        std::int64_t id = 0;
        std::string name;
        std::string mode;
        std::string default_frequency;
        // The net's 5-digit ZIP, or empty. Nearby-station autocomplete measures
        // from it, falling back to the operator's home ZIP. Free text from
        // before this was a ZIP field is converted on upgrade and on import
        // (see Database::NormalizeNetZips).
        std::string default_location;
        std::string default_grid_square;  // Resolved from default_location.
        std::string recurrence_description;
        std::string notes;
        // Unix timestamps: when this net was created here, and -- for a net
        // brought in from a .qlnet file -- when it was imported. Shown in the
        // net list so two nets with the same name (e.g. your own and one
        // someone sent you) can be told apart. 0 = not known (a net created
        // before these were recorded; shown blank, never guessed at).
        std::int64_t created_at = 0;
        std::int64_t imported_at = 0;
    };

    enum class NetInstanceStatus
    {
        kOpen = 0,
        kClosed = 1,
    };

    // One dated occurrence of a Net (e.g. this Tuesday's Skywarn net).
    struct NetInstance
    {
        std::int64_t id = 0;
        std::int64_t net_id = 0;
        std::string instance_date;  // ISO-8601 date, e.g. "2026-09-22".
        std::string net_control_callsign;
        std::string alternate_net_control_callsign;
        std::string logger_callsign;
        std::string created_by;  // The logged-in user's own callsign.
        std::string frequency;   // Overrides Net::default_frequency when set.
        std::string location;    // Overrides Net::default_location when set.
        NetInstanceStatus status = NetInstanceStatus::kOpen;
        std::int64_t closed_at = 0;  // Unix timestamp; 0 while still open.
        // Unix timestamp of the moment the net was started (the date half of
        // that moment is instance_date). 0 for a net logged before start
        // times were recorded -- shown as blank, never guessed at.
        std::int64_t started_at = 0;
        // Which of the three role-callsign fields above the operator claimed
        // when starting this net (kRoleNetControl/kRoleAlternateNetControl/
        // kRoleLogger). A check-in can be designated as one of the *other*
        // two roles (see CheckIn::designated_role) but never this one.
        int operator_role = kRoleNetControl;
    };

    // One station's check-in during a specific NetInstance. Signal report
    // lives here, not on Station, because it varies per check-in.
    struct CheckIn
    {
        std::int64_t id = 0;
        std::int64_t net_instance_id = 0;
        std::string callsign;
        int sequence_number = 0;  // Order this station checked in, within the net.
        std::string signal_report;
        std::string remarks;
        std::string comment;
        std::int64_t checked_in_at = 0;  // Unix timestamp.
        // Optional: this check-in additionally serving as Alternate Net
        // Control or Logger (never the operator's own role -- see
        // NetInstance::operator_role). kRoleNone means no extra role. At
        // most one check-in per NetInstance holds a given role at a time;
        // see ApplyCheckInRoleDesignation.
        int designated_role = kRoleNone;
    };

    // Persisted state of one background data job, keyed by `source`. Two
    // kinds of row share this shape (see data_updater.hpp):
    //   - one row per dataset ("uls", "zip_centroids", "zip_county_data")
    //     recording the outcome of its last load -- status, when, how many
    //     records, and the error if it failed;
    //   - the "data_refresh" row, which is the updater's lock and live
    //     progress report: while `status` is "running" it's rewritten every
    //     few seconds with the current phase/percent and a fresh
    //     heartbeat_at, so every session can show progress by reading it,
    //     and a run whose heartbeat stops (its process was killed) can be
    //     told apart from one that's still going.
    struct ImportRunStatus
    {
        std::string source;                // e.g. "uls".
        std::string status = "never_run";  // "never_run" | "running" | "complete" | "failed"
                                           // | "interrupted".
        std::int64_t started_at = 0;       // Unix timestamp.
        std::int64_t completed_at = 0;     // Unix timestamp; 0 if never completed.
        std::int64_t records_imported = 0;
        std::string last_error;
        std::string phase;              // While running: e.g. "Downloading".
        int percent = 0;                // While running: 0-100.
        std::int64_t heartbeat_at = 0;  // While running: last progress write.
        std::int64_t requested_at = 0;  // When a refresh was last asked for by hand.
    };

    // Approximate center point of a US ZIP code (from the Census Bureau's
    // ZCTA gazetteer), used to estimate distance for the saved-station
    // form's ULS proximity autocomplete. See Database::GetAllZipCentroids
    // and AppState::zip_centroids_cache (loaded once, not queried live).
    struct ZipCentroid
    {
        std::string zip;
        double lat = 0.0;
        double lon = 0.0;
    };

    // A ZIP code near the operator, and how far its centroid is from theirs
    // -- see NearbyZips in geo_utils.hpp.
    struct NearbyZip
    {
        std::string zip;
        double miles = 0.0;
    };

    // A ULS station found by Database::SearchNearbyUlsStations, with its
    // distance from the operator in miles (-1 if its ZIP has no centroid on
    // file, so its distance isn't known).
    struct NearbyUlsStation
    {
        Station station;
        double miles = -1.0;
    };

    // Just the callsign and distance of a nearby ULS station: what the
    // autocomplete keeps in memory for every licensee near the net (see
    // AppState::nearby_uls_callsigns), with the rest of a station's details
    // looked up only for the few that match what's typed.
    struct NearbyUlsCallsign
    {
        std::string callsign;
        double miles = -1.0;
    };

    // The county a US ZIP code is in, for filling in Station::county (FCC's
    // ULS data has none). For a ZIP that crosses a county line, this is the
    // county where most of its residents live -- see FetchAndLoadZipCounties
    // in uls_import.cpp. Loaded once per process into
    // AppState::zip_county_by_zip rather than queried live.
    struct ZipCounty
    {
        std::string zip;
        std::string county;
    };

    // For a ZIP that crosses a county line: a town/city/township inside it
    // and the county that town is in, so a station whose address names that
    // town gets the right county even when most of the ZIP's residents live
    // in a different one. `place` is uppercase with its Census suffix
    // removed ("NEWTON", not "Newton city"), ready to compare against a
    // station's city. Loaded alongside ZipCounty; see
    // AppState::zip_place_county_by_key.
    struct ZipPlaceCounty
    {
        std::string zip;
        std::string place;
        std::string county;
    };

    // A login identity for the built-in SSH server (see ssh_server.hpp) --
    // not the same thing as a Station/operator callsign, though `username`
    // is typically chosen to match one. `public_key` is a single-line
    // OpenSSH authorized_keys-style string ("ssh-ed25519 AAAA... comment"),
    // the exact format an operator already has in their own
    // ~/.ssh/id_ed25519.pub. Deliberately holds nothing else: this table is
    // global/shared identity data like Net, unlike AppSettings (see
    // settings.hpp), which becomes per-username once a user is SSH'd in
    // rather than living here.
    struct User
    {
        std::string username;
        std::string public_key;
        std::int64_t created_at = 0;
        std::int64_t last_login_at = 0;
    };

}  // namespace ql
