#include "update_check.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <vector>

#include <curl/curl.h>

#include "version.hpp"

namespace ql
{

    std::string ReleaseVersionFromJson(const std::string& json)
    {
        std::string::size_type key = json.find("\"tag_name\"");
        if (key == std::string::npos)
        {
            return "";
        }
        std::string::size_type colon = json.find(':', key);
        std::string::size_type open = colon == std::string::npos ? colon : json.find('"', colon);
        std::string::size_type close = open == std::string::npos ? open : json.find('"', open + 1);
        if (close == std::string::npos)
        {
            return "";
        }
        std::string tag = json.substr(open + 1, close - open - 1);
        if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
        {
            tag.erase(0, 1);
        }
        return tag;
    }

    // `version`'s dot-separated parts as numbers; false if it isn't just
    // digits and dots.
    static bool VersionParts(const std::string& version, std::vector<long>* parts)
    {
        parts->clear();
        long part = 0;
        bool have_digit = false;
        for (char c : version)
        {
            if (std::isdigit(static_cast<unsigned char>(c)) != 0)
            {
                if (part > 1000000)
                {
                    return false;
                }
                part = part * 10 + (c - '0');
                have_digit = true;
            }
            else if (c == '.' && have_digit)
            {
                parts->push_back(part);
                part = 0;
                have_digit = false;
            }
            else
            {
                return false;
            }
        }
        if (!have_digit)
        {
            return false;
        }
        parts->push_back(part);
        return true;
    }

    bool IsNewerVersion(const std::string& candidate, const std::string& current)
    {
        std::vector<long> a;
        std::vector<long> b;
        if (!VersionParts(candidate, &a) || !VersionParts(current, &b))
        {
            return false;
        }
        std::size_t count = std::max(a.size(), b.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            long x = i < a.size() ? a[i] : 0;
            long y = i < b.size() ? b[i] : 0;
            if (x != y)
            {
                return x > y;
            }
        }
        return false;
    }

    static std::size_t AppendToString(char* data, std::size_t size, std::size_t count,
                                      void* userdata)
    {
        std::string* body = static_cast<std::string*>(userdata);
        // A release's description is a few KB; anything this big isn't one.
        if (body->size() > 1024 * 1024)
        {
            return 0;
        }
        body->append(data, size * count);
        return size * count;
    }

    bool FetchLatestReleaseVersion(const std::string& url, std::string* version, std::string* error)
    {
        CURL* curl = curl_easy_init();
        if (curl == nullptr)
        {
            *error = "Failed to initialize libcurl.";
            return false;
        }
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        // GitHub's API refuses requests without one.
        std::string user_agent = std::string("QuickLogger/") + QuickLoggerVersion();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
        CURLcode result = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK)
        {
            *error = std::string("Update check failed: ") + curl_easy_strerror(result);
            return false;
        }
        *version = ReleaseVersionFromJson(body);
        if (version->empty())
        {
            *error = "Update check failed: no version in GitHub's reply.";
            return false;
        }
        return true;
    }

    static std::mutex g_update_mutex;
    static std::string g_available_update;
    static std::atomic<bool> g_update_check_enabled{true};

    std::string AvailableUpdate()
    {
        std::lock_guard<std::mutex> lock(g_update_mutex);
        return g_update_check_enabled ? g_available_update : std::string();
    }

    void SetAvailableUpdate(const std::string& version)
    {
        std::lock_guard<std::mutex> lock(g_update_mutex);
        g_available_update = version;
    }

    bool UpdateCheckEnabled()
    {
        return g_update_check_enabled;
    }

    void SetUpdateCheckEnabled(bool enabled)
    {
        g_update_check_enabled = enabled;
        if (!enabled)
        {
            SetAvailableUpdate("");
        }
    }

}  // namespace ql
