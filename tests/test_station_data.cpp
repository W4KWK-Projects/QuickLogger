// The station-data refresh: what's due, and a full run against local
// fixture files standing in for the FCC and Census downloads.

#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/uls_import.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static std::int64_t Now()
    {
        return static_cast<std::int64_t>(std::time(nullptr));
    }

    static ImportRunStatus MakeStatus(const std::string& source, const std::string& status,
                                      std::int64_t started_at, std::int64_t completed_at,
                                      std::int64_t records)
    {
        ImportRunStatus run;
        run.source = source;
        run.status = status;
        run.started_at = started_at;
        run.completed_at = completed_at;
        run.records_imported = records;
        return run;
    }

    // Marks every dataset as freshly loaded, so nothing is due.
    static void MarkAllLoaded(Database* db, std::int64_t now)
    {
        db->UpsertImportRunStatus(MakeStatus(kUlsDataset, "complete", now - 10, now - 5, 100));
        db->UpsertImportRunStatus(MakeStatus(kZipCountyDataset, "complete", now - 10, now - 5, 0));
        ZipCentroid centroid;
        centroid.zip = "37415";
        centroid.lat = 35.1;
        centroid.lon = -85.3;
        db->BulkUpsertZipCentroids({centroid});
    }

    // ---- Planning --------------------------------------------------------------

    QL_TEST(EverythingIsDueOnAFreshDatabase)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        DataRefreshPlan plan = PlanDataRefresh(&db, Now());
        CHECK(plan.uls);
        CHECK(plan.zip_centroids);
        CHECK(plan.zip_counties);
        CHECK(DataRefreshPlanHasWork(plan));
    }

    QL_TEST(NothingIsDueRightAfterALoad)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t now = Now();
        MarkAllLoaded(&db, now);
        CHECK(!DataRefreshPlanHasWork(PlanDataRefresh(&db, now)));
    }

    QL_TEST(UlsDataIsRefreshedWeekly)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t now = Now();
        MarkAllLoaded(&db, now);
        db.UpsertImportRunStatus(
            MakeStatus(kUlsDataset, "complete", 0, now - kUlsStalenessThresholdSeconds + 60, 100));
        CHECK(!PlanDataRefresh(&db, now).uls);
        db.UpsertImportRunStatus(
            MakeStatus(kUlsDataset, "complete", 0, now - kUlsStalenessThresholdSeconds - 60, 100));
        DataRefreshPlan plan = PlanDataRefresh(&db, now);
        CHECK(plan.uls);
        CHECK(!plan.zip_centroids);
        CHECK(!plan.zip_counties);
    }

    QL_TEST(AFailedLoadWaitsBeforeRetrying)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t now = Now();
        MarkAllLoaded(&db, now);
        db.UpsertImportRunStatus(MakeStatus(kUlsDataset, "failed", now - 60, 0, 0));
        CHECK(!PlanDataRefresh(&db, now).uls);
        db.UpsertImportRunStatus(
            MakeStatus(kUlsDataset, "failed", now - kFailedLoadRetrySeconds - 1, 0, 0));
        CHECK(PlanDataRefresh(&db, now).uls);
    }

    QL_TEST(ARefreshRequestMakesUlsAndFailuresDueNow)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t now = Now();
        MarkAllLoaded(&db, now);
        db.UpsertImportRunStatus(MakeStatus(kZipCountyDataset, "failed", now - 60, 0, 0));
        CHECK(!DataRefreshPlanHasWork(PlanDataRefresh(&db, now)));

        db.RequestImportRun(kDataRefreshJob, now);
        DataRefreshPlan plan = PlanDataRefresh(&db, now);
        CHECK(plan.uls);
        CHECK(plan.zip_counties);
        CHECK(!plan.zip_centroids);  // Loaded fine; no need to fetch again.
    }

    QL_TEST(ARunLeftBehindByAnOldVersionIsRedone)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t now = Now();
        MarkAllLoaded(&db, now);
        db.UpsertImportRunStatus(MakeStatus(kUlsDataset, "running", now - 60, 0, 0));
        CHECK(PlanDataRefresh(&db, now).uls);
    }

    // ---- A full run against fixture files ------------------------------------

    // One line of a delimited file with `count` fields, all blank except
    // those given (index, value).
    static std::string Row(char separator, std::size_t count,
                           const std::vector<std::pair<std::size_t, std::string>>& fields)
    {
        std::vector<std::string> values(count);
        for (const std::pair<std::size_t, std::string>& field : fields)
        {
            values[field.first] = field.second;
        }
        std::string line;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i > 0)
            {
                line += separator;
            }
            line += values[i];
        }
        return line;
    }

    static std::string EnRow(const std::string& id, const std::string& name,
                             const std::string& city, const std::string& state,
                             const std::string& zip)
    {
        return Row('|', 30,
                   {{0, "EN"},
                    {1, id},
                    {7, name},
                    {15, "1 Main St"},
                    {16, city},
                    {17, state},
                    {18, zip}}) +
               "\r\n";
    }

    // 2020 ZIP-county and ZIP-town relationship rows share a layout.
    static std::string RelRow(const std::string& zip, const std::string& geoid,
                              const std::string& name, const std::string& land)
    {
        return Row('|', 18, {{1, zip}, {9, geoid}, {10, name}, {16, land}}) + "\n";
    }

    static std::string PopulationRow(const std::string& zip, const std::string& county_geoid,
                                     const std::string& people, const std::string& zip_people)
    {
        return Row(',', 24, {{0, zip}, {3, county_geoid}, {4, people}, {8, zip_people}}) + "\r\n";
    }

    // Writes a small but realistic set of "downloads" into `dir` and returns
    // DataSources pointing at them.
    static DataSources WriteFixtures(const TempDir& dir)
    {
        // FCC: HD (licenses), EN (names/addresses), AM (class), tied together
        // by record id; only active ("A") licenses count. CRLF line endings,
        // as FCC ships them.
        std::string hd =
            "HD|1001|||W4KWK|A|HA\r\n"
            "HD|1002|||AA4FA|A|HA\r\n"
            "HD|1003|||K4OLD|E|HA\r\n"  // Expired.
            "HD|1004|||N4ZIP|A|HA\r\n"
            "HD|junk|||BAD1|A|HA\r\n"  // Unreadable id.
            "short|line\r\n";
        std::string en = EnRow("1001", "KEENE, WES", "CHATTANOOGA", "TN", "37415") +
                         EnRow("1002", "ABLE, ANN", "NEWTON", "MA", "02467") +
                         EnRow("1003", "OLD, EXPIRED", "X", "TN", "37415") +
                         EnRow("1004", "PLUS, FOUR", "TRENTON", "GA", "307524915") +
                         EnRow("9999", "NO LICENSE", "X", "TN", "37415");
        std::string am =
            "AM|1001|||W4KWK|E|\r\n"
            "AM|1002|||AA4FA|G|\r\n"
            "AM|1004|||N4ZIP|Q|\r\n";  // Unknown class code.
        WriteZipFile(dir.File("l_amat.zip"), {{"HD.dat", hd, true},
                                              {"EN.dat", en, true},
                                              {"AM.dat", am, false},
                                              {"counts", "not needed", true}});

        // Census gazetteer: tab-separated, header row, lat/lon in fields 5
        // and 6, the last column padded with spaces as in the real file.
        std::string padding(70, ' ');
        std::string gazetteer =
            "GEOID\tALAND\tAWATER\tALAND_SQMI\tAWATER_SQMI\tINTPTLAT\tINTPTLONG" + padding + "\n" +
            "37415\t1\t1\t1\t1\t35.1\t-85.28" + padding + "\n" +
            "30752\t1\t1\t1\t1\t34.87\t-85.51" + padding + "\n" +
            "99999\t1\t1\t1\t1\tnot-a-number\t-85\n" + "short\n";
        WriteZipFile(dir.File("gaz.zip"), {{"gaz.txt", gazetteer, true}});

        // ZIP -> county (2020). 37415 is all Hamilton. 02467 straddles
        // three counties; Norfolk has the most land but Middlesex the most
        // people. 30752 has no population figures, so land decides (Dade).
        std::string county_rel = "header\n" + RelRow("37415", "47065", "Hamilton County", "500") +
                                 RelRow("02467", "25017", "Middlesex County", "100") +
                                 RelRow("02467", "25021", "Norfolk County", "300") +
                                 RelRow("02467", "25025", "Suffolk County", "50") +
                                 RelRow("30752", "13083", "Dade County", "400") +
                                 RelRow("30752", "13295", "Walker County", "100") + "junk\n";
        WriteTextFile(dir.File("county.txt"), county_rel);

        std::string population = "header\r\n" + PopulationRow("37415", "47065", "9000", "9000") +
                                 PopulationRow("02467", "25017", "6000", "10000") +
                                 PopulationRow("02467", "25021", "3000", "10000") +
                                 PopulationRow("02467", "25025", "1000", "10000") +
                                 PopulationRow("11111", "99999", "5", "0");
        WriteTextFile(dir.File("population.txt"), population);

        // Towns inside ZIPs. Rising Fawn spans both 30752 counties; Walker
        // has more of it. "District 6" is a Census label, not a town.
        std::string towns = "header\n" + RelRow("02467", "2501745560", "Newton city", "100") +
                            RelRow("02467", "2502109175", "Brookline town", "300") +
                            RelRow("02467", "2502507000", "Boston city", "50") +
                            RelRow("02467", "2502500000", "District 6", "1") +
                            RelRow("30752", "1308300001", "Rising Fawn CCD", "10") +
                            RelRow("30752", "1329500001", "Rising Fawn CCD", "90") +
                            RelRow("37415", "4706500001", "Chattanooga city", "500");
        WriteTextFile(dir.File("towns.txt"), towns);

        DataSources sources;
        sources.uls_zip_url = FileUrl(dir.File("l_amat.zip"));
        sources.zip_gazetteer_url = FileUrl(dir.File("gaz.zip"));
        sources.zip_gazetteer_file_name = "gaz.txt";
        sources.zcta_county_url = FileUrl(dir.File("county.txt"));
        sources.zcta_county_population_url = FileUrl(dir.File("population.txt"));
        sources.zcta_county_subdivision_url = FileUrl(dir.File("towns.txt"));
        return sources;
    }

    static DataRefreshPlan FullPlan()
    {
        DataRefreshPlan plan;
        plan.uls = true;
        plan.zip_centroids = true;
        plan.zip_counties = true;
        return plan;
    }

    static std::string CountyFor(Database* db, const std::string& zip)
    {
        for (const ZipCounty& zip_county : db->GetAllZipCounties())
        {
            if (zip_county.zip == zip)
            {
                return zip_county.county;
            }
        }
        return "(none)";
    }

    static std::string PlaceCountyFor(Database* db, const std::string& zip,
                                      const std::string& place)
    {
        for (const ZipPlaceCounty& zip_place : db->GetAllZipPlaceCounties())
        {
            if (zip_place.zip == zip && zip_place.place == place)
            {
                return zip_place.county;
            }
        }
        return "(none)";
    }

    QL_TEST(AFullRefreshLoadsEveryDataset)
    {
        TempDir dir;
        DataSources sources = WriteFixtures(dir);
        std::string db_path = dir.File("q.db");
        Database db(db_path);
        REQUIRE(db.TryClaimImportRun(kDataRefreshJob, Now(), kJobStaleAfterSeconds));

        CHECK_EQ(RunDataRefresh(&db, db_path, FullPlan(), nullptr, sources),
                 std::string("complete"));

        // FCC licenses: active ones only, joined up, ZIP+4 trimmed.
        std::optional<Station> wes = db.FindUlsStationByCallsign("W4KWK");
        REQUIRE(wes.has_value());
        CHECK_EQ(wes->name, std::string("KEENE, WES"));
        CHECK_EQ(wes->city, std::string("CHATTANOOGA"));
        CHECK_EQ(wes->zip, std::string("37415"));
        CHECK_EQ(wes->license_class, std::string("Extra"));
        CHECK_EQ(db.FindUlsStationByCallsign("AA4FA")->license_class, std::string("General"));
        std::optional<Station> plus_four = db.FindUlsStationByCallsign("N4ZIP");
        REQUIRE(plus_four.has_value());
        CHECK_EQ(plus_four->zip, std::string("30752"));
        CHECK_EQ(plus_four->license_class, std::string(""));
        CHECK(!db.FindUlsStationByCallsign("K4OLD").has_value());
        CHECK(!db.FindUlsStationByCallsign("BAD1").has_value());
        CHECK(!db.FindUlsStationByCallsign("NOBODY").has_value());

        std::optional<ImportRunStatus> uls = db.GetImportRunStatus(kUlsDataset);
        REQUIRE(uls.has_value());
        CHECK_EQ(uls->status, std::string("complete"));
        CHECK_EQ(uls->records_imported, std::int64_t{3});
        CHECK(uls->completed_at > 0);

        // Gazetteer: good rows only, padded last column and all.
        std::vector<ZipCentroid> centroids = db.GetAllZipCentroids();
        CHECK_EQ(centroids.size(), std::size_t{2});
        CHECK_EQ(db.GetImportRunStatus(kZipCentroidsDataset)->status, std::string("complete"));

        // Counties: by population where known, by land where not.
        CHECK_EQ(CountyFor(&db, "37415"), std::string("Hamilton"));
        CHECK_EQ(CountyFor(&db, "02467"), std::string("Middlesex"));
        CHECK_EQ(CountyFor(&db, "30752"), std::string("Dade"));
        CHECK_EQ(db.GetImportRunStatus(kZipCountyDataset)->status, std::string("complete"));

        // Towns, only for ZIPs that straddle a county line.
        CHECK_EQ(PlaceCountyFor(&db, "02467", "NEWTON"), std::string("Middlesex"));
        CHECK_EQ(PlaceCountyFor(&db, "02467", "BROOKLINE"), std::string("Norfolk"));
        CHECK_EQ(PlaceCountyFor(&db, "02467", "BOSTON"), std::string("Suffolk"));
        CHECK_EQ(PlaceCountyFor(&db, "02467", "DISTRICT"), std::string("(none)"));
        CHECK_EQ(PlaceCountyFor(&db, "30752", "RISING FAWN"), std::string("Walker"));
        CHECK_EQ(PlaceCountyFor(&db, "37415", "CHATTANOOGA"), std::string("(none)"));

        // Downloads went next to the database, not anywhere else.
        CHECK(FileExists(dir.File("uls_cache/l_amat.zip")));
    }

    QL_TEST(ARefreshDropsLicensesThatAreNoLongerListed)
    {
        TempDir dir;
        DataSources sources = WriteFixtures(dir);
        std::string db_path = dir.File("q.db");
        Database db(db_path);
        db.BulkUpsertUlsStations({MakeStation("K9GONE", "EXPIRED, SINCE", "37415")}, 0, 1, 1);
        DataRefreshPlan plan;
        plan.uls = true;
        CHECK_EQ(RunDataRefresh(&db, db_path, plan, nullptr, sources), std::string("complete"));
        CHECK(!db.FindUlsStationByCallsign("K9GONE").has_value());
        CHECK(db.FindUlsStationByCallsign("W4KWK").has_value());
    }

    QL_TEST(AFailedDownloadIsRecordedAndOthersStillLoad)
    {
        TempDir dir;
        DataSources sources = WriteFixtures(dir);
        sources.uls_zip_url = FileUrl(dir.File("missing.zip"));
        std::string db_path = dir.File("q.db");
        Database db(db_path);

        CHECK_EQ(RunDataRefresh(&db, db_path, FullPlan(), nullptr, sources), std::string("failed"));
        std::optional<ImportRunStatus> uls = db.GetImportRunStatus(kUlsDataset);
        REQUIRE(uls.has_value());
        CHECK_EQ(uls->status, std::string("failed"));
        CHECK(!uls->last_error.empty());
        CHECK(db.HasAnyZipCentroids());
        CHECK_EQ(CountyFor(&db, "37415"), std::string("Hamilton"));

        bool is_problem = false;
        std::string notice = DescribeStationDataNotice(&db, Now(), &is_problem);
        CHECK(is_problem);
        CHECK(notice.find("failed") != std::string::npos);
        std::string status = DescribeStationDataStatus(&db, Now(), true);
        CHECK(status.find("retried automatically") != std::string::npos);
    }

    QL_TEST(AFailedRefreshKeepsTheLastGoodLoadsFigures)
    {
        TempDir dir;
        DataSources sources = WriteFixtures(dir);
        std::string db_path = dir.File("q.db");
        Database db(db_path);
        DataRefreshPlan plan;
        plan.uls = true;
        REQUIRE(RunDataRefresh(&db, db_path, plan, nullptr, sources) == "complete");
        std::int64_t completed_at = db.GetImportRunStatus(kUlsDataset)->completed_at;

        WriteZipFile(
            dir.File("l_amat.zip"),
            {{"HD.dat", "HD|1|||X|E|\r\n", true}, {"EN.dat", "", true}, {"AM.dat", "", true}});
        CHECK_EQ(RunDataRefresh(&db, db_path, plan, nullptr, sources), std::string("failed"));
        std::optional<ImportRunStatus> uls = db.GetImportRunStatus(kUlsDataset);
        CHECK_EQ(uls->status, std::string("failed"));
        CHECK_EQ(uls->completed_at, completed_at);
        CHECK_EQ(uls->records_imported, std::int64_t{3});
        // Still usable: the old data stays, and nothing alarming is shown.
        CHECK(db.FindUlsStationByCallsign("W4KWK").has_value());
        bool is_problem = true;
        CHECK_EQ(DescribeStationDataNotice(&db, Now(), &is_problem), std::string(""));
        CHECK(!is_problem);
    }

    QL_TEST(ACorruptArchiveFailsCleanly)
    {
        TempDir dir;
        DataSources sources = WriteFixtures(dir);
        WriteTextFile(dir.File("l_amat.zip"), "<html>Service unavailable</html>");
        std::string db_path = dir.File("q.db");
        Database db(db_path);
        DataRefreshPlan plan;
        plan.uls = true;
        CHECK_EQ(RunDataRefresh(&db, db_path, plan, nullptr, sources), std::string("failed"));
        CHECK(!db.HasAnyUlsStations());
    }

    static bool AlwaysStop()
    {
        return true;
    }

    QL_TEST(AStopRequestInterruptsWithoutRecordingAFailure)
    {
        TempDir dir;
        DataSources sources = WriteFixtures(dir);
        std::string db_path = dir.File("q.db");
        Database db(db_path);
        CHECK_EQ(RunDataRefresh(&db, db_path, FullPlan(), AlwaysStop, sources),
                 std::string("interrupted"));
        CHECK(!db.GetImportRunStatus(kUlsDataset).has_value());
    }

    QL_TEST(AnEmptyPlanDoesNothing)
    {
        TempDir dir;
        std::string db_path = dir.File("q.db");
        Database db(db_path);
        CHECK_EQ(RunDataRefresh(&db, db_path, DataRefreshPlan(), nullptr, DataSources()),
                 std::string("complete"));
        CHECK(!db.GetImportRunStatus(kUlsDataset).has_value());
    }

    // ---- Status text -----------------------------------------------------------

    QL_TEST(StatusNoticeTracksTheRunningJob)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t now = Now();
        bool is_problem = true;
        CHECK_EQ(DescribeStationDataNotice(&db, now, &is_problem),
                 std::string("Station data not loaded yet"));
        CHECK(!is_problem);

        db.TryClaimImportRun(kDataRefreshJob, now, kJobStaleAfterSeconds);
        db.UpdateImportProgress(kDataRefreshJob, "Downloading FCC license data", 47, 0, now);
        CHECK_EQ(DescribeStationDataNotice(&db, now, &is_problem),
                 std::string("Loading station data 45%"));
        std::string status = DescribeStationDataStatus(&db, now, true);
        CHECK(status.find("Downloading FCC license data, 45%") != std::string::npos);
        CHECK(status.find("Press F3") == std::string::npos);  // Already running.

        db.UpsertImportRunStatus(MakeStatus(kUlsDataset, "complete", now, now, 800000));
        CHECK_EQ(DescribeStationDataNotice(&db, now, &is_problem),
                 std::string("Updating station data 45%"));

        // The worker died: its heartbeat went stale.
        CHECK_EQ(DescribeStationDataNotice(&db, now + kJobStaleAfterSeconds + 1, &is_problem),
                 std::string(""));
    }

    QL_TEST(StatusTextDescribesLoadedData)
    {
        TempDir dir;
        Database db(dir.File("q.db"));
        std::int64_t now = Now();
        db.UpsertImportRunStatus(MakeStatus(kUlsDataset, "complete", now, now, 812345));
        std::string status = DescribeStationDataStatus(&db, now, true);
        CHECK(status.find("FCC license data last updated") != std::string::npos);
        CHECK(status.find("(812345 records)") != std::string::npos);
        CHECK(status.find("Press F3 to refresh now.") != std::string::npos);
        CHECK(DescribeStationDataStatus(&db, now, false).find("F3") == std::string::npos);

        db.UpsertImportRunStatus(MakeStatus(kZipCountyDataset, "failed", now, 0, 0));
        CHECK(DescribeStationDataStatus(&db, now, false).find("County data failed") !=
              std::string::npos);
    }

}  // namespace ql
