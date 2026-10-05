#include "precise_grid.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

#include <curl/curl.h>

#include "geo_utils.hpp"
#include "version.hpp"

namespace ql
{

    // The reply is a few hundred bytes; this is for something that isn't.
    static constexpr std::size_t kMaxReplyBytes = 64 * 1024;

    static bool IsDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    // True if `street` (any case) is a PO box or a rural route's box, by whole
    // words: "123 BOXWOOD DR" is a street.
    static bool LooksLikePoBox(const std::string& street)
    {
        std::string padded;
        padded.reserve(street.size() + 2);
        padded.push_back(' ');
        for (char c : street)
        {
            // "P.O. BOX" and "P O BOX" come to the same words.
            if (c == ',')
            {
                padded.push_back(' ');
            }
            else if (c != '.')
            {
                padded.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
        }
        padded.push_back(' ');
        // "CP" is a Canadian case postale; "RR" a rural route.
        return padded.find(" BOX ") != std::string::npos || padded.find(" PO ") != std::string::npos ||
               padded.find(" POB ") != std::string::npos || padded.find(" CP ") != std::string::npos ||
               padded.find(" RR ") != std::string::npos;
    }

    static std::string StreetOnly(const std::string& street);

    bool CanLookUpGrid(const Station& station)
    {
        const std::string& street = station.street_address;
        // Only the street itself counts: "2432 MAIN ST, RR 4" is a street, a
        // rural route being how the mail finds it.
        if (station.callsign.empty() || street.empty() || !IsDigit(street[0]) || LooksLikePoBox(StreetOnly(street)))
        {
            return false;
        }
        // Five digits of ZIP (or ZIP+4), or a Canadian postal code's first
        // three characters.
        std::size_t key = ZipCentroidKey(station.zip).size();
        return key == 5 || key == 3;
    }

    // `street` up to a unit designator (", APT 4", " STE 200", " #5").
    static std::string StreetOnly(const std::string& street)
    {
        std::string::size_type cut = street.size();
        static const char* const kMarkers[] = {",", " APT ", " UNIT ", " STE ", " SUITE ", " #", " LOT ", " TRLR "};
        for (const char* marker : kMarkers)
        {
            std::string::size_type at = street.find(marker);
            if (at != std::string::npos && at < cut)
            {
                cut = at;
            }
        }
        std::string only = street.substr(0, cut);
        while (!only.empty() && only.back() == ' ')
        {
            only.pop_back();
        }
        return only;
    }

    // "5-123 Main St" (unit 5 at number 123), as Canadians write it, as
    // "123 Main St"; anything else as it is.
    static std::string WithoutUnitNumber(const std::string& street)
    {
        std::size_t i = 0;
        while (i < street.size() && IsDigit(street[i]))
        {
            ++i;
        }
        std::size_t dash = i;
        while (dash < street.size() && street[dash] == ' ')
        {
            ++dash;
        }
        if (i == 0 || dash >= street.size() || street[dash] != '-')
        {
            return street;
        }
        std::size_t number = dash + 1;
        while (number < street.size() && street[number] == ' ')
        {
            ++number;
        }
        return number < street.size() && IsDigit(street[number]) ? street.substr(number) : street;
    }

    GridRequest MakeGridRequest(const Station& station)
    {
        GridRequest request;
        request.callsign = station.callsign;
        request.street = StreetOnly(station.street_address);
        request.city = station.city;
        request.state = station.state;
        request.zip = ZipCentroidKey(station.zip);
        request.canadian = request.zip.size() == 3;
        if (request.canadian)
        {
            request.street = WithoutUnitNumber(request.street);
        }
        return request;
    }

    // `text`, escaped for a URL's query.
    static void AppendEncoded(std::string* out, const std::string& text)
    {
        static const char kHex[] = "0123456789ABCDEF";
        for (char c : text)
        {
            unsigned char byte = static_cast<unsigned char>(c);
            if (std::isalnum(byte) != 0 || c == '-' || c == '.' || c == '_' || c == '~')
            {
                out->push_back(c);
            }
            else
            {
                out->push_back('%');
                out->push_back(kHex[byte >> 4]);
                out->push_back(kHex[byte & 0x0F]);
            }
        }
    }

    std::string CensusLookupUrl(const GridRequest& request)
    {
        std::string url = "https://geocoding.geo.census.gov/geocoder/locations/address?street=";
        AppendEncoded(&url, request.street);
        if (!request.city.empty())
        {
            url += "&city=";
            AppendEncoded(&url, request.city);
        }
        if (!request.state.empty())
        {
            url += "&state=";
            AppendEncoded(&url, request.state);
        }
        url += "&zip=";
        AppendEncoded(&url, request.zip);
        url += "&benchmark=Public_AR_Current&format=json";
        return url;
    }

    std::string NrcanLookupUrl(const GridRequest& request)
    {
        std::string address = request.street;
        for (const std::string* part : {&request.city, &request.state})
        {
            if (!part->empty())
            {
                address += ", ";
                address += *part;
            }
        }
        std::string url = "https://www.geolocator.api.geo.ca/geolocation/en/locate?q=";
        AppendEncoded(&url, address);
        return url;
    }

    // The number after `"key":` at or after `from` in `json`.
    static bool NumberAfter(const std::string& json, const char* key, std::string::size_type from, double* value)
    {
        std::string::size_type at = json.find(key, from);
        if (at == std::string::npos)
        {
            return false;
        }
        at = json.find(':', at);
        if (at == std::string::npos)
        {
            return false;
        }
        const char* start = json.c_str() + at + 1;
        char* end = nullptr;
        double parsed = std::strtod(start, &end);
        if (end == start || !std::isfinite(parsed))
        {
            return false;
        }
        *value = parsed;
        return true;
    }

    bool ParseCensusPoint(const std::string& json, double* lat, double* lon)
    {
        std::string::size_type matches = json.find("\"addressMatches\"");
        if (matches == std::string::npos)
        {
            return false;
        }
        std::string::size_type point = json.find("\"coordinates\"", matches);
        if (point == std::string::npos)
        {
            return false;  // An empty list of matches.
        }
        double x = 0.0;
        double y = 0.0;
        if (!NumberAfter(json, "\"x\"", point, &x) || !NumberAfter(json, "\"y\"", point, &y))
        {
            return false;
        }
        if (!(y >= -90.0 && y <= 90.0 && x >= -180.0 && x <= 180.0) || (x == 0.0 && y == 0.0))
        {
            return false;
        }
        *lat = y;
        *lon = x;
        return true;
    }

    bool ParseNrcanPoint(const std::string& json, double* lat, double* lon)
    {
        // The first match is the best. Its qualifier says what it is.
        std::string::size_type qualifier = json.find("\"qualifier\"");
        if (qualifier == std::string::npos)
        {
            return false;
        }
        std::string::size_type open = json.find('"', json.find(':', qualifier) + 1);
        if (open == std::string::npos)
        {
            return false;
        }
        static const char kInterpolated[] = "INTERPOLATED_";
        if (json.compare(open + 1, sizeof(kInterpolated) - 1, kInterpolated) != 0)
        {
            return false;
        }
        // Its coordinates, "[longitude, latitude]", not another match's.
        std::string::size_type coordinates = json.find("\"coordinates\"", open);
        if (coordinates == std::string::npos)
        {
            return false;
        }
        std::string::size_type next_match = json.find("\"qualifier\"", open);
        if (next_match != std::string::npos && next_match < coordinates)
        {
            return false;
        }
        std::string::size_type bracket = json.find('[', coordinates);
        if (bracket == std::string::npos)
        {
            return false;
        }
        const char* start = json.c_str() + bracket + 1;
        char* end = nullptr;
        double x = std::strtod(start, &end);
        if (end == start || *end != ',')
        {
            return false;
        }
        start = end + 1;
        double y = std::strtod(start, &end);
        if (end == start || !std::isfinite(x) || !std::isfinite(y))
        {
            return false;
        }
        // Inside Canada's bounds, or it is some other place of that name.
        if (!(y >= 41.0 && y <= 84.0 && x >= -142.0 && x <= -52.0))
        {
            return false;
        }
        *lat = y;
        *lon = x;
        return true;
    }

    // True if `a` and `b` are the same text, whatever the case.
    static bool SameTextIgnoringCase(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
            {
                return false;
            }
        }
        return true;
    }

    bool ShouldTakePreciseGrid(const std::string& current, const std::string& precise, const std::string& zip_grid)
    {
        if (precise.size() != 6)
        {
            return false;
        }
        if (current.empty())
        {
            return true;
        }
        if (current.size() != 4)
        {
            return false;
        }
        // The square it lies in, or the one the ZIP's centroid gave.
        return SameTextIgnoringCase(current, precise.substr(0, 4)) ||
               (zip_grid.size() == 4 && SameTextIgnoringCase(current, zip_grid));
    }

    // ---- libcurl ------------------------------------------------------------

    static std::size_t AppendReply(char* data, std::size_t size, std::size_t count, void* target)
    {
        std::string* body = static_cast<std::string*>(target);
        if (body->size() + size * count > kMaxReplyBytes)
        {
            return 0;
        }
        body->append(data, size * count);
        return size * count;
    }

    // Stops the transfer, at the next progress report, once cancelled.
    static int StopIfCancelled(void* cancel, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        return static_cast<const std::atomic<bool>*>(cancel)->load() ? 1 : 0;
    }

    CurlGridFetcher::~CurlGridFetcher()
    {
        if (curl_ != nullptr)
        {
            curl_easy_cleanup(static_cast<CURL*>(curl_));
        }
    }

    bool CurlGridFetcher::Fetch(const std::string& url, const std::atomic<bool>& cancel, std::string* body)
    {
        if (curl_ == nullptr)
        {
            curl_ = curl_easy_init();
            if (curl_ == nullptr)
            {
                return false;
            }
        }
        else
        {
            // The options start over; the open connection and its TLS
            // session are kept for the next request.
            curl_easy_reset(static_cast<CURL*>(curl_));
        }
        CURL* curl = static_cast<CURL*>(curl_);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendReply);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, StopIfCancelled);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel);
        std::string user_agent = std::string("QuickLogger/") + QuickLoggerVersion();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
        return curl_easy_perform(curl) == CURLE_OK;
    }

    // ---- The lookup ---------------------------------------------------------

    PreciseGridLookup::PreciseGridLookup(GridFetcher* fetcher, GridResultHandler* handler,
                                         std::chrono::milliseconds quiet_after_failure)
        : fetcher_(fetcher), handler_(handler), quiet_after_failure_(quiet_after_failure)
    {
    }

    PreciseGridLookup::~PreciseGridLookup()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    std::string PreciseGridLookup::KeyOf(const GridRequest& request)
    {
        std::string key;
        key.reserve(request.callsign.size() + request.street.size() + request.zip.size() + 2);
        key += request.callsign;
        key.push_back('|');
        key += request.street;
        key.push_back('|');
        key += request.zip;
        return key;
    }

    void PreciseGridLookup::Request(GridRequest request)
    {
        std::string key = KeyOf(request);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_ || std::chrono::steady_clock::now() < quiet_until_)
            {
                return;
            }
            if (!asked_.insert(std::move(key)).second)
            {
                return;
            }
            // Never an ever-growing set in a session left running for days.
            if (asked_.size() > kMaxRemembered)
            {
                asked_.clear();
            }
            if (queue_.size() >= kMaxQueued)
            {
                queue_.pop_front();
            }
            queue_.push_back(std::move(request));
            if (!started_)
            {
                started_ = true;
                thread_ = std::thread(&PreciseGridLookup::Run, this);
            }
        }
        wake_.notify_one();
    }

    void PreciseGridLookup::Run()
    {
        while (true)
        {
            GridRequest request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, HasWork(this));
                if (stop_)
                {
                    return;
                }
                // The newest first: it's the station on screen now.
                request = std::move(queue_.back());
                queue_.pop_back();
            }
            std::string body;
            if (!fetcher_->Fetch(request.canadian ? NrcanLookupUrl(request) : CensusLookupUrl(request), stop_, &body))
            {
                // No network, or the service is down: say nothing, stop
                // asking for a minute, and let this station be asked about
                // again later.
                std::lock_guard<std::mutex> lock(mutex_);
                quiet_until_ = std::chrono::steady_clock::now() + quiet_after_failure_;
                asked_.erase(KeyOf(request));
                queue_.clear();
                continue;
            }
            double lat = 0.0;
            double lon = 0.0;
            if (!(request.canadian ? ParseNrcanPoint(body, &lat, &lon) : ParseCensusPoint(body, &lat, &lon)))
            {
                continue;  // No such address: not asked about again.
            }
            std::string grid = MaidenheadGrid6(lat, lon);
            if (!grid.empty())
            {
                handler_->OnGrid(request.callsign, grid);
            }
        }
    }

}  // namespace ql
