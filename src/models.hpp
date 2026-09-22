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

}  // namespace ql
