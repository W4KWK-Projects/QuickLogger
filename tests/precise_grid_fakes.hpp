#pragma once

// A fake network and the answers it gives, for the tests of the exact-grid
// lookup (precise_grid.hpp), shared by the lookup's own tests and the ones
// that go through the app's state and database.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "../src/models.hpp"
#include "../src/precise_grid.hpp"

namespace ql
{

    // What the Census returns for one address it knows (shortened).
    inline const char* const kCensusFound =
        "{\"result\":{\"input\":{\"address\":{\"zip\":\"20233\",\"street\":\"4600 Silver Hill Rd\"}},"
        "\"addressMatches\":[{\"tigerLine\":{\"side\":\"L\",\"tigerLineId\":\"657091557\"},"
        "\"coordinates\":{\"x\":-76.92836638093,\"y\":38.84505589808},"
        "\"matchedAddress\":\"4600 SILVER HILL RD, WASHINGTON, DC, 20233\"}]}}";

    inline const char* const kCensusNoMatch =
        "{\"result\":{\"input\":{\"address\":{\"zip\":\"65801\",\"street\":\"1 Nowhere Zzzz St\"}},"
        "\"addressMatches\":[]}}";

    inline Station ArlingtonStation(const std::string& callsign)
    {
        Station station;
        station.callsign = callsign;
        station.street_address = "4600 SILVER HILL RD";
        station.city = "WASHINGTON";
        station.state = "DC";
        station.zip = "20233";
        return station;
    }

    // A network that answers from a table, and can be held back.
    class FakeFetcher : public GridFetcher
    {
    public:
        bool Fetch(const std::string& url, const std::atomic<bool>& cancel, std::string* body) override
        {
            std::unique_lock<std::mutex> lock(mutex_);
            urls_.push_back(url);
            arrived_.notify_all();
            // Held until released (or cancelled), as a slow network would.
            while (hold_ && !cancel)
            {
                released_.wait_for(lock, std::chrono::milliseconds(5));
            }
            if (cancel || fail_)
            {
                return false;
            }
            *body = reply_;
            return true;
        }

        void SetReply(const std::string& reply)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reply_ = reply;
        }

        void SetFail(bool fail)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fail_ = fail;
        }

        void Hold(bool hold)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hold_ = hold;
            released_.notify_all();
        }

        // Waits until `count` URLs have been asked for.
        bool WaitForUrls(std::size_t count)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return arrived_.wait_for(lock, std::chrono::seconds(3), UrlsAtLeast(this, count));
        }

        std::vector<std::string> Urls()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return urls_;
        }

    private:
        class UrlsAtLeast
        {
        public:
            UrlsAtLeast(const FakeFetcher* fetcher, std::size_t count) : fetcher_(fetcher), count_(count) {}
            bool operator()() const
            {
                return fetcher_->urls_.size() >= count_;
            }

        private:
            const FakeFetcher* fetcher_;
            std::size_t count_;
        };

        std::mutex mutex_;
        std::condition_variable arrived_;
        std::condition_variable released_;
        std::vector<std::string> urls_;
        std::string reply_ = kCensusFound;
        bool hold_ = false;
        bool fail_ = false;
    };

    // Collects what the lookup finds.
    class FakeResults : public GridResultHandler
    {
    public:
        void OnGrid(const std::string& callsign, const std::string& grid) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            found_.push_back(callsign + "=" + grid);
            arrived_.notify_all();
        }

        bool WaitForResults(std::size_t count)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return arrived_.wait_for(lock, std::chrono::seconds(3), ResultsAtLeast(this, count));
        }

        std::vector<std::string> Found()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return found_;
        }

    private:
        class ResultsAtLeast
        {
        public:
            ResultsAtLeast(const FakeResults* results, std::size_t count) : results_(results), count_(count) {}
            bool operator()() const
            {
                return results_->found_.size() >= count_;
            }

        private:
            const FakeResults* results_;
            std::size_t count_;
        };

        std::mutex mutex_;
        std::condition_variable arrived_;
        std::vector<std::string> found_;
    };

    inline GridRequest RequestFor(const std::string& callsign, const std::string& street)
    {
        Station station = ArlingtonStation(callsign);
        station.street_address = street;
        return MakeGridRequest(station);
    }

}  // namespace ql
