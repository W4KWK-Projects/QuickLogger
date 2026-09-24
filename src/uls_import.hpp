#pragma once

#include <cstdint>
#include <string>

namespace ql
{

    class Database;

    // The station data QuickLogger downloads for itself: the FCC ULS amateur
    // license database (callsign -> name/address/license class), the Census
    // ZIP gazetteer (ZIP -> lat/lon, for the saved-station proximity search)
    // and the Census ZIP-to-county data (for Station::county). All of it is
    // shared by every session, kept up to date automatically by the data
    // updater (see data_updater.hpp) -- never by a particular session or
    // user -- and reported through import_runs rows (see ImportRunStatus):

    // One row per dataset, recording its last load.
    constexpr const char* kUlsDataset = "uls";
    constexpr const char* kZipCentroidsDataset = "zip_centroids";
    // Named "_data" rather than reusing the older "zip_counties" row, which
    // described an earlier, less accurate way of building the same table --
    // a database carrying only that older row gets the new data loaded once.
    constexpr const char* kZipCountyDataset = "zip_county_data";
    // The refresh job itself: the updater's lock and live progress report.
    constexpr const char* kDataRefreshJob = "data_refresh";

    // FCC republishes the ULS amateur database weekly; a completed load
    // older than this is due for a refresh.
    constexpr std::int64_t kUlsStalenessThresholdSeconds = std::int64_t{7} * 24 * 60 * 60;
    // How long after a failed load before it's tried again on its own.
    constexpr std::int64_t kFailedLoadRetrySeconds = std::int64_t{60} * 60;
    // A "running" job whose heartbeat is older than this belongs to a process
    // that died; another updater may take it over.
    constexpr std::int64_t kJobStaleAfterSeconds = 120;

    // Which datasets are due for (re)loading right now.
    struct DataRefreshPlan
    {
        bool uls = false;
        bool zip_centroids = false;
        bool zip_counties = false;
    };

    // Works out what's due: a dataset that has never loaded, whose last
    // attempt failed more than kFailedLoadRetrySeconds ago, or (ULS only)
    // whose last load is more than a week old. A refresh requested by hand
    // (Database::RequestImportRun on kDataRefreshJob) makes the ULS data and
    // anything that failed due immediately.
    DataRefreshPlan PlanDataRefresh(Database* db, std::int64_t now);

    // True if `plan` has anything to do.
    bool DataRefreshPlanHasWork(const DataRefreshPlan& plan);

    // Loads everything in `plan`, synchronously, reporting progress on the
    // kDataRefreshJob row (which the caller must already have claimed with
    // Database::TryClaimImportRun) and recording each dataset's outcome on
    // its own row. Calls `should_stop` regularly and gives up promptly once
    // it returns true. Downloads go into a uls_cache/ directory next to
    // `db_path`. Returns "complete", "failed" or "interrupted", for the job
    // row.
    std::string RunDataRefresh(Database* db, const std::string& db_path,
                               const DataRefreshPlan& plan, bool (*should_stop)());

    // One or two sentences for the Settings page describing the station
    // data: whether it's loaded and how current, any failure, and a refresh
    // in progress. `can_request_refresh` adds the "Press F3" hint (local
    // console only).
    std::string DescribeStationDataStatus(Database* db, std::int64_t now, bool can_request_refresh);

    // A short notice for the top bar of every page while the station data
    // isn't fully usable or is being refreshed ("Loading station data
    // 45%"), or an empty string when there's nothing to say. `is_problem`
    // is set when the data is missing and the last attempt failed.
    std::string DescribeStationDataNotice(Database* db, std::int64_t now, bool* is_problem);

}  // namespace ql
