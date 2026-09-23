#pragma once

#include <cstdint>
#include <string>

namespace ql
{

    // Where a Station's contact data came from, so the UI can decide whether
    // it's safe to overwrite with a fresh lookup or whether the user's own
    // edits should be preserved.
    enum class StationDataSource
    {
        kManual = 0,
        kQrz = 1,
        kUls = 2,
    };

    // A callsign the logger has ever seen, independent of any particular net.
    // Most fields are normally populated from QRZ/the FCC ULS database, but
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
        std::string default_location;     // City/county/ZIP as entered by the user.
        std::string default_grid_square;  // Resolved from default_location.
        std::string recurrence_description;
        std::string notes;
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

    // Persisted state of one bulk-import job (e.g. "uls"), so its ongoing/
    // complete status survives page navigation and app restarts. See
    // Database::GetImportRunStatus/UpsertImportRunStatus.
    struct ImportRunStatus
    {
        std::string source;                // e.g. "uls".
        std::string status = "never_run";  // "never_run" | "running" | "complete" | "failed".
        std::int64_t started_at = 0;       // Unix timestamp.
        std::int64_t completed_at = 0;     // Unix timestamp; 0 if never completed.
        std::int64_t records_imported = 0;
        std::string last_error;
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

    // Which county a US ZIP code falls in, from the Census Bureau's ZCTA-to-
    // county relationship file. A ZCTA can span more than one county; this
    // is whichever county has the largest land-area overlap with it (see
    // FetchAndLoadZipCounties). Used to backfill Station::county for a
    // ULS-sourced station, which has no county field at all in FCC's data
    // -- see BackfillCountyFromZip and AppState::zip_county_by_zip (loaded
    // once, not queried live, same rationale as zip_centroids_cache).
    struct ZipCounty
    {
        std::string zip;
        std::string county;
    };

    // A (city, state) -> county mapping derived entirely from data already
    // on hand: for every city/state pair seen in uls_stations, whichever
    // county its member ZIPs' zip_counties entries agree on most often (see
    // Database::ComputeCityCounties). A city is unambiguous even when one
    // of its ZIPs straddles a county line close to evenly -- which is
    // exactly the case ZipCounty's own area-based ZIP lookup can get wrong
    // (see kZipCountyOverrides in uls_import.cpp) -- so BackfillCountyFromZip
    // prefers this over the raw per-ZIP lookup when a station's city/state
    // are known. Not persisted as a table: recomputed into
    // AppState::city_county_by_city_state once per process run, same
    // loaded-once-and-cached rationale as zip_county_by_zip.
    struct CityCounty
    {
        std::string city;
        std::string state;
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
