#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include "models.hpp"

namespace ql
{

    // The exact (6-character) grid of a US station, asked of the Census
    // Bureau's address geocoder for the one station an operator has just
    // picked. The 4-character grid from the ZIP's centroid shows at once
    // (BackfillGridFromZip); this refines it in the background a moment
    // later, and says nothing at all when it can't -- no network, a street
    // the Census doesn't know, a Canadian station, a PO box.
    //
    // Nothing here runs on the UI thread except the cheap checks: the lookup
    // is one request at a time on one thread, started when the first is
    // asked for, with short timeouts, and a failure silences it for a
    // minute.

    // What a lookup needs of a station.
    struct GridRequest
    {
        std::string callsign;
        std::string street;
        std::string city;
        std::string state;
        std::string zip;  // Five digits.
    };

    // True if `station` has a US street address worth asking about: a house
    // number and street, and a five-digit ZIP. PO boxes, Canadian stations
    // and stations with no address are not.
    bool CanLookUpGrid(const Station& station);

    // The request for `station` (see CanLookUpGrid), its street cut short
    // of an apartment or suite, which the geocoder doesn't take.
    GridRequest MakeGridRequest(const Station& station);

    // The Census geocoder URL for `request`.
    std::string CensusLookupUrl(const GridRequest& request);

    // The point the geocoder's JSON reply gives for the first address it
    // matched; false if there was no match, or the reply isn't one.
    bool ParseCensusPoint(const std::string& json, double* lat, double* lon);

    // True if a station whose grid is `current` should take `precise`: its
    // grid is blank, or is the 4-character square `precise` lies in (the
    // one the ZIP gave it, or one typed). A grid that says anything else,
    // or already holds 6 characters, is left alone.
    bool ShouldTakePreciseGrid(const std::string& current, const std::string& precise);

    // Fetches a URL. A fake one stands in for the network in tests.
    class GridFetcher
    {
    public:
        virtual ~GridFetcher() = default;

        // The reply's body in `body`; false on any failure. Gives up
        // quickly once `cancel` is set.
        virtual bool Fetch(const std::string& url, const std::atomic<bool>& cancel, std::string* body) = 0;
    };

    // The real one, over HTTPS with libcurl: 2 seconds to connect, 4 in all,
    // and a reply of no more than 64 KB. It keeps one handle, so a second
    // lookup reuses the first one's connection (no new TLS handshake): which
    // makes Fetch for one thread at a time, as the lookup's is.
    class CurlGridFetcher : public GridFetcher
    {
    public:
        CurlGridFetcher() = default;
        ~CurlGridFetcher() override;
        CurlGridFetcher(const CurlGridFetcher&) = delete;
        CurlGridFetcher& operator=(const CurlGridFetcher&) = delete;

        bool Fetch(const std::string& url, const std::atomic<bool>& cancel, std::string* body) override;

    private:
        void* curl_ = nullptr;  // A CURL*, made by the first Fetch.
    };

    // Told of each grid found. Called on the lookup's own thread.
    class GridResultHandler
    {
    public:
        virtual ~GridResultHandler() = default;
        virtual void OnGrid(const std::string& callsign, const std::string& grid) = 0;
    };

    // The lookup: Request returns at once, and the answer, if there is one,
    // comes later to the handler. The newest request is looked at first. A
    // station asked about once is not asked about again this session, unless
    // the network failed; after a failure nothing is asked for a minute.
    class PreciseGridLookup
    {
    public:
        // `quiet_after_failure` is how long nothing is asked for after a
        // network failure.
        PreciseGridLookup(GridFetcher* fetcher, GridResultHandler* handler,
                          std::chrono::milliseconds quiet_after_failure = std::chrono::seconds(60));
        ~PreciseGridLookup();

        PreciseGridLookup(const PreciseGridLookup&) = delete;
        PreciseGridLookup& operator=(const PreciseGridLookup&) = delete;

        void Request(GridRequest request);

    private:
        // The most waiting at once (the oldest is dropped), and the most
        // remembered as asked about.
        static constexpr std::size_t kMaxQueued = 8;
        static constexpr std::size_t kMaxRemembered = 512;

        class HasWork
        {
        public:
            explicit HasWork(const PreciseGridLookup* lookup) : lookup_(lookup) {}
            bool operator()() const
            {
                return lookup_->stop_ || !lookup_->queue_.empty();
            }

        private:
            const PreciseGridLookup* lookup_;
        };

        void Run();
        static std::string KeyOf(const GridRequest& request);

        GridFetcher* fetcher_;
        GridResultHandler* handler_;
        std::mutex mutex_;
        std::condition_variable wake_;
        std::deque<GridRequest> queue_;
        std::unordered_set<std::string> asked_;
        std::chrono::milliseconds quiet_after_failure_;
        std::chrono::steady_clock::time_point quiet_until_;
        std::atomic<bool> stop_{false};
        bool started_ = false;
        std::thread thread_;
    };

}  // namespace ql
