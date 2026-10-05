// The exact grid of a picked US station (precise_grid.hpp): the 6-character
// grid, what is worth asking about, the Census URL and reply, when a grid
// is taken, and the background lookup, over a fake network.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/geo_utils.hpp"
#include "../src/precise_grid.hpp"
#include "../src/ui/app_state.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // What the Census returns for one address it knows (shortened).
    static const char* const kCensusFound =
        "{\"result\":{\"input\":{\"address\":{\"zip\":\"20233\",\"street\":\"4600 Silver Hill Rd\"}},"
        "\"addressMatches\":[{\"tigerLine\":{\"side\":\"L\",\"tigerLineId\":\"657091557\"},"
        "\"coordinates\":{\"x\":-76.92836638093,\"y\":38.84505589808},"
        "\"matchedAddress\":\"4600 SILVER HILL RD, WASHINGTON, DC, 20233\"}]}}";

    static const char* const kCensusNoMatch =
        "{\"result\":{\"input\":{\"address\":{\"zip\":\"65801\",\"street\":\"1 Nowhere Zzzz St\"}},"
        "\"addressMatches\":[]}}";

    static Station ArlingtonStation(const std::string& callsign)
    {
        Station station;
        station.callsign = callsign;
        station.street_address = "4600 SILVER HILL RD";
        station.city = "WASHINGTON";
        station.state = "DC";
        station.zip = "20233";
        return station;
    }

    QL_TEST(TheSixCharacterGridIsTheUsualCase)
    {
        // ARRL headquarters, Newington, Connecticut: FN31pr.
        CHECK_EQ(MaidenheadGrid6(41.7147, -72.7272), std::string("FN31pr"));
        CHECK_EQ(MaidenheadGrid6(38.84505589808, -76.92836638093), std::string("FM18mu"));
        CHECK_EQ(MaidenheadGrid6(0.0, 0.0), std::string("JJ00aa"));
        // The corners belong to the last subsquares, never past them.
        CHECK_EQ(MaidenheadGrid6(-90.0, -180.0), std::string("AA00aa"));
        CHECK_EQ(MaidenheadGrid6(90.0, 180.0), std::string("RR99xx"));
        CHECK_EQ(MaidenheadGrid6(91.0, 0.0), std::string(""));
        CHECK_EQ(MaidenheadGrid6(0.0, -181.0), std::string(""));
        // It always starts with the 4-character grid.
        CHECK_EQ(MaidenheadGrid6(41.7147, -72.7272).substr(0, 4), MaidenheadGrid4(41.7147, -72.7272));
    }

    QL_TEST(OnlyAUsStreetAddressIsWorthAskingAbout)
    {
        Station station = ArlingtonStation("W4KWK");
        CHECK(CanLookUpGrid(station));
        station.zip = "20233-1234";
        CHECK(CanLookUpGrid(station));
        station = ArlingtonStation("W4KWK");
        station.street_address = "123 BOXWOOD DR";
        CHECK(CanLookUpGrid(station));

        station = ArlingtonStation("W4KWK");
        station.callsign.clear();
        CHECK(!CanLookUpGrid(station));
        station = ArlingtonStation("W4KWK");
        station.street_address.clear();
        CHECK(!CanLookUpGrid(station));
        station.street_address = "SILVER HILL RD";  // No house number.
        CHECK(!CanLookUpGrid(station));
        for (const char* box : {"1 PO BOX 55", "123 P.O. BOX 7", "45 BOX 12", "9 POB 3"})
        {
            station.street_address = box;
            CHECK(!CanLookUpGrid(station));
        }
        station = ArlingtonStation("VE3ABC");
        station.zip = "K1A 0B1";  // Canadian.
        CHECK(!CanLookUpGrid(station));
        station.zip.clear();
        CHECK(!CanLookUpGrid(station));
    }

    QL_TEST(ARequestKeepsTheStreetAndLeavesOutTheApartment)
    {
        Station station = ArlingtonStation("W4KWK");
        station.street_address = "4600 SILVER HILL RD APT 4";
        station.zip = "20233-1234";
        GridRequest request = MakeGridRequest(station);
        CHECK_EQ(request.callsign, std::string("W4KWK"));
        CHECK_EQ(request.street, std::string("4600 SILVER HILL RD"));
        CHECK_EQ(request.zip, std::string("20233"));
        station.street_address = "12 OAK ST, STE 200";
        CHECK_EQ(MakeGridRequest(station).street, std::string("12 OAK ST"));
        station.street_address = "77 ELM AVE #5";
        CHECK_EQ(MakeGridRequest(station).street, std::string("77 ELM AVE"));
    }

    QL_TEST(TheCensusUrlIsEscapedAndAsksForJson)
    {
        GridRequest request = MakeGridRequest(ArlingtonStation("W4KWK"));
        std::string url = CensusLookupUrl(request);
        CHECK(url.rfind("https://geocoding.geo.census.gov/geocoder/locations/address?street=", 0) == 0);
        CHECK(url.find("street=4600%20SILVER%20HILL%20RD") != std::string::npos);
        CHECK(url.find("&city=WASHINGTON") != std::string::npos);
        CHECK(url.find("&state=DC") != std::string::npos);
        CHECK(url.find("&zip=20233") != std::string::npos);
        CHECK(url.find("&benchmark=Public_AR_Current&format=json") != std::string::npos);
        // Whatever is in an address can't break out of its parameter.
        request.street = "1 A&B ST #2?x=y";
        std::string odd = CensusLookupUrl(request);
        CHECK(odd.find("street=1%20A%26B%20ST%20%232%3Fx%3Dy&") != std::string::npos);
        // Nothing to say about a missing city or state.
        request.city.clear();
        request.state.clear();
        CHECK(CensusLookupUrl(request).find("&city=") == std::string::npos);
        CHECK(CensusLookupUrl(request).find("&state=") == std::string::npos);
    }

    QL_TEST(TheCensusReplyGivesAPointOrNothing)
    {
        double lat = 0.0;
        double lon = 0.0;
        REQUIRE(ParseCensusPoint(kCensusFound, &lat, &lon));
        CHECK(lat > 38.845 && lat < 38.846);
        CHECK(lon < -76.928 && lon > -76.929);
        CHECK(!ParseCensusPoint(kCensusNoMatch, &lat, &lon));
        CHECK(!ParseCensusPoint("", &lat, &lon));
        CHECK(!ParseCensusPoint("<html>Access Denied</html>", &lat, &lon));
        CHECK(!ParseCensusPoint("{\"result\":{\"addressMatches\":[{\"coordinates\":{\"x\":-76.9}}]}}", &lat, &lon));
        CHECK(!ParseCensusPoint("{\"addressMatches\":[{\"coordinates\":{\"x\":999,\"y\":38.8}}]}", &lat, &lon));
        CHECK(!ParseCensusPoint("{\"addressMatches\":[{\"coordinates\":{\"x\":0,\"y\":0}}]}", &lat, &lon));
        CHECK(!ParseCensusPoint("{\"addressMatches\":[{\"coordinates\":{\"x\":\"a\",\"y\":\"b\"}}]}", &lat, &lon));
    }

    QL_TEST(AGridIsTakenOnlyWhereItAddsToWhatIsThere)
    {
        CHECK(ShouldTakePreciseGrid("", "FM18mu"));
        CHECK(ShouldTakePreciseGrid("FM18", "FM18mu"));
        CHECK(ShouldTakePreciseGrid("fm18", "FM18mu"));
        // A different square, or more than the 4 characters, is left alone.
        CHECK(!ShouldTakePreciseGrid("FM19", "FM18mu"));
        CHECK(!ShouldTakePreciseGrid("FM18aa", "FM18mu"));
        CHECK(!ShouldTakePreciseGrid("FM1", "FM18mu"));
        CHECK(!ShouldTakePreciseGrid("FM18", "FM18"));
        CHECK(!ShouldTakePreciseGrid("", ""));
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

    static GridRequest RequestFor(const std::string& callsign, const std::string& street)
    {
        Station station = ArlingtonStation(callsign);
        station.street_address = street;
        return MakeGridRequest(station);
    }

    QL_TEST(ALookupFindsTheGridInTheBackground)
    {
        FakeFetcher fetcher;
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results);
        lookup.Request(RequestFor("W4KWK", "4600 SILVER HILL RD"));
        REQUIRE(results.WaitForResults(1));
        CHECK_EQ(results.Found()[0], std::string("W4KWK=FM18mu"));
        REQUIRE(fetcher.Urls().size() == 1);
        CHECK(fetcher.Urls()[0].find("street=4600%20SILVER%20HILL%20RD") != std::string::npos);
    }

    QL_TEST(AStationIsAskedAboutOncePerSession)
    {
        FakeFetcher fetcher;
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results);
        lookup.Request(RequestFor("W4KWK", "4600 SILVER HILL RD"));
        REQUIRE(results.WaitForResults(1));
        lookup.Request(RequestFor("W4KWK", "4600 SILVER HILL RD"));
        lookup.Request(RequestFor("W4KWK", "4600 SILVER HILL RD"));
        // A different address of the same call sign is a new question.
        lookup.Request(RequestFor("W4KWK", "12 OAK ST"));
        REQUIRE(results.WaitForResults(2));
        CHECK_EQ(fetcher.Urls().size(), std::size_t{2});
    }

    QL_TEST(AnAddressTheCensusDoesNotKnowIsSilentAndNotAskedAgain)
    {
        FakeFetcher fetcher;
        fetcher.SetReply(kCensusNoMatch);
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results);
        lookup.Request(RequestFor("W4KWK", "1 NOWHERE ZZZZ ST"));
        REQUIRE(fetcher.WaitForUrls(1));
        // A later one is answered, so the first has been dealt with.
        fetcher.SetReply(kCensusFound);
        lookup.Request(RequestFor("K4AAA", "4600 SILVER HILL RD"));
        REQUIRE(results.WaitForResults(1));
        CHECK_EQ(results.Found()[0], std::string("K4AAA=FM18mu"));
        lookup.Request(RequestFor("W4KWK", "1 NOWHERE ZZZZ ST"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK_EQ(fetcher.Urls().size(), std::size_t{2});
        CHECK_EQ(results.Found().size(), std::size_t{1});
    }

    QL_TEST(NoNetworkIsSilentAndQuietsTheLookupForAWhile)
    {
        FakeFetcher fetcher;
        fetcher.SetFail(true);
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results, std::chrono::milliseconds(300));
        lookup.Request(RequestFor("W4KWK", "4600 SILVER HILL RD"));
        REQUIRE(fetcher.WaitForUrls(1));
        // Wait out the failure's being noted, then ask during the quiet.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        lookup.Request(RequestFor("K4AAA", "12 OAK ST"));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK_EQ(fetcher.Urls().size(), std::size_t{1});
        CHECK(results.Found().empty());
        // After it, the network is back, and the station that failed is asked again.
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        fetcher.SetFail(false);
        lookup.Request(RequestFor("W4KWK", "4600 SILVER HILL RD"));
        REQUIRE(results.WaitForResults(1));
        CHECK_EQ(results.Found()[0], std::string("W4KWK=FM18mu"));
    }

    QL_TEST(TheNewestRequestIsLookedAtFirstAndTheOldestDropWhenManyWait)
    {
        FakeFetcher fetcher;
        fetcher.Hold(true);
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results);
        lookup.Request(RequestFor("K4AAA", "1 FIRST ST"));
        REQUIRE(fetcher.WaitForUrls(1));  // In flight, held.
        // Eleven more while it waits: only the newest eight are kept.
        for (int i = 1; i <= 11; ++i)
        {
            lookup.Request(RequestFor("K4AAA", std::to_string(i) + " STREET ST"));
        }
        fetcher.Hold(false);
        REQUIRE(results.WaitForResults(9));
        std::vector<std::string> urls = fetcher.Urls();
        REQUIRE(urls.size() == 9);
        // After the first, the newest comes first, down to the 4th.
        CHECK(urls[1].find("street=11%20STREET%20ST") != std::string::npos);
        CHECK(urls[2].find("street=10%20STREET%20ST") != std::string::npos);
        CHECK(urls[8].find("street=4%20STREET%20ST") != std::string::npos);
    }

    QL_TEST(ALookupStopsAtOnceEvenWithARequestHungOnTheNetwork)
    {
        FakeFetcher fetcher;
        fetcher.Hold(true);
        FakeResults results;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        {
            PreciseGridLookup lookup(&fetcher, &results);
            lookup.Request(RequestFor("W4KWK", "4600 SILVER HILL RD"));
            REQUIRE(fetcher.WaitForUrls(1));
        }
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        CHECK(results.Found().empty());
    }

    QL_TEST(AGridThatIsNotNeededIsNotAskedFor)
    {
        // Nothing hangs off a session with no lookup.
        AppState state;
        state.grid_lookup = nullptr;
        RequestPreciseGrid(&state, ArlingtonStation("W4KWK"));

        FakeFetcher fetcher;
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results);
        state.grid_lookup = &lookup;
        // Already 6 characters, or something else entirely: leave it.
        Station station = ArlingtonStation("W4KWK");
        station.grid_square = "FM18mu";
        RequestPreciseGrid(&state, station);
        station.grid_square = "FM1";
        RequestPreciseGrid(&state, station);
        // Not a US street address.
        station = ArlingtonStation("W4KWK");
        station.street_address = "PO BOX 5";
        RequestPreciseGrid(&state, station);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(fetcher.Urls().empty());
        // Blank, or the 4 characters the ZIP gave, is asked about.
        station = ArlingtonStation("W4KWK");
        station.grid_square = "FM18";
        RequestPreciseGrid(&state, station);
        REQUIRE(results.WaitForResults(1));
        CHECK_EQ(results.Found()[0], std::string("W4KWK=FM18mu"));
    }

    QL_TEST(AFoundGridGoesInTheFormOnlyForTheStationStillShowing)
    {
        AppState state;
        state.show_new_station_modal = true;
        state.modal_station = ArlingtonStation("W4KWK");
        state.modal_station.grid_square = "FM18";
        ApplyPreciseGrid(&state, "W4KWK", "FM18mu");
        CHECK_EQ(state.modal_station.grid_square, std::string("FM18mu"));

        // The operator went on to another station before the answer came.
        state.modal_station = ArlingtonStation("K4AAA");
        state.modal_station.grid_square = "FM18";
        ApplyPreciseGrid(&state, "W4KWK", "FM18mu");
        CHECK_EQ(state.modal_station.grid_square, std::string("FM18"));

        // A grid someone typed that says something else is theirs.
        state.modal_station = ArlingtonStation("W4KWK");
        state.modal_station.grid_square = "EM75";
        ApplyPreciseGrid(&state, "W4KWK", "FM18mu");
        CHECK_EQ(state.modal_station.grid_square, std::string("EM75"));

        // The form was closed meanwhile.
        state.show_new_station_modal = false;
        state.modal_station = ArlingtonStation("W4KWK");
        ApplyPreciseGrid(&state, "W4KWK", "FM18mu");
        CHECK(state.modal_station.grid_square.empty());

        // The saved-station form is the same.
        state.show_saved_station_modal = true;
        state.saved_station = ArlingtonStation("W4KWK");
        ApplyPreciseGrid(&state, "W4KWK", "FM18mu");
        CHECK_EQ(state.saved_station.grid_square, std::string("FM18mu"));
    }

}  // namespace ql
