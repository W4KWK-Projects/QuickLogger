#include "uls_import.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "date_utils.hpp"
#include "db/database.hpp"
#include "file_export.hpp"
#include "models.hpp"
#include "ui/app_state.hpp"
#include "zip_extract.hpp"

namespace ql
{

    static const char* kUlsZipUrl = "https://data.fcc.gov/download/pub/uls/complete/l_amat.zip";

    // Field positions below are 0-indexed and were confirmed against a real
    // downloaded l_amat.zip (FCC PUBACC amateur database), not assumed from
    // memory: HD.dat/EN.dat/AM.dat all join 1:1:1 on unique_system_identifier
    // (field 1 in every file), with no duplicate ids in EN.dat or AM.dat.

    // HD.dat ("HD" record): license header, one row per license.
    static constexpr std::size_t kHdUniqueSystemId = 1;
    static constexpr std::size_t kHdCallSign = 4;
    static constexpr std::size_t kHdLicenseStatus = 5;  // "A" = Active (also seen: E/C/T).

    // EN.dat ("EN" record): entity/name/address.
    static constexpr std::size_t kEnUniqueSystemId = 1;
    static constexpr std::size_t kEnEntityName = 7;  // Already "LAST, FIRST MI" or club name.
    static constexpr std::size_t kEnStreetAddress = 15;
    static constexpr std::size_t kEnCity = 16;
    static constexpr std::size_t kEnState = 17;
    static constexpr std::size_t kEnZip = 18;

    // AM.dat ("AM" record): amateur-specific data.
    static constexpr std::size_t kAmUniqueSystemId = 1;
    static constexpr std::size_t kAmOperatorClass = 5;

    static std::string UlsCacheDir(const std::string& db_path)
    {
        std::string::size_type slash = db_path.find_last_of('/');
        std::string dir = slash == std::string::npos ? std::string(".") : db_path.substr(0, slash);
        return dir + "/uls_cache";
    }

    static std::vector<std::string> SplitPipeDelimited(const std::string& line)
    {
        std::vector<std::string> fields;
        std::string::size_type start = 0;
        while (true)
        {
            std::string::size_type pipe = line.find('|', start);
            if (pipe == std::string::npos)
            {
                fields.push_back(line.substr(start));
                break;
            }
            fields.push_back(line.substr(start, pipe - start));
            start = pipe + 1;
        }
        return fields;
    }

    static std::string FieldOrEmpty(const std::vector<std::string>& fields, std::size_t index)
    {
        return index < fields.size() ? fields[index] : std::string();
    }

    // FCC's EN.dat zip field is sometimes a 9-digit ZIP+4 with no separator
    // (e.g. "374152623" for "37415-2623") rather than a plain 5-digit ZIP --
    // confirmed against a real downloaded l_amat.zip. Every consumer of
    // Station::zip in this app (the zip_centroids/zip_counties lookups, the
    // saved-station form's ULS proximity tier) keys off a plain 5-digit
    // ZIP, so normalize once here at the source rather than in every
    // consumer.
    static std::string NormalizeZip5(const std::string& zip)
    {
        return zip.size() > 5 ? zip.substr(0, 5) : zip;
    }

    static std::string LicenseClassFromCode(const std::string& code)
    {
        if (code == "T")
        {
            return "Technician";
        }
        if (code == "G")
        {
            return "General";
        }
        if (code == "E")
        {
            return "Extra";
        }
        if (code == "A")
        {
            return "Advanced";
        }
        if (code == "N")
        {
            return "Novice";
        }
        if (code == "P")
        {
            return "Technician Plus";
        }
        return "";
    }

    static std::string FormatTimestamp(std::int64_t unix_time)
    {
        if (unix_time <= 0)
        {
            return "";
        }
        std::tm local_time = LocalTime(static_cast<std::time_t>(unix_time));
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %I:%M %p", &local_time);
        return std::string(buffer);
    }

    static std::int64_t CountLines(const std::string& path)
    {
        std::ifstream file(path);
        std::int64_t count = 0;
        std::string line;
        while (std::getline(file, line))
        {
            ++count;
        }
        return count;
    }

    static size_t CurlWriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        std::FILE* file = static_cast<std::FILE*>(userdata);
        return std::fwrite(ptr, size, nmemb, file);
    }

    static int CurlProgressCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                                    curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
    {
        UlsImportProgress* progress = static_cast<UlsImportProgress*>(userdata);
        if (dltotal > 0)
        {
            // Download maps to the first 70% of the overall progress bar --
            // see ParseAndImport for the remaining 75-100% (extraction gets a
            // fixed jump to 70-75%, set directly by the caller).
            int percent = static_cast<int>((dlnow * 70) / dltotal);
            progress->percent.store(percent);
        }
        return 0;
    }

    static bool DownloadUlsZip(const std::string& zip_path, UlsImportProgress* progress,
                               std::string* error)
    {
        std::FILE* file = std::fopen(zip_path.c_str(), "wb");
        if (file == nullptr)
        {
            *error = "Failed to open " + zip_path + " for writing.";
            return false;
        }

        CURL* curl = curl_easy_init();
        if (curl == nullptr)
        {
            std::fclose(file);
            *error = "Failed to initialize libcurl.";
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, kUlsZipUrl);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "QuickLogger/1.0");

        CURLcode result = curl_easy_perform(curl);
        std::fclose(file);
        curl_easy_cleanup(curl);

        if (result != CURLE_OK)
        {
            *error = std::string("Download failed: ") + curl_easy_strerror(result);
            return false;
        }
        return true;
    }

    static bool ExtractUlsZip(const std::string& cache_dir, const std::string& zip_path,
                              std::string* error)
    {
        if (!ExtractZipEntries(zip_path, cache_dir, {"HD.dat", "EN.dat", "AM.dat"}, error))
        {
            *error = "Failed to extract the FCC ULS archive: " + *error;
            return false;
        }
        for (const char* name : {"HD.dat", "EN.dat", "AM.dat"})
        {
            std::ifstream check(cache_dir + "/" + name);
            if (!check.good())
            {
                *error = std::string("Extraction did not produce ") + name + ".";
                return false;
            }
        }
        return true;
    }

    struct UlsEntityFields
    {
        std::string name;
        std::string street_address;
        std::string city;
        std::string state;
        std::string zip;
    };

    static std::unordered_map<std::string, UlsEntityFields> ParseEntityFile(const std::string& path)
    {
        std::unordered_map<std::string, UlsEntityFields> result;
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line))
        {
            std::vector<std::string> fields = SplitPipeDelimited(line);
            if (fields.empty() || fields[0] != "EN")
            {
                continue;
            }
            UlsEntityFields entity;
            entity.name = FieldOrEmpty(fields, kEnEntityName);
            entity.street_address = FieldOrEmpty(fields, kEnStreetAddress);
            entity.city = FieldOrEmpty(fields, kEnCity);
            entity.state = FieldOrEmpty(fields, kEnState);
            entity.zip = NormalizeZip5(FieldOrEmpty(fields, kEnZip));
            result[FieldOrEmpty(fields, kEnUniqueSystemId)] = entity;
        }
        return result;
    }

    static std::unordered_map<std::string, std::string> ParseAmateurFile(const std::string& path)
    {
        std::unordered_map<std::string, std::string> result;
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line))
        {
            std::vector<std::string> fields = SplitPipeDelimited(line);
            if (fields.empty() || fields[0] != "AM")
            {
                continue;
            }
            result[FieldOrEmpty(fields, kAmUniqueSystemId)] =
                LicenseClassFromCode(FieldOrEmpty(fields, kAmOperatorClass));
        }
        return result;
    }

    static bool ParseAndImport(const std::string& cache_dir, Database* db,
                               UlsImportProgress* progress, std::int64_t* out_records,
                               std::string* error)
    {
        std::unordered_map<std::string, UlsEntityFields> entities =
            ParseEntityFile(cache_dir + "/EN.dat");
        std::unordered_map<std::string, std::string> license_classes =
            ParseAmateurFile(cache_dir + "/AM.dat");

        std::string hd_path = cache_dir + "/HD.dat";
        std::int64_t total_lines = CountLines(hd_path);
        if (total_lines <= 0)
        {
            *error = "HD.dat was empty.";
            return false;
        }

        std::ifstream file(hd_path);
        if (!file.good())
        {
            *error = "Failed to open HD.dat.";
            return false;
        }

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        constexpr std::size_t kBatchSize = 1000;
        std::vector<Station> batch;
        batch.reserve(kBatchSize);
        std::int64_t imported = 0;
        std::int64_t line_number = 0;
        std::string line;
        while (std::getline(file, line))
        {
            ++line_number;
            std::vector<std::string> fields = SplitPipeDelimited(line);
            if (fields.size() <= kHdLicenseStatus || fields[0] != "HD")
            {
                continue;
            }
            if (FieldOrEmpty(fields, kHdLicenseStatus) != "A")
            {
                continue;
            }
            std::string call_sign = FieldOrEmpty(fields, kHdCallSign);
            if (call_sign.empty())
            {
                continue;
            }
            std::string id = FieldOrEmpty(fields, kHdUniqueSystemId);

            Station station;
            station.callsign = call_sign;

            std::unordered_map<std::string, UlsEntityFields>::const_iterator entity_it =
                entities.find(id);
            if (entity_it != entities.end())
            {
                station.name = entity_it->second.name;
                station.street_address = entity_it->second.street_address;
                station.city = entity_it->second.city;
                station.state = entity_it->second.state;
                station.zip = entity_it->second.zip;
            }

            std::unordered_map<std::string, std::string>::const_iterator class_it =
                license_classes.find(id);
            if (class_it != license_classes.end())
            {
                station.license_class = class_it->second;
            }

            batch.push_back(station);
            ++imported;

            if (batch.size() >= kBatchSize)
            {
                db->BulkUpsertUlsStations(batch, now);
                batch.clear();
                progress->records_imported.store(imported);
                int percent = 75 + static_cast<int>((line_number * 25) / total_lines);
                progress->percent.store(percent > 100 ? 100 : percent);
            }
        }
        if (!batch.empty())
        {
            db->BulkUpsertUlsStations(batch, now);
        }

        progress->records_imported.store(imported);
        *out_records = imported;
        return true;
    }

    // Census Bureau ZCTA gazetteer: approximate lat/lon centroid per US ZIP
    // code, used to estimate distance for the saved-station form's ULS
    // proximity autocomplete (see DescribeUlsImportStatus's callers and
    // RefreshSavedStationSuggestions in app_state.cpp). Centroids are
    // effectively permanent (population-weighted ZIP centers barely move
    // year to year), so this is fetched once and never re-fetched, unlike
    // the ULS license data. The filename is year-prefixed by Census; if this
    // URL 404s in the future, it needs updating to a newer edition (and the
    // matching extracted filename below).
    static const char* kZipGazetteerUrl =
        "https://www2.census.gov/geo/docs/maps-data/data/gazetteer/2023_Gazetteer/"
        "2023_Gaz_zcta_national.zip";
    static const char* kZipGazetteerFileName = "2023_Gaz_zcta_national.txt";

    // Census Bureau ZCTA-to-county relationship file: which county(ies) each
    // ZIP code overlaps, used to backfill Station::county for a ULS-sourced
    // station (see BackfillCountyFromZip in app_state.cpp) -- FCC's ULS data
    // has no county field anywhere in it (confirmed against a real
    // downloaded l_amat.zip: EN.dat's 30 fields cover only street/city/
    // state/zip, and the only other candidate file, CO.dat, turned out to be
    // license status "Comments", not counties). Unlike the gazetteer above,
    // this is served as a plain pipe-delimited text file, not zipped. County
    // boundaries are effectively permanent, so -- like the centroids -- this
    // is fetched once and never re-fetched.
    static const char* kZctaCountyUrl =
        "https://www2.census.gov/geo/docs/maps-data/data/rel2020/zcta520/"
        "tab20_zcta520_county20_natl.txt";

    static std::vector<std::string> SplitTabDelimited(const std::string& line)
    {
        std::vector<std::string> fields;
        std::string::size_type start = 0;
        while (true)
        {
            std::string::size_type tab = line.find('\t', start);
            if (tab == std::string::npos)
            {
                fields.push_back(line.substr(start));
                break;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        return fields;
    }

    static bool DownloadFileNoProgress(const std::string& url, const std::string& dest_path,
                                       std::string* error)
    {
        std::FILE* file = std::fopen(dest_path.c_str(), "wb");
        if (file == nullptr)
        {
            *error = "Failed to open " + dest_path + " for writing.";
            return false;
        }

        CURL* curl = curl_easy_init();
        if (curl == nullptr)
        {
            std::fclose(file);
            *error = "Failed to initialize libcurl.";
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "QuickLogger/1.0");

        CURLcode result = curl_easy_perform(curl);
        std::fclose(file);
        curl_easy_cleanup(curl);

        if (result != CURLE_OK)
        {
            *error = std::string("Gazetteer download failed: ") + curl_easy_strerror(result);
            return false;
        }
        return true;
    }

    static bool FetchAndLoadZipCentroids(const std::string& cache_dir, Database* db,
                                         std::string* error)
    {
        std::string zip_path = cache_dir + "/zip_gazetteer.zip";
        if (!DownloadFileNoProgress(kZipGazetteerUrl, zip_path, error))
        {
            return false;
        }

        if (!ExtractZipEntries(zip_path, cache_dir, {kZipGazetteerFileName}, error))
        {
            *error = "Failed to extract the ZIP gazetteer archive: " + *error;
            return false;
        }

        std::string txt_path = cache_dir + "/" + kZipGazetteerFileName;
        std::ifstream file(txt_path);
        if (!file.good())
        {
            *error = "Extraction did not produce " + std::string(kZipGazetteerFileName) + ".";
            return false;
        }

        std::string line;
        std::getline(file, line);  // Header row.
        constexpr std::size_t kBatchSize = 1000;
        std::vector<ZipCentroid> batch;
        batch.reserve(kBatchSize);
        while (std::getline(file, line))
        {
            std::vector<std::string> fields = SplitTabDelimited(line);
            if (fields.size() < 7)
            {
                continue;
            }
            ZipCentroid centroid;
            centroid.zip = fields[0];
            try
            {
                centroid.lat = std::stod(fields[5]);
                centroid.lon = std::stod(fields[6]);
            }
            catch (const std::exception&)
            {
                continue;
            }
            batch.push_back(centroid);
            if (batch.size() >= kBatchSize)
            {
                db->BulkUpsertZipCentroids(batch);
                batch.clear();
            }
        }
        if (!batch.empty())
        {
            db->BulkUpsertZipCentroids(batch);
        }
        return true;
    }

    // The relationship file's NAMELSAD_COUNTY_20 field spells out the full
    // legal name (e.g. "Hamilton County"), but the app just wants the bare
    // name to display (e.g. "Hamilton") -- the column header/context already
    // says "County".
    static std::string StripCountySuffix(const std::string& county)
    {
        const std::string suffix = " County";
        if (county.size() > suffix.size() &&
            county.compare(county.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            return county.substr(0, county.size() - suffix.size());
        }
        return county;
    }

    // A ZCTA that straddles a county line has its land area split across
    // both counties' rows in the relationship file; FetchAndLoadZipCounties
    // picks whichever split has the larger AREALAND_PART, which is right
    // for the overwhelming majority of ZIPs but can disagree with the
    // county the ZIP is actually associated with (its USPS-designated city,
    // where its addresses/population actually are) when the split is close
    // to even -- there's no population or address data in this file to
    // break the tie correctly. Confirmed 2026-09-22: ZIP 37419 (Chattanooga,
    // TN) splits ~52%/48% Marion/Hamilton by land area, but is a Hamilton
    // County ZIP by every practical measure (a real user's own callsign is
    // registered there). Rather than a general fix (would need real
    // population-weighted data, e.g. HUD's USPS crosswalk, which requires
    // registration), corrected by hand as each is found.
    struct ZipCountyOverride
    {
        const char* zip;
        const char* county;
    };
    // A plain constant array rather than a std::unordered_map, so there's
    // nothing to construct (or fail to construct) at program startup; it's
    // a handful of entries, searched once per ZIP during an import.
    static constexpr ZipCountyOverride kZipCountyOverrides[] = {
        {"37419", "Hamilton"},
    };

    // The hand-corrected county for `zip`, or nullptr if it has none.
    static const char* ZipCountyOverrideFor(const std::string& zip)
    {
        for (const ZipCountyOverride& entry : kZipCountyOverrides)
        {
            if (zip == entry.zip)
            {
                return entry.county;
            }
        }
        return nullptr;
    }

    static bool FetchAndLoadZipCounties(const std::string& cache_dir, Database* db,
                                        std::string* error)
    {
        std::string txt_path = cache_dir + "/zcta_county.txt";
        if (!DownloadFileNoProgress(kZctaCountyUrl, txt_path, error))
        {
            return false;
        }

        std::ifstream file(txt_path);
        if (!file.good())
        {
            *error = "Failed to open the downloaded ZCTA-county file.";
            return false;
        }

        std::string line;
        std::getline(file, line);  // Header row.

        // A ZCTA can span more than one county (see tab20_zcta520_county20_natl's
        // format); keep whichever has the largest land-area overlap
        // (AREALAND_PART, field 16) as that ZIP's county.
        std::unordered_map<std::string, std::string> best_county_by_zip;
        std::unordered_map<std::string, double> best_area_by_zip;
        while (std::getline(file, line))
        {
            std::vector<std::string> fields = SplitPipeDelimited(line);
            if (fields.size() < 17)
            {
                continue;
            }
            const std::string& zip = fields[1];
            const std::string& county = fields[10];
            if (zip.empty() || county.empty())
            {
                continue;
            }
            double area = 0.0;
            try
            {
                area = std::stod(fields[16]);
            }
            catch (const std::exception&)
            {
                continue;
            }

            std::unordered_map<std::string, double>::const_iterator existing =
                best_area_by_zip.find(zip);
            if (existing == best_area_by_zip.end() || area > existing->second)
            {
                best_area_by_zip[zip] = area;
                best_county_by_zip[zip] = county;
            }
        }

        std::vector<ZipCounty> batch;
        batch.reserve(best_county_by_zip.size());
        for (const std::pair<const std::string, std::string>& entry : best_county_by_zip)
        {
            ZipCounty zip_county;
            zip_county.zip = entry.first;
            const char* override_county = ZipCountyOverrideFor(entry.first);
            zip_county.county = override_county != nullptr ? std::string(override_county)
                                                           : StripCountySuffix(entry.second);
            batch.push_back(zip_county);
        }
        db->BulkUpsertZipCounties(batch);
        return true;
    }

    // Runs the whole fetch->extract->parse->import pipeline on whatever
    // thread calls operator()(). A named functor (not a lambda) per this
    // project's coding convention, spawned via std::thread(...).detach() by
    // StartUlsImport.
    class UlsImportWorker
    {
    public:
        UlsImportWorker(std::string db_path, UlsImportProgress* progress,
                        ftxui::ScreenInteractive* screen)
            : db_path_(std::move(db_path)), progress_(progress), screen_(screen)
        {
        }

        void operator()() const
        {
            std::string cache_dir = UlsCacheDir(db_path_);
            EnsureDirectory(cache_dir);
            std::string zip_path = cache_dir + "/l_amat.zip";
            std::int64_t started_at = static_cast<std::int64_t>(std::time(nullptr));
            std::string error;

            progress_->phase.store(UlsImportPhase::kDownloading);
            PostRedraw();
            bool ok = DownloadUlsZip(zip_path, progress_, &error);

            if (ok)
            {
                progress_->phase.store(UlsImportPhase::kExtracting);
                progress_->percent.store(70);
                PostRedraw();
                ok = ExtractUlsZip(cache_dir, zip_path, &error);
            }

            if (ok)
            {
                progress_->phase.store(UlsImportPhase::kParsing);
                progress_->percent.store(75);
                PostRedraw();
            }

            ImportRunStatus status;
            status.source = "uls";
            status.started_at = started_at;
            std::int64_t records_imported = 0;

            try
            {
                Database db(db_path_);
                if (ok)
                {
                    ok = ParseAndImport(cache_dir, &db, progress_, &records_imported, &error);
                }
                if (ok && !db.HasAnyZipCentroids())
                {
                    // Best-effort and independent of the ULS import's own
                    // success/failure status: if this fails, the saved-station
                    // form's proximity autocomplete just has no data yet
                    // rather than the whole import being reported as failed.
                    // Tracked as its own import_runs row (source
                    // "zip_centroids") specifically so a failure here is
                    // visible and retried on the *next* F3/auto-trigger even
                    // if ULS itself already shows "complete" and won't be
                    // re-attempted again for up to a week -- see
                    // DescribeUlsImportStatus, which surfaces this row.
                    std::string geocode_error;
                    bool geocode_ok = FetchAndLoadZipCentroids(cache_dir, &db, &geocode_error);
                    ImportRunStatus geocode_status;
                    geocode_status.source = "zip_centroids";
                    geocode_status.started_at = started_at;
                    geocode_status.completed_at = static_cast<std::int64_t>(std::time(nullptr));
                    geocode_status.status = geocode_ok ? "complete" : "failed";
                    geocode_status.last_error = geocode_ok ? "" : geocode_error;
                    db.UpsertImportRunStatus(geocode_status);
                }
                if (ok && !db.HasAnyZipCounties())
                {
                    // Same independent, best-effort treatment as the
                    // centroid geocode step above, and for the same reason:
                    // a failure here shouldn't fail the whole ULS import,
                    // just leave Station::county unbackfilled for ULS-sourced
                    // stations until the next retry.
                    std::string county_error;
                    bool county_ok = FetchAndLoadZipCounties(cache_dir, &db, &county_error);
                    ImportRunStatus county_status;
                    county_status.source = "zip_counties";
                    county_status.started_at = started_at;
                    county_status.completed_at = static_cast<std::int64_t>(std::time(nullptr));
                    county_status.status = county_ok ? "complete" : "failed";
                    county_status.last_error = county_ok ? "" : county_error;
                    db.UpsertImportRunStatus(county_status);
                }
                status.completed_at = static_cast<std::int64_t>(std::time(nullptr));
                status.records_imported = records_imported;
                status.status = ok ? "complete" : "failed";
                status.last_error = ok ? "" : error;
                db.UpsertImportRunStatus(status);
            }
            catch (const std::exception& e)
            {
                ok = false;
                status.completed_at = static_cast<std::int64_t>(std::time(nullptr));
                status.records_imported = records_imported;
                status.status = "failed";
                status.last_error = e.what();
                try
                {
                    Database fallback_db(db_path_);
                    fallback_db.UpsertImportRunStatus(status);
                }
                // NOLINTNEXTLINE(bugprone-empty-catch): deliberately ignored, see below.
                catch (const std::exception&)
                {
                    // Nothing more we can do; the in-memory progress below
                    // still reflects the failure for this run.
                }
            }

            progress_->phase.store(ok ? UlsImportPhase::kComplete : UlsImportPhase::kFailed);
            progress_->percent.store(ok ? 100 : progress_->percent.load());
            if (!ok)
            {
                std::lock_guard<std::mutex> lock(progress_->error_mutex);
                progress_->last_error = status.last_error;
            }
            progress_->running.store(false);
            PostRedraw();
        }

    private:
        void PostRedraw() const
        {
            if (screen_ != nullptr)
            {
                screen_->PostEvent(ftxui::Event::Custom);
            }
        }

        std::string db_path_;
        UlsImportProgress* progress_;
        ftxui::ScreenInteractive* screen_;
    };

    void StartUlsImport(const std::string& db_path, UlsImportProgress* progress,
                        ftxui::ScreenInteractive* screen)
    {
        if (progress->running.load())
        {
            return;
        }

        std::int64_t started_at = static_cast<std::int64_t>(std::time(nullptr));
        try
        {
            Database db(db_path);
            if (!db.TryClaimImportRun("uls", started_at))
            {
                // Another connection -- most likely a second instance of the
                // app pointed at this same quicklogger.db, auto-triggering
                // at startup within the same moment -- already claimed this
                // import. Don't launch a second worker: it would download to
                // and unzip into the same uls_cache/ files the other
                // worker's already using, and race it writing the same
                // uls_stations rows.
                return;
            }
        }
        // NOLINTNEXTLINE(bugprone-empty-catch): deliberately ignored, see below.
        catch (const std::exception&)
        {
            // Couldn't even check -- fall through and let the worker try its
            // own connection and report its own failure, same as before.
        }

        progress->running.store(true);
        progress->percent.store(0);
        progress->phase.store(UlsImportPhase::kDownloading);

        std::thread(UlsImportWorker(db_path, progress, screen)).detach();
    }

    std::string DescribeUlsImportStatus(const AppState* state)
    {
        const UlsImportProgress& progress = state->uls_import_progress;
        if (progress.running.load())
        {
            UlsImportPhase phase = progress.phase.load();
            int percent = progress.percent.load();
            if (phase == UlsImportPhase::kDownloading)
            {
                return "ULS import: in progress -- downloading (" + std::to_string(percent) + "%).";
            }
            if (phase == UlsImportPhase::kExtracting)
            {
                return "ULS import: in progress -- extracting (" + std::to_string(percent) + "%).";
            }
            std::int64_t records = progress.records_imported.load();
            return "ULS import: in progress -- importing (" + std::to_string(percent) + "%, " +
                   std::to_string(records) + " records so far).";
        }

        // Not running: read the persisted row fresh rather than caching it on
        // AppState, so the displayed status can't go stale while sitting on
        // the Settings page across the exact frame a background run finishes
        // (same "just query it, it's cheap" precedent as NetHistoryRenderer).
        std::optional<ImportRunStatus> status = state->db->GetImportRunStatus("uls");
        if (status.has_value() && status->status == "failed")
        {
            return "ULS import failed at " + FormatTimestamp(status->completed_at) + ": " +
                   status->last_error + ". Press F3 to retry.";
        }
        if (status.has_value() && status->status == "complete")
        {
            std::string message = "ULS station database: last updated " +
                                  FormatTimestamp(status->completed_at) + ", " +
                                  std::to_string(status->records_imported) + " records.";
            std::optional<ImportRunStatus> geocode_status =
                state->db->GetImportRunStatus("zip_centroids");
            if (geocode_status.has_value() && geocode_status->status == "failed")
            {
                message += " ZIP proximity data failed to load (" + geocode_status->last_error +
                           "); saved-station ULS matches are unavailable until this succeeds. "
                           "Press F3 to retry.";
            }
            std::optional<ImportRunStatus> county_status =
                state->db->GetImportRunStatus("zip_counties");
            if (county_status.has_value() && county_status->status == "failed")
            {
                message += " ZIP-to-county data failed to load (" + county_status->last_error +
                           "); County won't be filled in for ULS-sourced stations until this "
                           "succeeds. Press F3 to retry.";
            }
            return message;
        }
        return "ULS station database: never imported. Press F3 to import now (~1M+ records, "
               "several minutes).";
    }

    bool ShouldAutoStartUlsImport(const std::optional<ImportRunStatus>& status, std::int64_t now)
    {
        if (!status.has_value())
        {
            return true;
        }
        if (status->status == "failed" || status->status == "running")
        {
            // A "running" status found at startup can only be from a
            // previous run that crashed (a clean quit is blocked while
            // running -- see QuitHandler) -- treat it as stale, not ongoing.
            return true;
        }
        if (status->status == "complete")
        {
            return (now - status->completed_at) > kUlsStalenessThresholdSeconds;
        }
        return true;
    }

    bool ShouldRetryAuxiliaryImport(const std::optional<ImportRunStatus>& status)
    {
        return status.has_value() && status->status == "failed";
    }

}  // namespace ql
