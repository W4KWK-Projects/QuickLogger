#pragma once

#include <cstdint>
#include <string>

namespace ql
{

    class Database;

    // The station data QuickLogger downloads for itself: the FCC ULS amateur
    // license database (callsign -> name/address/license class), its
    // Canadian counterpart from ISED (Innovation, Science and Economic
    // Development Canada's amateur call sign database), the Census
    // ZIP gazetteer (ZIP -> lat/lon, for the saved-station proximity search)
    // and the Census ZIP-to-county data (for Station::county). All of it is
    // shared by every session, kept up to date automatically by the data
    // updater (see data_updater.hpp) -- never by a particular session or
    // user -- and reported through import_runs rows (see ImportRunStatus):

    // One row per dataset, recording its last load.
    constexpr const char* kUlsDataset = "uls";
    constexpr const char* kIsedDataset = "ised";
    // The FCC's GMRS licenses (l_gmrs.zip), for GMRS nets.
    constexpr const char* kGmrsDataset = "gmrs";
    constexpr const char* kZipCentroidsDataset = "zip_centroids";
    // Named "_data" rather than reusing the older "zip_counties" row, which
    // described an earlier, less accurate way of building the same table --
    // a database carrying only that older row gets the new data loaded once.
    constexpr const char* kZipCountyDataset = "zip_county_data";
    // The refresh job itself: the updater's lock and live progress report.
    constexpr const char* kDataRefreshJob = "data_refresh";

    // FCC republishes the ULS amateur database weekly; a completed load
    // older than this is due for a refresh. ISED's is refreshed as often.
    constexpr std::int64_t kUlsStalenessThresholdSeconds = std::int64_t{7} * 24 * 60 * 60;
    // How long after a failed load before it's tried again on its own.
    constexpr std::int64_t kFailedLoadRetrySeconds = std::int64_t{60} * 60;
    // A "running" job whose heartbeat is older than this belongs to a process
    // that died; another updater may take it over.
    constexpr std::int64_t kJobStaleAfterSeconds = 120;

    // Where each dataset is downloaded from. DefaultDataSources() is the real
    // addresses; tests substitute file:// URLs to local fixtures, which
    // libcurl reads the same way.
    struct DataSources
    {
        // The FCC's license file: first from uls_zip_url (QuickLogger's
        // GitHub copy), then, if that can't be downloaded or unpacked, from
        // uls_zip_fallback_url (the FCC itself) when there is one.
        std::string uls_zip_url;
        std::string uls_zip_fallback_url;
        // The FCC's GMRS license file, the same way: the GitHub copy, then
        // the FCC.
        std::string gmrs_zip_url;
        std::string gmrs_zip_fallback_url;
        std::string ised_zip_url;
        std::string zip_gazetteer_url;
        std::string zip_gazetteer_file_name;  // The file inside that zip.
        std::string zcta_county_url;
        std::string zcta_county_population_url;
        std::string zcta_county_subdivision_url;
    };
    DataSources DefaultDataSources();

    // Which datasets are due for (re)loading right now.
    struct DataRefreshPlan
    {
        bool uls = false;
        bool gmrs = false;
        bool ised = false;
        bool zip_centroids = false;
        bool zip_counties = false;
    };

    // Works out what's due: a dataset that has never loaded, whose last
    // attempt failed more than kFailedLoadRetrySeconds ago, or (the FCC and
    // ISED license data) whose last load is more than a week old. A refresh
    // requested by hand (Database::RequestImportRun on kDataRefreshJob)
    // makes the license data and anything that failed due immediately.
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
    std::string RunDataRefresh(Database* db, const std::string& db_path, const DataRefreshPlan& plan,
                               bool (*should_stop)(), const DataSources& sources);

    // Lines ("\n"-separated) for the Settings page describing the station
    // data: whether it's loaded and how current, any failure, and a refresh
    // in progress.
    std::string DescribeStationDataStatus(Database* db, std::int64_t now);

    // A short notice for the top bar of every page while the station data
    // isn't fully usable or is being refreshed ("Loading station data
    // 45%"), or an empty string when there's nothing to say. `is_problem`
    // is set when the data is missing and the last attempt failed.
    std::string DescribeStationDataNotice(Database* db, std::int64_t now, bool* is_problem);

}  // namespace ql
