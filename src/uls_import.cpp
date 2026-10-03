#include "uls_import.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "date_utils.hpp"
#include "db/database.hpp"
#include "file_export.hpp"
#include "geo_utils.hpp"
#include "models.hpp"
#include "text_utils.hpp"
#include "version.hpp"
#include "zip_extract.hpp"

namespace ql
{

    DataSources DefaultDataSources()
    {
        DataSources sources;
        // The FCC's weekly file of every amateur license, copied each week to
        // the repository's "fcc-data" release (.github/workflows/
        // fcc-mirror.yml): Akamai blocks fcc.gov from some cloud networks
        // (403 Access Denied), where a QuickLogger server is likely to run,
        // but not from GitHub's. The FCC itself is the fallback.
        sources.uls_zip_url = "https://github.com/W4KWK-Projects/QuickLogger/releases/download/fcc-data/l_amat.zip";
        sources.uls_zip_fallback_url = "https://data.fcc.gov/download/pub/uls/complete/l_amat.zip";
        // The FCC's GMRS licenses, mirrored the same way (fcc-mirror.yml).
        sources.gmrs_zip_url = "https://github.com/W4KWK-Projects/QuickLogger/releases/download/fcc-data/l_gmrs.zip";
        sources.gmrs_zip_fallback_url = "https://data.fcc.gov/download/pub/uls/complete/l_gmrs.zip";
        // ISED's amateur call sign database, republished daily: one
        // semicolon-delimited file of every Canadian amateur and club call
        // sign (see LoadIsed).
        sources.ised_zip_url = "https://apc-cap.ic.gc.ca/datafiles/amateur_delim.zip";
        // Census Bureau ZCTA gazetteer: approximate lat/lon centroid per US
        // ZIP code, used to estimate distance for the proximity autocomplete
        // (see AppendNearbyUlsSuggestions in app_state.cpp). Centroids are
        // effectively permanent, so this is fetched once and never re-fetched,
        // unlike the ULS license data. The filename is year-prefixed by
        // Census; if this URL 404s in the future, it needs updating to a newer
        // edition (and the matching extracted filename).
        sources.zip_gazetteer_url =
            "https://www2.census.gov/geo/docs/maps-data/data/gazetteer/2023_Gazetteer/"
            "2023_Gaz_zcta_national.zip";
        sources.zip_gazetteer_file_name = "2023_Gaz_zcta_national.txt";
        // GeoNames' Canadian postal codes (CC BY 4.0): one lat/lon per FSA,
        // the first three characters of a postal code. Rough in rural
        // areas, decent in cities.
        sources.ca_postal_url = "https://download.geonames.org/export/zip/CA.zip";
        sources.ca_postal_file_name = "CA.txt";
        // The three Census files the ZIP-to-county data is built from (see
        // FetchAndLoadZipCounties). FCC's ULS data has no county field
        // anywhere in it (confirmed against a real downloaded l_amat.zip:
        // EN.dat's 30 fields cover only street/city/state/zip, and the only
        // other candidate file, CO.dat, turned out to be license status
        // "Comments", not counties). County boundaries are effectively
        // permanent, so -- like the centroids -- these are fetched once and
        // never re-fetched. All three are plain pipe- or comma-delimited text.
        //
        // 2020 ZIP (ZCTA) to county: every county each ZIP overlaps, with the
        // land area of each overlap, and the county names.
        sources.zcta_county_url =
            "https://www2.census.gov/geo/docs/maps-data/data/rel2020/zcta520/"
            "tab20_zcta520_county20_natl.txt";
        // 2010 ZIP to county, which unlike the 2020 edition also counts the
        // *people* in each overlap -- the better guide to which county a ZIP
        // belongs to, since land area can put a ZIP in the county that owns
        // an empty ridge rather than the one its residents live in.
        sources.zcta_county_population_url =
            "https://www2.census.gov/geo/docs/maps-data/data/rel/zcta_county_rel_10.txt";
        // 2020 ZIP to county subdivision: every town/city/township (or, in
        // some states, census division) each ZIP overlaps, with its county.
        sources.zcta_county_subdivision_url =
            "https://www2.census.gov/geo/docs/maps-data/data/rel2020/zcta520/"
            "tab20_zcta520_cousub20_natl.txt";
        return sources;
    }

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

    // A ZIP with at least this share of its people (or, lacking population
    // figures, its land) in one county is simply "in" that county; below
    // it, the ZIP straddles a county line and the town a station gives as
    // its city decides (see ZipPlaceCounty).
    static constexpr double kSingleCountyShare = 0.95;

    static std::string UlsCacheDir(const std::string& db_path)
    {
        std::string::size_type slash = db_path.find_last_of('/');
        std::string dir = slash == std::string::npos ? std::string(".") : db_path.substr(0, slash);
        return dir + "/uls_cache";
    }

    // Splits `line` on `separator` into `fields`, as views into `line` --
    // no per-field allocation, and `fields` is reused from call to call, so
    // parsing millions of lines costs no allocations after the first. The
    // views are only valid while `line` is unchanged.
    static void SplitFields(std::string_view line, char separator, std::vector<std::string_view>* fields)
    {
        fields->clear();
        std::string_view::size_type start = 0;
        while (true)
        {
            std::string_view::size_type found = line.find(separator, start);
            if (found == std::string_view::npos)
            {
                fields->push_back(line.substr(start));
                return;
            }
            fields->push_back(line.substr(start, found - start));
            start = found + 1;
        }
    }

    static std::string_view FieldOrEmpty(const std::vector<std::string_view>& fields, std::size_t index)
    {
        return index < fields.size() ? fields[index] : std::string_view();
    }

    // The FCC files' record ids are plain decimal numbers; 0 if a field isn't
    // one. Parsed to an integer so the id lookups below hash a number rather
    // than a string.
    static std::uint32_t ParseRecordId(std::string_view text)
    {
        std::uint32_t id = 0;
        std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), id);
        return result.ec == std::errc() ? id : 0;
    }

    // std::stod wants a std::string, and std::from_chars for floating point
    // isn't available in every standard library this builds with; strtod on
    // a small stack copy handles the short numeric fields these files have.
    static double ParseDoubleOr(std::string_view text, double fallback)
    {
        // Some Census files pad fields with spaces (the gazetteer's last
        // column carries dozens), so trim before measuring.
        std::string_view::size_type first = text.find_first_not_of(" \t\r");
        if (first == std::string_view::npos)
        {
            return fallback;
        }
        text = text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
        char buffer[64];
        if (text.size() >= sizeof(buffer))
        {
            return fallback;
        }
        std::memcpy(buffer, text.data(), text.size());
        buffer[text.size()] = '\0';
        char* end = nullptr;
        double value = std::strtod(buffer, &end);
        return end == buffer ? fallback : value;
    }

    // FCC's EN.dat zip field is sometimes a 9-digit ZIP+4 with no separator
    // (e.g. "374152623" for "37415-2623") rather than a plain 5-digit ZIP --
    // confirmed against a real downloaded l_amat.zip. Every consumer of
    // Station::zip in this app (the zip_centroids/zip_counties lookups, the
    // saved-station form's ULS proximity tier) keys off a plain 5-digit
    // ZIP, so normalize once here at the source rather than in every
    // consumer.
    static std::string_view NormalizeZip5(std::string_view zip)
    {
        return zip.size() > 5 ? zip.substr(0, 5) : zip;
    }

    static const char* LicenseClassFromCode(std::string_view code)
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

    static std::int64_t Now()
    {
        return static_cast<std::int64_t>(std::time(nullptr));
    }

    static std::int64_t FileSize(const std::string& path)
    {
        std::error_code error;
        std::uintmax_t size = std::filesystem::file_size(path, error);
        return error ? 0 : static_cast<std::int64_t>(size);
    }

    // Writes the refresh job's progress (see kDataRefreshJob) and answers
    // "should we stop?". The work is split into steps, each owning a slice
    // [base, base + span) of the overall 0-100%; a step reports how far
    // through itself it is, and this maps that onto the overall figure.
    // Writes are throttled to one a second -- a progress row rewritten on
    // every downloaded chunk would be thousands of pointless writes.
    class ProgressReporter
    {
    public:
        ProgressReporter(Database* db, bool (*should_stop)()) : db_(db), should_stop_(should_stop) {}

        void BeginStep(const std::string& phase, int base, int span)
        {
            phase_ = phase;
            base_ = base;
            span_ = span;
            Write(base_);
        }

        // `fraction` is how far through the current step, 0.0-1.0.
        void Report(double fraction, std::int64_t records)
        {
            records_ = records;
            if (fraction < 0.0)
            {
                fraction = 0.0;
            }
            if (fraction > 1.0)
            {
                fraction = 1.0;
            }
            if (Now() - last_write_ >= 1)
            {
                Write(base_ + static_cast<int>(fraction * span_));
            }
        }

        [[nodiscard]] bool StopRequested() const
        {
            return should_stop_ != nullptr && should_stop_();
        }

    private:
        void Write(int percent)
        {
            last_write_ = Now();
            db_->UpdateImportProgress(kDataRefreshJob, phase_, percent, records_, last_write_);
        }

        Database* db_;
        bool (*should_stop_)();
        std::string phase_;
        int base_ = 0;
        int span_ = 0;
        std::int64_t records_ = 0;
        std::int64_t last_write_ = 0;
    };

    static size_t CurlWriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        std::FILE* file = static_cast<std::FILE*>(userdata);
        return std::fwrite(ptr, size, nmemb, file);
    }

    // Reports download progress, and aborts the transfer (non-zero return)
    // once a stop has been requested.
    static int CurlProgressCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/,
                                    curl_off_t /*ulnow*/)
    {
        ProgressReporter* reporter = static_cast<ProgressReporter*>(userdata);
        if (dltotal > 0)
        {
            reporter->Report(static_cast<double>(dlnow) / static_cast<double>(dltotal), 0);
        }
        return reporter->StopRequested() ? 1 : 0;
    }

    static bool DownloadFile(const std::string& url, const std::string& dest_path, long timeout_seconds,
                             ProgressReporter* reporter, std::string* error)
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
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, reporter);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
        std::string user_agent = std::string("QuickLogger/") + QuickLoggerVersion();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());

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

    // ---- FCC ULS license data ----------------------------------------------

    // One of the FCC's license files: the amateur one (l_amat.zip) or the
    // GMRS one (l_gmrs.zip). Both hold HD.dat and EN.dat in the same layout;
    // only the amateur one has AM.dat, with the license class.
    struct FccLicenseFile
    {
        std::string url;
        std::string fallback_url;
        std::string zip_name;     // As downloaded into the cache.
        std::string extract_dir;  // Where its .dat files are unpacked.
        LicenseTable table;
        bool has_classes;  // AM.dat
        const char* what;  // "FCC license data", for progress.
    };

    static bool ExtractUlsZip(const FccLicenseFile& file, const std::string& zip_path, std::string* error)
    {
        std::vector<std::string> entries = {"HD.dat", "EN.dat"};
        if (file.has_classes)
        {
            entries.emplace_back("AM.dat");
        }
        EnsureDirectory(file.extract_dir);
        if (!ExtractZipEntries(zip_path, file.extract_dir, entries, error))
        {
            *error = "Failed to extract the FCC ULS archive: " + *error;
            return false;
        }
        return true;
    }

    // Reads `path` line by line, calling `handler.HandleLine(fields)` with
    // each line split on '|', and reporting progress through the file (by
    // bytes read) as `fraction_base` to `fraction_base + fraction_span` of
    // the current step. Returns false if the file can't be read or a stop
    // was requested.
    template <typename Handler>
    static bool ForEachUlsRecord(const std::string& path, ProgressReporter* reporter, double fraction_base,
                                 double fraction_span, Handler* handler)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.good())
        {
            return false;
        }
        double total_bytes = static_cast<double>(FileSize(path));
        std::string line;
        std::vector<std::string_view> fields;
        std::int64_t line_number = 0;
        while (std::getline(file, line))
        {
            SplitFields(line, '|', &fields);
            handler->HandleLine(fields);
            if (++line_number % 20000 == 0)
            {
                if (reporter->StopRequested())
                {
                    return false;
                }
                std::streamoff position = file.tellg();
                if (total_bytes > 0.0 && position > 0)
                {
                    reporter->Report(fraction_base + fraction_span * static_cast<double>(position) / total_bytes, 0);
                }
            }
        }
        return true;
    }

    // Where a piece of text is in UlsLicenses::text.
    struct TextRef
    {
        std::uint32_t offset = 0;
        std::uint16_t length = 0;
    };

    // One active license, holding only what's imported -- about 800,000 of
    // these are in memory at once, so each is kept small: its text lives in
    // one shared buffer (UlsLicenses::text) rather than in strings of its
    // own, the state and ZIP in place, and the class as a pointer to one of
    // LicenseClassFromCode's names. About 48 bytes, where a Station is more
    // than 250 before its text.
    struct UlsRecord
    {
        TextRef callsign;
        TextRef name;
        TextRef street_address;
        TextRef city;
        char state[3] = {};
        char zip[6] = {};
        const char* license_class = "";
    };

    // Copies up to size - 1 characters of `text` into `field`.
    template <std::size_t N>
    static void CopyField(std::string_view text, char (&field)[N])
    {
        std::size_t length = text.size() < N - 1 ? text.size() : N - 1;
        std::memcpy(field, text.data(), length);
        field[length] = '\0';
    }

    // Everything needed from the three FCC files, for the active licenses
    // only. HD.dat is read first to find those (about half of its 1.7M rows);
    // EN.dat and AM.dat then fill in just those records, found by numeric id
    // -- far less memory and hashing than keeping all 1.7M EN.dat entries.
    // The ids are a sorted list searched by halves rather than a hash map:
    // 8 bytes an id instead of about 50.
    struct UlsLicenses
    {
        std::vector<UlsRecord> records;
        // Every record's text, one after another.
        std::string text;
        // (id, index into records), sorted by id once HD.dat has been read.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ids;

        TextRef Store(std::string_view value)
        {
            TextRef ref;
            ref.offset = static_cast<std::uint32_t>(text.size());
            ref.length = static_cast<std::uint16_t>(value.size() < 0xFFFF ? value.size() : 0xFFFF);
            text.append(value.data(), ref.length);
            return ref;
        }

        std::string_view Text(TextRef ref) const
        {
            return std::string_view(text.data() + ref.offset, ref.length);
        }

        // The record with FCC id `id`, or nullptr if it isn't an active one.
        UlsRecord* Find(std::uint32_t id)
        {
            std::vector<std::pair<std::uint32_t, std::uint32_t>>::iterator it =
                std::lower_bound(ids.begin(), ids.end(), std::pair<std::uint32_t, std::uint32_t>(id, 0));
            if (it == ids.end() || it->first != id)
            {
                return nullptr;
            }
            return &records[it->second];
        }
    };

    class HdLineHandler
    {
    public:
        explicit HdLineHandler(UlsLicenses* licenses) : licenses_(licenses) {}

        void HandleLine(const std::vector<std::string_view>& fields)
        {
            if (fields.size() <= kHdLicenseStatus || fields[0] != "HD" || fields[kHdLicenseStatus] != "A" ||
                fields[kHdCallSign].empty())
            {
                return;
            }
            std::uint32_t id = ParseRecordId(fields[kHdUniqueSystemId]);
            if (id == 0)
            {
                return;
            }
            licenses_->ids.emplace_back(id, static_cast<std::uint32_t>(licenses_->records.size()));
            licenses_->records.emplace_back();
            licenses_->records.back().callsign = licenses_->Store(fields[kHdCallSign]);
        }

    private:
        UlsLicenses* licenses_;
    };

    class EnLineHandler
    {
    public:
        explicit EnLineHandler(UlsLicenses* licenses) : licenses_(licenses) {}

        void HandleLine(const std::vector<std::string_view>& fields)
        {
            if (fields.empty() || fields[0] != "EN")
            {
                return;
            }
            UlsRecord* record = licenses_->Find(ParseRecordId(FieldOrEmpty(fields, kEnUniqueSystemId)));
            if (record == nullptr)
            {
                return;
            }
            record->name = licenses_->Store(FieldOrEmpty(fields, kEnEntityName));
            record->street_address = licenses_->Store(FieldOrEmpty(fields, kEnStreetAddress));
            record->city = licenses_->Store(FieldOrEmpty(fields, kEnCity));
            CopyField(FieldOrEmpty(fields, kEnState), record->state);
            CopyField(NormalizeZip5(FieldOrEmpty(fields, kEnZip)), record->zip);
        }

    private:
        UlsLicenses* licenses_;
    };

    class AmLineHandler
    {
    public:
        explicit AmLineHandler(UlsLicenses* licenses) : licenses_(licenses) {}

        void HandleLine(const std::vector<std::string_view>& fields)
        {
            if (fields.empty() || fields[0] != "AM")
            {
                return;
            }
            UlsRecord* record = licenses_->Find(ParseRecordId(FieldOrEmpty(fields, kAmUniqueSystemId)));
            if (record == nullptr)
            {
                return;
            }
            record->license_class = LicenseClassFromCode(FieldOrEmpty(fields, kAmOperatorClass));
        }

    private:
        UlsLicenses* licenses_;
    };

    static bool ParseAndImport(const FccLicenseFile& file, Database* db, ProgressReporter* reporter,
                               std::int64_t* out_records, std::string* error)
    {
        const std::string& cache_dir = file.extract_dir;
        UlsLicenses licenses;
        licenses.records.reserve(900000);
        licenses.ids.reserve(900000);
        licenses.text.reserve(std::size_t{48} * 1024 * 1024);

        HdLineHandler hd_handler(&licenses);
        EnLineHandler en_handler(&licenses);
        AmLineHandler am_handler(&licenses);
        if (!ForEachUlsRecord(cache_dir + "/HD.dat", reporter, 0.0, 0.15, &hd_handler))
        {
            *error = reporter->StopRequested() ? "Interrupted." : "Failed to read the FCC files.";
            return false;
        }
        std::sort(licenses.ids.begin(), licenses.ids.end());
        if (!ForEachUlsRecord(cache_dir + "/EN.dat", reporter, 0.15, 0.2, &en_handler) ||
            (file.has_classes && !ForEachUlsRecord(cache_dir + "/AM.dat", reporter, 0.35, 0.05, &am_handler)))
        {
            *error = reporter->StopRequested() ? "Interrupted." : "Failed to read the FCC files.";
            return false;
        }
        if (licenses.records.empty())
        {
            *error = "HD.dat had no active licenses.";
            return false;
        }
        // Not needed from here on.
        std::vector<std::pair<std::uint32_t, std::uint32_t>>().swap(licenses.ids);

        // Written a batch at a time, each batch briefly as Stations.
        std::int64_t now = Now();
        constexpr std::size_t kBatchSize = 5000;
        std::size_t total = licenses.records.size();
        std::vector<Station> batch;
        batch.reserve(kBatchSize);
        for (std::size_t start = 0; start < total; start += kBatchSize)
        {
            std::size_t end = start + kBatchSize < total ? start + kBatchSize : total;
            batch.clear();
            for (std::size_t i = start; i < end; ++i)
            {
                const UlsRecord& record = licenses.records[i];
                Station station;
                station.callsign = std::string(licenses.Text(record.callsign));
                station.name = std::string(licenses.Text(record.name));
                station.street_address = std::string(licenses.Text(record.street_address));
                station.city = std::string(licenses.Text(record.city));
                station.state = record.state;
                station.zip = record.zip;
                station.license_class = record.license_class;
                batch.push_back(std::move(station));
            }
            db->BulkUpsertUlsStations(batch, 0, batch.size(), now, file.table);
            reporter->Report(0.4 + 0.6 * static_cast<double>(end) / static_cast<double>(total),
                             static_cast<std::int64_t>(end));
            if (reporter->StopRequested())
            {
                *error = "Interrupted.";
                return false;
            }
        }

        // Licenses no longer in the file have expired or been cancelled.
        std::vector<std::string_view> callsigns;
        callsigns.reserve(total);
        for (const UlsRecord& record : licenses.records)
        {
            callsigns.push_back(licenses.Text(record.callsign));
        }
        db->DeleteUlsStationsNotIn(callsigns, file.table);

        *out_records = static_cast<std::int64_t>(total);
        return true;
    }

    static bool DownloadAndExtractUls(const FccLicenseFile& file, const std::string& url, const std::string& zip_path,
                                      ProgressReporter* reporter, int base, int download_span, int extract_span,
                                      std::string* error)
    {
        reporter->BeginStep(std::string("Downloading ") + file.what, base, download_span);
        if (!DownloadFile(url, zip_path, 1800L, reporter, error))
        {
            return false;
        }
        reporter->BeginStep(std::string("Unpacking ") + file.what, base + download_span, extract_span);
        return ExtractUlsZip(file, zip_path, error);
    }

    static bool LoadFccLicenses(const FccLicenseFile& file, const std::string& cache_dir, Database* db,
                                ProgressReporter* reporter, int base, int span, std::int64_t* out_records,
                                std::string* error)
    {
        // Download ~60% of this step's time, extraction ~5%, parsing the
        // rest -- roughly how long each takes on a typical connection.
        int download_span = span * 60 / 100;
        int extract_span = span * 5 / 100;
        std::string zip_path = cache_dir + "/" + file.zip_name;

        // The GitHub copy, then the FCC's own if the copy can't be had (or
        // turns out damaged: unpacking checks every file's CRC).
        std::string first_error;
        bool fetched =
            DownloadAndExtractUls(file, file.url, zip_path, reporter, base, download_span, extract_span, &first_error);
        if (!fetched)
        {
            if (file.fallback_url.empty() || reporter->StopRequested())
            {
                *error = first_error;
                return false;
            }
            std::string fallback_error;
            fetched = DownloadAndExtractUls(file, file.fallback_url, zip_path, reporter, base, download_span,
                                            extract_span, &fallback_error);
            if (!fetched)
            {
                *error = "GitHub copy: " + first_error + "; FCC: " + fallback_error;
                return false;
            }
        }

        reporter->BeginStep(std::string("Importing ") + file.what, base + download_span + extract_span,
                            span - download_span - extract_span);
        return ParseAndImport(file, db, reporter, out_records, error);
    }

    static bool LoadUls(const DataSources& sources, const std::string& cache_dir, Database* db,
                        ProgressReporter* reporter, int base, int span, std::int64_t* out_records, std::string* error)
    {
        FccLicenseFile file{sources.uls_zip_url, sources.uls_zip_fallback_url, "l_amat.zip",
                            cache_dir,           LicenseTable::kAmateur,       true,
                            "FCC license data"};
        return LoadFccLicenses(file, cache_dir, db, reporter, base, span, out_records, error);
    }

    // The FCC's GMRS licenses, unpacked into a folder of their own (their
    // HD.dat and EN.dat would replace the amateur ones').
    static bool LoadGmrs(const DataSources& sources, const std::string& cache_dir, Database* db,
                         ProgressReporter* reporter, int base, int span, std::int64_t* out_records, std::string* error)
    {
        FccLicenseFile file{sources.gmrs_zip_url, sources.gmrs_zip_fallback_url, "l_gmrs.zip",
                            cache_dir + "/gmrs",  LicenseTable::kGmrs,           false,
                            "FCC GMRS data"};
        return LoadFccLicenses(file, cache_dir, db, reporter, base, span, out_records, error);
    }

    // ---- ISED (Canadian) call sign data -------------------------------------

    // amateur_delim.txt: a header row, then one call sign per line, fields
    // separated by ';' (0-indexed; confirmed against a real download):
    static constexpr std::size_t kIsedCallsign = 0;
    static constexpr std::size_t kIsedFirstName = 1;
    static constexpr std::size_t kIsedSurname = 2;
    static constexpr std::size_t kIsedAddress = 3;
    static constexpr std::size_t kIsedCity = 4;
    static constexpr std::size_t kIsedProvince = 5;
    static constexpr std::size_t kIsedPostalCode = 6;
    // Qualifications, each its letter or blank: A Basic, B Morse (5 wpm),
    // C Morse (12 wpm), D Advanced, E Basic with Honours.
    static constexpr std::size_t kIsedQualBasic = 7;
    static constexpr std::size_t kIsedQualAdvanced = 10;
    static constexpr std::size_t kIsedQualHonours = 11;
    // A club call sign carries the club's own name and address as well as
    // its trustee's.
    static constexpr std::size_t kIsedClubName = 12;
    static constexpr std::size_t kIsedClubName2 = 13;
    static constexpr std::size_t kIsedClubAddress = 14;
    static constexpr std::size_t kIsedClubCity = 15;
    static constexpr std::size_t kIsedClubProvince = 16;
    static constexpr std::size_t kIsedClubPostalCode = 17;

    // "B4E2X8" as Canadians write it, "B4E 2X8".
    static std::string FormatPostalCode(std::string_view code)
    {
        std::string text(code);
        if (text.size() == 6)
        {
            text.insert(3, " ");
        }
        return text;
    }

    // The highest qualification, as a license class.
    static std::string IsedLicenseClass(const std::vector<std::string_view>& fields)
    {
        if (!FieldOrEmpty(fields, kIsedQualAdvanced).empty())
        {
            return "Advanced";
        }
        if (!FieldOrEmpty(fields, kIsedQualHonours).empty())
        {
            return "Basic with Honours";
        }
        if (!FieldOrEmpty(fields, kIsedQualBasic).empty())
        {
            return "Basic";
        }
        return "";
    }

    // One line of amateur_delim.txt as a Station, or false for the header
    // or a line without a call sign.
    static bool ParseIsedLine(const std::vector<std::string_view>& fields, Station* station)
    {
        std::string_view callsign = FieldOrEmpty(fields, kIsedCallsign);
        if (callsign.empty() || callsign == "callsign")
        {
            return false;
        }
        *station = Station();
        station->callsign = ToUpperAscii(std::string(callsign));
        station->data_source = StationDataSource::kIsed;
        station->license_class = IsedLicenseClass(fields);
        std::string club = std::string(FieldOrEmpty(fields, kIsedClubName));
        if (!FieldOrEmpty(fields, kIsedClubName2).empty())
        {
            club += (club.empty() ? "" : " ") + std::string(FieldOrEmpty(fields, kIsedClubName2));
        }
        if (!club.empty())
        {
            station->name = club;
        }
        else
        {
            // "Surname, First", as the FCC data has names.
            std::string surname(FieldOrEmpty(fields, kIsedSurname));
            std::string first(FieldOrEmpty(fields, kIsedFirstName));
            station->name = surname.empty() || first.empty() ? surname + first : surname + ", " + first;
        }
        bool club_address = !club.empty() && !FieldOrEmpty(fields, kIsedClubCity).empty();
        station->street_address = std::string(FieldOrEmpty(fields, club_address ? kIsedClubAddress : kIsedAddress));
        station->city = std::string(FieldOrEmpty(fields, club_address ? kIsedClubCity : kIsedCity));
        station->state = std::string(FieldOrEmpty(fields, club_address ? kIsedClubProvince : kIsedProvince));
        station->zip = FormatPostalCode(FieldOrEmpty(fields, club_address ? kIsedClubPostalCode : kIsedPostalCode));
        return true;
    }

    static bool LoadIsed(const DataSources& sources, const std::string& cache_dir, Database* db,
                         ProgressReporter* reporter, int base, int span, std::int64_t* out_records, std::string* error)
    {
        std::string zip_path = cache_dir + "/amateur_delim.zip";
        reporter->BeginStep("Downloading Canadian call sign data", base, span / 2);
        if (!DownloadFile(sources.ised_zip_url, zip_path, 600L, reporter, error))
        {
            return false;
        }
        reporter->BeginStep("Importing Canadian call sign data", base + span / 2, span - span / 2);
        if (!ExtractZipEntries(zip_path, cache_dir, {"amateur_delim.txt"}, error))
        {
            *error = "Failed to extract the ISED archive: " + *error;
            return false;
        }
        std::ifstream file(cache_dir + "/amateur_delim.txt", std::ios::binary);
        if (!file.good())
        {
            *error = "The ISED archive had no amateur_delim.txt.";
            return false;
        }
        std::vector<Station> stations;
        std::string line;
        std::vector<std::string_view> fields;
        Station station;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            SplitFields(line, ';', &fields);
            if (ParseIsedLine(fields, &station))
            {
                stations.push_back(station);
            }
        }
        if (stations.empty())
        {
            *error = "The ISED file had no call signs.";
            return false;
        }
        if (reporter->StopRequested())
        {
            *error = "Interrupted.";
            return false;
        }
        db->ReplaceIsedStations(stations, Now());
        *out_records = static_cast<std::int64_t>(stations.size());
        return true;
    }

    // ---- ZIP centroids -----------------------------------------------------

    static bool FetchAndLoadZipCentroids(const DataSources& sources, const std::string& cache_dir, Database* db,
                                         ProgressReporter* reporter, std::string* error)
    {
        std::string zip_path = cache_dir + "/zip_gazetteer.zip";
        if (!DownloadFile(sources.zip_gazetteer_url, zip_path, 300L, reporter, error))
        {
            return false;
        }

        if (!ExtractZipEntries(zip_path, cache_dir, {sources.zip_gazetteer_file_name}, error))
        {
            *error = "Failed to extract the ZIP gazetteer archive: " + *error;
            return false;
        }

        std::string txt_path = cache_dir + "/" + sources.zip_gazetteer_file_name;
        std::ifstream file(txt_path);
        if (!file.good())
        {
            *error = "Extraction did not produce " + std::string(sources.zip_gazetteer_file_name) + ".";
            return false;
        }

        std::string line;
        std::getline(file, line);  // Header row.
        constexpr std::size_t kBatchSize = 1000;
        std::vector<ZipCentroid> batch;
        batch.reserve(kBatchSize);
        std::vector<std::string_view> fields;
        while (std::getline(file, line))
        {
            SplitFields(line, '\t', &fields);
            if (fields.size() < 7)
            {
                continue;
            }
            constexpr double kNotANumber = 1000.0;  // Outside any real lat/lon.
            ZipCentroid centroid;
            centroid.zip = std::string(fields[0]);
            centroid.lat = ParseDoubleOr(fields[5], kNotANumber);
            centroid.lon = ParseDoubleOr(fields[6], kNotANumber);
            if (centroid.lat == kNotANumber || centroid.lon == kNotANumber)
            {
                continue;
            }
            batch.push_back(std::move(centroid));
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

    static bool FetchAndLoadCaPostalCentroids(const DataSources& sources, const std::string& cache_dir, Database* db,
                                              ProgressReporter* reporter, std::string* error)
    {
        std::string zip_path = cache_dir + "/ca_postal.zip";
        if (!DownloadFile(sources.ca_postal_url, zip_path, 120L, reporter, error))
        {
            return false;
        }
        if (!ExtractZipEntries(zip_path, cache_dir, {sources.ca_postal_file_name}, error))
        {
            *error = "Failed to extract the Canadian postal code archive: " + *error;
            return false;
        }
        std::string txt_path = cache_dir + "/" + sources.ca_postal_file_name;
        std::ifstream file(txt_path);
        if (!file.good())
        {
            *error = "Extraction did not produce " + sources.ca_postal_file_name + ".";
            return false;
        }

        // Tab-separated, no header: country, postal code, place, province
        // name, province code, three blank admin columns, latitude,
        // longitude, accuracy.
        constexpr std::size_t kCodeField = 1;
        constexpr std::size_t kLatField = 9;
        constexpr std::size_t kLonField = 10;
        constexpr double kNotANumber = 1000.0;  // Outside any real lat/lon.
        std::vector<ZipCentroid> batch;
        std::string line;
        std::vector<std::string_view> fields;
        while (std::getline(file, line))
        {
            SplitFields(line, '\t', &fields);
            if (fields.size() <= kLonField)
            {
                continue;
            }
            ZipCentroid centroid;
            centroid.zip = ZipCentroidKey(std::string(fields[kCodeField]));
            centroid.lat = ParseDoubleOr(fields[kLatField], kNotANumber);
            centroid.lon = ParseDoubleOr(fields[kLonField], kNotANumber);
            if (centroid.zip.size() != 3 || centroid.lat == kNotANumber || centroid.lon == kNotANumber)
            {
                continue;
            }
            batch.push_back(std::move(centroid));
        }
        if (batch.empty())
        {
            *error = "The Canadian postal code file had no usable rows.";
            return false;
        }
        db->BulkUpsertZipCentroids(batch);
        return true;
    }

    // ---- ZIP to county -----------------------------------------------------

    // The relationship files spell out the county's full legal name (e.g.
    // "Hamilton County"), but the app just wants the bare name to display
    // (e.g. "Hamilton") -- the column header/context already says "County".
    static std::string StripCountySuffix(const std::string& county)
    {
        const std::string suffix = " County";
        if (county.size() > suffix.size() && county.compare(county.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            return county.substr(0, county.size() - suffix.size());
        }
        return county;
    }

    // One county a ZIP overlaps, with how much of the ZIP is in it.
    struct ZipCountyShare
    {
        std::string county_geoid;
        double land_area = 0.0;
        double share = 0.0;  // Filled in once all the rows for the ZIP are known.
    };

    // A town inside a straddling ZIP, and the county that won it so far.
    struct PlaceCandidate
    {
        std::string county;
        double land_area = 0.0;
    };

    // Words that on their own are just part of a Census label, never a town
    // name a station would give as its city ("District 6" -> "DISTRICT").
    static bool IsGenericPlaceWord(const std::string& name)
    {
        return name.empty() || name == "DISTRICT" || name == "PRECINCT" || name == "WARD" || name == "BEAT" ||
               name == "ELECTION DISTRICT";
    }

    // The forms a Census county-subdivision name might take as a mailing
    // city: the name as-is, and with its type suffix removed -- "Newton
    // city" -> "NEWTON", "Hanover township" -> "HANOVER", "Cayey
    // barrio-pueblo" -> "CAYEY", "Cleveland CCD" -> "CLEVELAND", and the few
    // two-word suffixes, "Canton charter township" -> "CANTON". (Only those
    // known two-word suffixes are stripped as a pair -- dropping any last two
    // words would turn "North Hempstead town" into "NORTH".)
    static std::vector<std::string> PlaceNameVariants(const std::string& census_name)
    {
        std::string full = NormalizePlaceName(census_name);
        std::vector<std::string> variants;
        if (!IsGenericPlaceWord(full))
        {
            variants.push_back(full);
        }
        for (const char* suffix : {" CHARTER TOWNSHIP", " CENSUS SUBAREA"})
        {
            std::string two_word_suffix(suffix);
            if (full.size() > two_word_suffix.size() &&
                full.compare(full.size() - two_word_suffix.size(), two_word_suffix.size(), two_word_suffix) == 0)
            {
                variants.push_back(full.substr(0, full.size() - two_word_suffix.size()));
                return variants;
            }
        }
        std::string::size_type last_space = full.find_last_of(' ');
        if (last_space != std::string::npos)
        {
            std::string without_suffix = full.substr(0, last_space);
            if (!IsGenericPlaceWord(without_suffix))
            {
                variants.push_back(without_suffix);
            }
        }
        return variants;
    }

    // Builds the ZIP-to-county data from the three Census files:
    //
    //  1. For every ZIP, the counties it overlaps (2020 file), each weighted
    //     by its share of the ZIP's people (2010 file) -- or its share of the
    //     ZIP's land where there are no population figures (a ZIP created
    //     after 2010, or one whose county codes have changed since). The
    //     county with the biggest share is the ZIP's county (ZipCounty).
    //     Weighting by people rather than land is what gets e.g. 37419
    //     (Chattanooga) right: 93% of its residents are in Hamilton County,
    //     though 53% of its land is in Marion.
    //
    //  2. For every ZIP that straddles a county line (no county holds
    //     kSingleCountyShare of it), the towns inside it and each town's
    //     county (ZipPlaceCounty), from the county-subdivision file. A
    //     station whose city names one of those towns gets that town's
    //     county, even when most of the ZIP's people are elsewhere -- e.g.
    //     02467 (Chestnut Hill) is split between Boston (Suffolk), Newton
    //     (Middlesex) and Brookline (Norfolk). Where a town's name appears in
    //     more than one county within the ZIP (a town split by the line
    //     itself, like Bethlehem, PA), the county with more of that town's
    //     land in the ZIP wins.
    static bool FetchAndLoadZipCounties(const DataSources& sources, const std::string& cache_dir, Database* db,
                                        ProgressReporter* reporter, int base, int span, std::string* error)
    {
        // The three downloads (about 6, 7 and 16 MB) get a third of the
        // step's progress each.
        std::string county_path = cache_dir + "/zcta_county.txt";
        std::string population_path = cache_dir + "/zcta_county_population.txt";
        std::string subdivision_path = cache_dir + "/zcta_county_subdivision.txt";
        reporter->BeginStep("Downloading county data", base, span / 3);
        if (!DownloadFile(sources.zcta_county_url, county_path, 300L, reporter, error))
        {
            return false;
        }
        reporter->BeginStep("Downloading county data", base + span / 3, span / 3);
        if (!DownloadFile(sources.zcta_county_population_url, population_path, 300L, reporter, error))
        {
            return false;
        }
        reporter->BeginStep("Downloading county data", base + 2 * (span / 3), span - 2 * (span / 3));
        if (!DownloadFile(sources.zcta_county_subdivision_url, subdivision_path, 300L, reporter, error))
        {
            return false;
        }

        // 2020 ZIP -> county overlaps. Fields: 1 GEOID_ZCTA5_20,
        // 9 GEOID_COUNTY_20, 10 NAMELSAD_COUNTY_20, 16 AREALAND_PART.
        std::unordered_map<std::string, std::vector<ZipCountyShare>> shares_by_zip;
        std::unordered_map<std::string, std::string> county_names;
        {
            std::ifstream file(county_path);
            if (!file.good())
            {
                *error = "Failed to open the downloaded ZIP-county file.";
                return false;
            }
            std::string line;
            std::getline(file, line);  // Header row.
            std::vector<std::string_view> fields;
            while (std::getline(file, line))
            {
                SplitFields(line, '|', &fields);
                if (fields.size() < 17 || fields[1].empty() || fields[9].empty())
                {
                    continue;
                }
                ZipCountyShare share;
                share.county_geoid = std::string(fields[9]);
                share.land_area = ParseDoubleOr(fields[16], 0.0);
                std::string county_name = StripCountySuffix(std::string(fields[10]));
                county_names[share.county_geoid] = std::move(county_name);
                shares_by_zip[std::string(fields[1])].push_back(std::move(share));
            }
        }
        if (shares_by_zip.empty())
        {
            *error = "The ZIP-county file was empty.";
            return false;
        }

        // 2010 population per ZIP/county overlap. Comma-separated; fields:
        // 0 ZCTA5, 3 GEOID (county), 4 POPPT, 8 ZPOP.
        std::unordered_map<std::string, std::unordered_map<std::string, double>> population_share;
        {
            std::ifstream file(population_path);
            std::string line;
            std::getline(file, line);  // Header row.
            std::vector<std::string_view> fields;
            while (std::getline(file, line))
            {
                SplitFields(line, ',', &fields);
                if (fields.size() < 9)
                {
                    continue;
                }
                double zip_population = ParseDoubleOr(fields[8], 0.0);
                if (zip_population <= 0.0)
                {
                    continue;
                }
                population_share[std::string(fields[0])][std::string(fields[3])] =
                    ParseDoubleOr(fields[4], 0.0) / zip_population;
            }
        }

        std::vector<ZipCounty> zip_counties;
        zip_counties.reserve(shares_by_zip.size());
        std::unordered_set<std::string> straddling_zips;
        for (std::pair<const std::string, std::vector<ZipCountyShare>>& entry : shares_by_zip)
        {
            std::vector<ZipCountyShare>& shares = entry.second;

            // Population shares, if the 2010 figures cover this ZIP's 2020
            // counties (most of its people accounted for); land otherwise.
            std::unordered_map<std::string, std::unordered_map<std::string, double>>::const_iterator population_it =
                population_share.find(entry.first);
            double population_covered = 0.0;
            if (population_it != population_share.end())
            {
                for (const ZipCountyShare& share : shares)
                {
                    std::unordered_map<std::string, double>::const_iterator county_it =
                        population_it->second.find(share.county_geoid);
                    if (county_it != population_it->second.end())
                    {
                        population_covered += county_it->second;
                    }
                }
            }
            double total_land = 0.0;
            for (const ZipCountyShare& share : shares)
            {
                total_land += share.land_area;
            }
            for (ZipCountyShare& share : shares)
            {
                if (population_covered >= 0.9)
                {
                    std::unordered_map<std::string, double>::const_iterator county_it =
                        population_it->second.find(share.county_geoid);
                    share.share = county_it != population_it->second.end() ? county_it->second : 0.0;
                }
                else
                {
                    share.share =
                        total_land > 0.0 ? share.land_area / total_land : 1.0 / static_cast<double>(shares.size());
                }
            }

            const ZipCountyShare* best = &shares.front();
            for (const ZipCountyShare& share : shares)
            {
                if (share.share > best->share)
                {
                    best = &share;
                }
            }
            ZipCounty zip_county;
            zip_county.zip = entry.first;
            zip_county.county = county_names[best->county_geoid];
            zip_counties.push_back(std::move(zip_county));
            if (best->share < kSingleCountyShare)
            {
                straddling_zips.insert(entry.first);
            }
        }

        // Towns within the straddling ZIPs. Fields: 1 GEOID_ZCTA5_20,
        // 9 GEOID_COUSUB_20 (its first five digits are the county),
        // 10 NAMELSAD_COUSUB_20, 16 AREALAND_PART.
        std::unordered_map<std::string, PlaceCandidate> best_by_zip_place;
        {
            std::ifstream file(subdivision_path);
            if (!file.good())
            {
                *error = "Failed to open the downloaded ZIP-town file.";
                return false;
            }
            std::string line;
            std::getline(file, line);  // Header row.
            std::vector<std::string_view> fields;
            while (std::getline(file, line))
            {
                SplitFields(line, '|', &fields);
                if (fields.size() < 17 || fields[9].size() < 5 || straddling_zips.count(std::string(fields[1])) == 0)
                {
                    continue;
                }
                std::unordered_map<std::string, std::string>::const_iterator county_it =
                    county_names.find(std::string(fields[9].substr(0, 5)));
                if (county_it == county_names.end())
                {
                    continue;
                }
                double land_area = ParseDoubleOr(fields[16], 0.0);
                std::string zip_prefix = std::string(fields[1]) + "|";
                for (const std::string& place : PlaceNameVariants(std::string(fields[10])))
                {
                    PlaceCandidate& candidate = best_by_zip_place[zip_prefix + place];
                    if (candidate.county.empty() || land_area > candidate.land_area)
                    {
                        candidate.county = county_it->second;
                        candidate.land_area = land_area;
                    }
                }
            }
        }

        std::vector<ZipPlaceCounty> zip_place_counties;
        zip_place_counties.reserve(best_by_zip_place.size());
        for (const std::pair<const std::string, PlaceCandidate>& entry : best_by_zip_place)
        {
            std::string::size_type bar = entry.first.find('|');
            ZipPlaceCounty zip_place;
            zip_place.zip = entry.first.substr(0, bar);
            zip_place.place = entry.first.substr(bar + 1);
            zip_place.county = entry.second.county;
            zip_place_counties.push_back(std::move(zip_place));
        }

        db->ReplaceZipCountyData(zip_counties, zip_place_counties);
        return true;
    }

    // ---- Planning and running a refresh ------------------------------------

    static bool IsRetryDue(const ImportRunStatus& status, std::int64_t now, bool requested)
    {
        return requested || now - status.started_at >= kFailedLoadRetrySeconds;
    }

    // The FCC or ISED license data (`status`) is due: never loaded; failed
    // (and due a retry); a "running" row left behind by an older version of
    // the app, which ran imports inside a session; or a week old.
    static bool IsLicenseDataDue(const std::optional<ImportRunStatus>& status, std::int64_t now, bool requested)
    {
        if (!status.has_value() || status->status != "complete")
        {
            return !status.has_value() || status->status != "failed" || IsRetryDue(*status, now, requested);
        }
        return requested || now - status->completed_at > kUlsStalenessThresholdSeconds;
    }

    DataRefreshPlan PlanDataRefresh(Database* db, std::int64_t now)
    {
        // Its reads under one snapshot and one lock.
        Database::ReadTransaction reads(db);
        std::optional<ImportRunStatus> job = db->GetImportRunStatus(kDataRefreshJob);
        bool requested = job.has_value() && job->requested_at > job->started_at;

        DataRefreshPlan plan;

        plan.uls = IsLicenseDataDue(db->GetImportRunStatus(kUlsDataset), now, requested);
        plan.gmrs = IsLicenseDataDue(db->GetImportRunStatus(kGmrsDataset), now, requested);
        plan.ised = IsLicenseDataDue(db->GetImportRunStatus(kIsedDataset), now, requested);

        if (!db->HasAnyZipCentroids())
        {
            std::optional<ImportRunStatus> centroids = db->GetImportRunStatus(kZipCentroidsDataset);
            plan.zip_centroids =
                !centroids.has_value() || centroids->status != "failed" || IsRetryDue(*centroids, now, requested);
        }

        std::optional<ImportRunStatus> ca_postal = db->GetImportRunStatus(kCaPostalDataset);
        if (!ca_postal.has_value() || ca_postal->status != "complete")
        {
            plan.ca_postal =
                !ca_postal.has_value() || ca_postal->status != "failed" || IsRetryDue(*ca_postal, now, requested);
        }

        std::optional<ImportRunStatus> counties = db->GetImportRunStatus(kZipCountyDataset);
        if (!counties.has_value() || counties->status != "complete")
        {
            plan.zip_counties =
                !counties.has_value() || counties->status != "failed" || IsRetryDue(*counties, now, requested);
        }
        return plan;
    }

    bool DataRefreshPlanHasWork(const DataRefreshPlan& plan)
    {
        return plan.uls || plan.gmrs || plan.ised || plan.zip_centroids || plan.ca_postal || plan.zip_counties;
    }

    // Records one dataset's outcome. A failure keeps the figures from the
    // last good load (completed_at, records) so "last updated" stays true,
    // and stamps started_at with this attempt for the retry timer.
    static void RecordDatasetOutcome(Database* db, const std::string& dataset, bool ok, std::int64_t started_at,
                                     std::int64_t records, const std::string& error)
    {
        ImportRunStatus status;
        std::optional<ImportRunStatus> previous = db->GetImportRunStatus(dataset);
        if (previous.has_value())
        {
            status = *previous;
        }
        status.source = dataset;
        status.started_at = started_at;
        status.phase.clear();
        status.percent = 0;
        status.heartbeat_at = 0;
        if (ok)
        {
            status.status = "complete";
            status.completed_at = Now();
            status.records_imported = records;
            status.last_error.clear();
        }
        else
        {
            status.status = "failed";
            status.last_error = error;
        }
        db->UpsertImportRunStatus(status);
    }

    std::string RunDataRefresh(Database* db, const std::string& db_path, const DataRefreshPlan& plan,
                               bool (*should_stop)(), const DataSources& sources)
    {
        std::string cache_dir = UlsCacheDir(db_path);
        EnsureDirectory(cache_dir);
        ProgressReporter reporter(db, should_stop);

        // Each planned step's share of the overall percentage, by roughly how
        // long it takes.
        int uls_weight = plan.uls ? 80 : 0;
        int gmrs_weight = plan.gmrs ? 6 : 0;
        int ised_weight = plan.ised ? 4 : 0;
        int centroid_weight = plan.zip_centroids ? 4 : 0;
        int ca_postal_weight = plan.ca_postal ? 1 : 0;
        int county_weight = plan.zip_counties ? 6 : 0;
        int total_weight = uls_weight + gmrs_weight + ised_weight + centroid_weight + ca_postal_weight + county_weight;
        if (total_weight == 0)
        {
            return "complete";
        }
        int base = 0;
        bool all_ok = true;

        if (plan.uls)
        {
            int span = 100 * uls_weight / total_weight;
            std::int64_t started_at = Now();
            std::int64_t records = 0;
            std::string error;
            bool ok = LoadUls(sources, cache_dir, db, &reporter, base, span, &records, &error);
            if (reporter.StopRequested())
            {
                return "interrupted";
            }
            RecordDatasetOutcome(db, kUlsDataset, ok, started_at, records, error);
            all_ok = all_ok && ok;
            base += span;
        }

        if (plan.gmrs)
        {
            int span = 100 * gmrs_weight / total_weight;
            std::int64_t started_at = Now();
            std::int64_t records = 0;
            std::string error;
            bool ok = LoadGmrs(sources, cache_dir, db, &reporter, base, span, &records, &error);
            if (reporter.StopRequested())
            {
                return "interrupted";
            }
            RecordDatasetOutcome(db, kGmrsDataset, ok, started_at, records, error);
            all_ok = all_ok && ok;
            base += span;
        }

        if (plan.ised)
        {
            int span = 100 * ised_weight / total_weight;
            std::int64_t started_at = Now();
            std::int64_t records = 0;
            std::string error;
            bool ok = LoadIsed(sources, cache_dir, db, &reporter, base, span, &records, &error);
            if (reporter.StopRequested())
            {
                return "interrupted";
            }
            RecordDatasetOutcome(db, kIsedDataset, ok, started_at, records, error);
            all_ok = all_ok && ok;
            base += span;
        }

        if (plan.zip_centroids)
        {
            int span = 100 * centroid_weight / total_weight;
            std::int64_t started_at = Now();
            std::string error;
            reporter.BeginStep("Downloading ZIP code locations", base, span);
            bool ok = FetchAndLoadZipCentroids(sources, cache_dir, db, &reporter, &error);
            if (reporter.StopRequested())
            {
                return "interrupted";
            }
            RecordDatasetOutcome(db, kZipCentroidsDataset, ok, started_at, 0, error);
            all_ok = all_ok && ok;
            base += span;
        }

        if (plan.ca_postal)
        {
            int span = 100 * ca_postal_weight / total_weight;
            std::int64_t started_at = Now();
            std::string error;
            reporter.BeginStep("Downloading Canadian postal code locations", base, span);
            bool ok = FetchAndLoadCaPostalCentroids(sources, cache_dir, db, &reporter, &error);
            if (reporter.StopRequested())
            {
                return "interrupted";
            }
            RecordDatasetOutcome(db, kCaPostalDataset, ok, started_at, 0, error);
            all_ok = all_ok && ok;
            base += span;
        }

        if (plan.zip_counties)
        {
            int span = 100 - base;
            std::int64_t started_at = Now();
            std::string error;
            bool ok = FetchAndLoadZipCounties(sources, cache_dir, db, &reporter, base, span, &error);
            if (reporter.StopRequested())
            {
                return "interrupted";
            }
            RecordDatasetOutcome(db, kZipCountyDataset, ok, started_at, 0, error);
            all_ok = all_ok && ok;
        }

        return all_ok ? "complete" : "failed";
    }

    // ---- Status text ---------------------------------------------------------

    // The refresh job, if one is running right now (heartbeat still fresh).
    static std::optional<ImportRunStatus> RunningJob(Database* db, std::int64_t now)
    {
        std::optional<ImportRunStatus> job = db->GetImportRunStatus(kDataRefreshJob);
        if (job.has_value() && job->status == "running" && now - job->heartbeat_at <= kJobStaleAfterSeconds)
        {
            return job;
        }
        return std::nullopt;
    }

    // Percent rounded down to a multiple of 5, so the text (and so the
    // screen) only changes -- and only costs a redraw -- 20 times per run.
    static std::string RoundedPercent(int percent)
    {
        return std::to_string(percent / 5 * 5) + "%";
    }

    static bool UlsDataLoaded(const std::optional<ImportRunStatus>& uls)
    {
        return uls.has_value() && uls->completed_at > 0 && uls->records_imported > 0;
    }

    std::string DescribeStationDataStatus(Database* db, std::int64_t now)
    {
        std::string message;
        std::optional<ImportRunStatus> job = RunningJob(db, now);
        if (job.has_value())
        {
            message = "Updating station data: " + job->phase + ", " + RoundedPercent(job->percent) + ". ";
        }

        std::optional<ImportRunStatus> uls = db->GetImportRunStatus(kUlsDataset);
        if (uls.has_value() && UlsDataLoaded(uls))
        {
            message += "FCC license data updated " + FormatLocalDateTime(uls->completed_at) + " (" +
                       std::to_string(uls->records_imported) + " records).";
        }
        else if (!job.has_value())
        {
            message += "FCC license data not loaded yet.";
        }
        if (uls.has_value() && uls->status == "failed")
        {
            message += " Last attempt (" + FormatLocalDateTime(uls->started_at) + ") failed: " + uls->last_error +
                       " Retrying.";
        }

        // Each dataset after the FCC's starts a line of its own. "Retrying"
        // is enough: the Settings heading says the data is kept up to date
        // automatically.
        std::optional<ImportRunStatus> ised = db->GetImportRunStatus(kIsedDataset);
        std::string ised_message;
        if (UlsDataLoaded(ised))
        {
            ised_message = "Canadian (ISED) call sign data updated " + FormatLocalDateTime(ised->completed_at) + " (" +
                           std::to_string(ised->records_imported) + " records).";
        }
        if (ised.has_value() && ised->status == "failed")
        {
            if (!ised_message.empty())
            {
                ised_message += " ";
            }
            ised_message += "Canadian call sign data failed to load (" + ised->last_error + "). Retrying.";
        }
        if (!ised_message.empty())
        {
            message += "\n" + ised_message;
        }

        std::optional<ImportRunStatus> gmrs = db->GetImportRunStatus(kGmrsDataset);
        if (UlsDataLoaded(gmrs))
        {
            message += "\nFCC GMRS license data updated " + FormatLocalDateTime(gmrs->completed_at) + " (" +
                       std::to_string(gmrs->records_imported) + " records).";
        }
        if (gmrs.has_value() && gmrs->status == "failed")
        {
            message += "\nGMRS license data failed to load (" + gmrs->last_error + "). Retrying.";
        }

        std::optional<ImportRunStatus> centroids = db->GetImportRunStatus(kZipCentroidsDataset);
        if (centroids.has_value() && centroids->status == "failed" && !db->HasAnyZipCentroids())
        {
            message += "\nZIP location data failed to load (" + centroids->last_error + "); no nearby stations yet.";
        }
        std::optional<ImportRunStatus> ca_postal = db->GetImportRunStatus(kCaPostalDataset);
        if (ca_postal.has_value() && ca_postal->status == "failed")
        {
            message += "\nCanadian postal code data failed to load (" + ca_postal->last_error +
                       "); Canadian grids may be missing.";
        }
        std::optional<ImportRunStatus> counties = db->GetImportRunStatus(kZipCountyDataset);
        if (counties.has_value() && counties->status == "failed")
        {
            message += "\nCounty data failed to load (" + counties->last_error + "); County may be missing.";
        }
        return message;
    }

    std::string DescribeStationDataNotice(Database* db, std::int64_t now, bool* is_problem)
    {
        *is_problem = false;
        std::optional<ImportRunStatus> uls = db->GetImportRunStatus(kUlsDataset);
        bool loaded = UlsDataLoaded(uls);
        std::optional<ImportRunStatus> job = RunningJob(db, now);
        if (job.has_value())
        {
            return std::string(loaded ? "Updating" : "Loading") + " station data " + RoundedPercent(job->percent);
        }
        if (loaded)
        {
            return "";
        }
        if (uls.has_value() && uls->status == "failed")
        {
            *is_problem = true;
            return "Station data download failed; will retry";
        }
        return "Station data not loaded yet";
    }

}  // namespace ql
