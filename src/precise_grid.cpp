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
        return padded.find(" BOX ") != std::string::npos || padded.find(" PO ") != std::string::npos ||
               padded.find(" POB ") != std::string::npos;
    }

    bool CanLookUpGrid(const Station& station)
    {
        const std::string& street = station.street_address;
        if (station.callsign.empty() || street.empty() || !IsDigit(street[0]) || LooksLikePoBox(street))
        {
            return false;
        }
        // Five digits of ZIP (or ZIP+4): a US address; a Canadian postal
        // code is a different key.
        return ZipCentroidKey(station.zip).size() == 5;
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

    GridRequest MakeGridRequest(const Station& station)
    {
        GridRequest request;
        request.callsign = station.callsign;
        request.street = StreetOnly(station.street_address);
        request.city = station.city;
        request.state = station.state;
        request.zip = ZipCentroidKey(station.zip);
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

    bool ShouldTakePreciseGrid(const std::string& current, const std::string& precise)
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
        for (std::size_t i = 0; i < 4; ++i)
        {
            if (std::toupper(static_cast<unsigned char>(current[i])) !=
                std::toupper(static_cast<unsigned char>(precise[i])))
            {
                return false;
            }
        }
        return true;
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
            if (!fetcher_->Fetch(CensusLookupUrl(request), stop_, &body))
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
            if (!ParseCensusPoint(body, &lat, &lon))
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
