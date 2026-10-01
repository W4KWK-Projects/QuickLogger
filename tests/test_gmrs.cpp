// GMRS nets: the service a net is on, its channels, and keeping Amateur
// Radio and GMRS apart.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "../src/callsign_rules.hpp"
#include "../src/date_utils.hpp"
#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/gmrs_channels.hpp"
#include "../src/net_slice.hpp"
#include "../src/remote_command.hpp"
#include "../src/ui/app_state.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // An AppState on a temporary database, with no screen.
    class GmrsFixture
    {
    public:
        GmrsFixture() : db_(dir_.File("quicklogger.db"))
        {
            state.db = &db_;
            state.db_path = dir_.File("quicklogger.db");
            state.settings.callsign = "W4KWK";
            state.settings.location = "37415";
        }

        Database* db()
        {
            return &db_;
        }
        const TempDir& dir() const
        {
            return dir_;
        }

        std::int64_t AddNet(const std::string& name, NetService service)
        {
            Net net;
            net.name = name;
            net.service = service;
            return db_.CreateNet(net);
        }

        AppState state;

    private:
        TempDir dir_;
        Database db_;
    };

    QL_TEST(GmrsCallSignsHaveTheirOwnFormat)
    {
        CHECK(IsValidGmrsCallsign("WSIP663"));
        CHECK(IsValidGmrsCallsign("WRAA123"));
        CHECK(IsValidGmrsCallsign("KAE1234"));
        CHECK(!IsValidGmrsCallsign("W4KWK"));
        CHECK(!IsValidGmrsCallsign("WSIP66"));
        CHECK(!IsValidGmrsCallsign("WSIP6633"));
        CHECK(!IsValidGmrsCallsign("NSIP663"));
        CHECK(!IsValidGmrsCallsign("WS1P663"));
        CHECK(!IsValidGmrsCallsign("WSIPA63"));
        CHECK(!IsValidGmrsCallsign("wsip663"));
        CHECK(!IsValidGmrsCallsign("WSIP663/M"));
        // And the amateur check doesn't take them.
        CHECK(!IsValidCallsign("WSIP663"));
    }

    QL_TEST(GmrsHasThirtyChannels)
    {
        const std::vector<GmrsChannel>& channels = GmrsChannels();
        REQUIRE(channels.size() == 30);
        CHECK_EQ(std::string(channels[0].name), std::string("1"));
        CHECK_EQ(std::string(channels[0].frequency), std::string("462.5625"));
        CHECK_EQ(std::string(channels[7].frequency), std::string("467.5625"));
        CHECK_EQ(std::string(channels[21].name), std::string("22"));
        CHECK_EQ(std::string(channels[22].name), std::string("15R"));
        CHECK_EQ(std::string(channels[29].input), std::string("467.7250"));

        // Channel 20 and its repeater pair share an output frequency; the
        // offset tells them apart.
        CHECK_EQ(FindGmrsChannel("462.6750", ""), 19);
        CHECK_EQ(FindGmrsChannel("462.6750", "+5"), 27);
        CHECK_EQ(FindGmrsChannel("146.940", ""), -1);
        CHECK_EQ(DescribeGmrsChannel(channels[27]), std::string("20R  462.6750, input 467.6750"));
        CHECK_EQ(DescribeGmrsChannel(channels[2]), std::string("3  462.6125"));
    }

    QL_TEST(ANetKeepsItsService)
    {
        GmrsFixture f;
        std::int64_t gmrs = f.AddNet("Family Net", NetService::kGmrs);
        std::int64_t ham = f.AddNet("Skywarn", NetService::kAmateur);
        CHECK(f.db()->GetNetById(gmrs)->service == NetService::kGmrs);
        CHECK(f.db()->GetNetById(ham)->service == NetService::kAmateur);

        // Editing a net doesn't change its service.
        Net edited = *f.db()->GetNetById(gmrs);
        edited.service = NetService::kAmateur;
        f.db()->UpdateNet(edited);
        CHECK(f.db()->GetNetById(gmrs)->service == NetService::kGmrs);
    }

    QL_TEST(AGmrsNetIsSetUpByChannel)
    {
        GmrsFixture f;
        f.state.new_net_service_index = 1;
        SetNewNetService(&f.state);
        CHECK(f.state.new_net_gmrs);
        CHECK(!f.state.new_net_amateur);
        f.state.new_net_gmrs_channel = 27;  // 20R
        f.state.new_net_tone = "141.3";
        // Whatever was typed in the Amateur Radio fields doesn't count.
        f.state.new_net_frequency = "146.940";
        f.state.new_net_partial_match_index = 1;
        Net net;
        REQUIRE(ReadNewNetRadio(&f.state, &net));
        CHECK(net.service == NetService::kGmrs);
        CHECK_EQ(net.mode, std::string("FM"));
        CHECK_EQ(net.default_frequency, std::string("462.6750"));
        CHECK_EQ(net.repeater_offset, std::string("+5"));
        CHECK_EQ(net.pl_tone, std::string("141.3"));
        CHECK(!net.partial_match_canada);
        CHECK_EQ(DescribeNetRadio(net), std::string("GMRS 20R  462.6750 MHz  PL 141.3"));

        f.state.new_net_tone = "12";
        Net bad;
        CHECK(!ReadNewNetRadio(&f.state, &bad));
        CHECK(!f.state.form_error.empty());

        // Back to Amateur Radio: the usual fields, checked as before.
        f.state.new_net_service_index = 0;
        SetNewNetService(&f.state);
        f.state.new_net_tone = "";
        f.state.form_error.clear();
        Net ham;
        REQUIRE(ReadNewNetRadio(&f.state, &ham));
        CHECK(ham.service == NetService::kAmateur);
        CHECK_EQ(ham.default_frequency, std::string("146.940"));
    }

    QL_TEST(TheNetListShowsEachNetsType)
    {
        GmrsFixture f;
        Net family;
        family.name = "Family Net";
        family.service = NetService::kGmrs;
        family.default_frequency = "462.7250";
        family.repeater_offset = "+5";
        f.db()->CreateNet(family);
        f.AddNet("Skywarn", NetService::kAmateur);
        RefreshNets(&f.state);
        REQUIRE(f.state.net_names.size() == 2);
        CHECK(f.state.net_names[0].find("Family Net" + std::string(30 - 10 + 2, ' ') + "GMRS  Ch 22R") == 0);
        CHECK(f.state.net_names[1].find("Skywarn" + std::string(30 - 7 + 2, ' ') + "HAM") == 0);
    }

    QL_TEST(LongNetNamesGiveTheTypeItsRoomAt80Columns)
    {
        GmrsFixture f;
        Net net;
        net.name = "Tennessee Alabama Georgia Skywarn Weekly Net";
        net.created_at = 1790000000;
        f.db()->CreateNet(net);
        RefreshNets(&f.state);
        REQUIRE(f.state.net_names.size() == 1);
        // The name is cut at 36, so the date after it never is.
        const std::string& row = f.state.net_names[0];
        CHECK(row.find("Tennessee Alabama Georgia Skywarn We  HAM") == 0);
        CHECK(row.find("created " + FormatLocalDate(1790000000)) != std::string::npos);
        CHECK(static_cast<int>(row.size()) <= 75);
    }

    // A .qlsession of one session of `net_name` on `service`, written into
    // `dir`/`file`.
    static void WriteSession(const std::string& dir, const std::string& file, const std::string& net_name,
                             NetService service)
    {
        TempDir source_dir;
        Database source(source_dir.File("source.db"));
        Net net;
        net.name = net_name;
        net.service = service;
        std::int64_t net_id = source.CreateNet(net);
        std::int64_t instance = AddTestInstance(&source, net_id, "2026-09-14", 1789428600, "WSIP663");
        AddTestCheckIn(&source, instance, "WSIP663", 1);
        source.CloseNetInstance(instance, 1789430400);
        std::string error;
        REQUIRE(EnsureDirectory(dir));
        REQUIRE(WriteNetSliceFile(dir + "/" + file, GatherSessionSlice(&source, instance), &error));
    }

    QL_TEST(SessionFilesCarryTheirService)
    {
        TempDir dir;
        WriteSession(dir.path(), "Family.qlsession", "Family Net", NetService::kGmrs);
        std::string error;
        std::optional<NetSlice> slice = ReadSessionSliceFile(dir.File("Family.qlsession"), &error);
        REQUIRE(slice.has_value());
        CHECK(slice->net.service == NetService::kGmrs);
    }

    QL_TEST(PushedSessionsStayOnTheirService)
    {
        GmrsFixture f;
        // An Amateur Radio net of the very name, and a GMRS one like it.
        f.AddNet("Family Net", NetService::kAmateur);
        std::int64_t gmrs = f.AddNet("Family GMRS Net", NetService::kGmrs);
        std::string imports = SessionImportsDir(f.state.db_path, "W4KWK");
        WriteSession(imports, "Family.qlsession", "Family Net", NetService::kGmrs);

        RemoteCommand command;
        std::string error;
        REQUIRE(ParseRemoteCommand("import-session Family.qlsession", &command, &error));
        RemoteCommandResult result = RunRemoteCommand(command, f.db(), f.state.db_path, "W4KWK", false, 1800000000);
        CHECK_EQ(result.exit_status, kRemoteExitNeedsConfirmation);
        CHECK(result.output.find("net: Family GMRS Net\n") != std::string::npos);

        REQUIRE(ParseRemoteCommand("import-session --confirm-net \"Family Net\" Family.qlsession", &command, &error));
        result = RunRemoteCommand(command, f.db(), f.state.db_path, "W4KWK", false, 1800000000);
        CHECK_EQ(result.exit_status, kRemoteExitRefused);
        CHECK(result.output.find("is on Amateur Radio and this session was logged on GMRS") != std::string::npos);

        REQUIRE(
            ParseRemoteCommand("import-session --confirm-net \"Family GMRS Net\" Family.qlsession", &command, &error));
        result = RunRemoteCommand(command, f.db(), f.state.db_path, "W4KWK", false, 1800000000);
        CHECK_EQ(result.exit_status, kRemoteExitOk);
        CHECK_EQ(f.db()->GetNetInstancesForNet(gmrs).size(), std::size_t(1));
    }

    QL_TEST(HistoryImportStaysOnItsService)
    {
        GmrsFixture f;
        std::int64_t ham = f.AddNet("Family Net", NetService::kAmateur);
        WriteSession(ImportsDir(f.state.db_path), "Family.qlsession", "Family Net", NetService::kGmrs);
        RefreshNets(&f.state);
        f.state.selected_net_index = 0;
        f.state.page = kPageNetHistory;
        OpenSessionImport(&f.state);
        REQUIRE(!f.state.import_net_files.empty());
        ImportSelectedSession(&f.state);
        CHECK_EQ(f.state.form_error,
                 std::string("This session was logged on GMRS, and Family Net is on Amateur Radio."));
        CHECK(f.db()->GetNetInstancesForNet(ham).empty());
    }

    QL_TEST(GmrsExportsHaveNoAdif)
    {
        GmrsFixture f;
        std::int64_t net_id = f.AddNet("Family Net", NetService::kGmrs);
        std::int64_t instance_id = AddTestInstance(f.db(), net_id, "2026-09-14", 1789428600, "WSIP663");
        AddTestCheckIn(f.db(), instance_id, "WSIP663", 1);
        std::optional<NetInstance> instance = f.db()->GetNetInstanceById(instance_id);
        ExportNetLog(&f.state, "Family Net", *instance, f.db()->GetCheckInsForNetInstance(instance_id));
        CHECK(f.state.form_error.empty());
        std::string exports = ExportsDir(f.state.db_path);
        CHECK_EQ(ListFilesWithExtension(exports, ".qlsession").size(), std::size_t(1));
        CHECK_EQ(ListFilesWithExtension(exports, "_log.txt").size(), std::size_t(1));
        CHECK(ListFilesWithExtension(exports, ".adi").empty());
    }

    // Opens a session of a new GMRS net, Family Net, in `f`; returns the net.
    static std::int64_t StartGmrsSession(GmrsFixture* f)
    {
        std::int64_t net_id = f->AddNet("Family Net", NetService::kGmrs);
        NetInstance instance;
        instance.net_id = net_id;
        instance.instance_date = "2026-09-24";
        instance.started_at = 1000;
        instance.net_control_callsign = "WSIP663";
        instance.id = f->db()->CreateNetInstance(instance);
        f->state.active_instance = instance;
        f->state.active_net_name = "Family Net";
        f->state.active_net_service = NetService::kGmrs;
        f->state.operator_callsign = "WSIP663";
        return net_id;
    }

    // Logs `callsign` checking in as `name` in `f`'s session.
    static bool LogGmrs(GmrsFixture* f, const std::string& callsign, const std::string& name)
    {
        f->state.form_error.clear();
        f->state.modal_station = MakeStation(callsign, name);
        return LogStationCheckIn(&f->state);
    }

    QL_TEST(GmrsCheckInsTakeGmrsCallSigns)
    {
        GmrsFixture f;
        StartGmrsSession(&f);
        f.state.modal_station = MakeStation("W4KWK", "Wes");
        CHECK(!LogStationCheckIn(&f.state));
        CHECK_EQ(f.state.form_error, std::string("W4KWK isn't a valid GMRS call sign."));

        CHECK(LogGmrs(&f, "WRAA123", "Pat"));
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(f.state.active_instance.id).size(), std::size_t(1));
    }

    QL_TEST(AFamilyChecksInUnderOneCallSign)
    {
        GmrsFixture f;
        std::int64_t net_id = StartGmrsSession(&f);
        CHECK(LogGmrs(&f, "WRAA123", "Pat"));
        // Typed again: Pat, or someone new on the license.
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "WRAA123";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 2);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].name, std::string("Pat"));
        CHECK(f.state.modal_callsign_suggestions[1].name.empty());
        CHECK(f.state.modal_callsign_suggestion_labels[1].find("(new name)") != std::string::npos);
        f.state.selected_suggestion_index = 1;
        ApplySelectedCallsignSuggestion(&f.state);
        CHECK(f.state.modal_station.name.empty());

        CHECK(LogGmrs(&f, "WRAA123", "Sam"));
        CHECK(!LogGmrs(&f, "WRAA123", "pat"));
        CHECK_EQ(f.state.form_error, std::string("WRAA123 (pat) is already in this session's log, as #1."));

        std::vector<CheckIn> check_ins = f.db()->GetCheckInsForNetInstance(f.state.active_instance.id);
        REQUIRE(check_ins.size() == 2);
        CHECK_EQ(check_ins[0].name, std::string("Pat"));
        CHECK_EQ(check_ins[1].name, std::string("Sam"));
        // Each is saved to the net, and matched, under their own name.
        std::vector<SavedNetStation> saved = f.db()->GetSavedNetEntries(net_id);
        REQUIRE(saved.size() == 2);
        CHECK_EQ(saved[0].name, std::string("Pat"));
        CHECK_EQ(saved[1].name, std::string("Sam"));
        std::vector<Station> matches = f.db()->SearchNetStationsByCallsignSubstring(net_id, "RAA", 10);
        REQUIRE(matches.size() == 2);
        CHECK_EQ(matches[0].name, std::string("Pat"));
        CHECK_EQ(matches[1].name, std::string("Sam"));
        std::vector<CallsignTally> activity = f.db()->GetSavedStationActivity(net_id);
        REQUIRE(activity.size() == 2);
        CHECK_EQ(activity[0].count, 1);
        CHECK_EQ(activity[1].count, 1);
    }

    QL_TEST(AGmrsNetSuggestsGmrsLicenseesOnly)
    {
        GmrsFixture f;
        f.db()->BulkUpsertZipCentroids({{"37415", 35.10, -85.28}, {"37402", 35.05, -85.31}});
        f.db()->BulkUpsertUlsStations({MakeStation("WRAA123", "SMITH, PAT", "37402")}, 0, 1, 1, LicenseTable::kGmrs);
        f.db()->BulkUpsertUlsStations({MakeStation("KR4AAA", "HAM, HANK", "37402")}, 0, 1, 1);
        std::int64_t other = f.AddNet("Skywarn", NetService::kAmateur);
        f.db()->SaveNetStation(other, MakeStation("KR4AAB", "Other Net Guy"), "", 1);
        StartGmrsSession(&f);
        f.state.active_net_zip = "37415";

        f.state.modal_station.callsign = "R";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("WRAA123"));
        CHECK(f.state.modal_callsign_suggestion_labels[0].find("(ULS, ~") != std::string::npos);

        // And an Amateur Radio net, amateur licensees only.
        f.state.active_net_service = NetService::kAmateur;
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 2);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("KR4AAB"));
        CHECK_EQ(f.state.modal_callsign_suggestions[1].callsign, std::string("KR4AAA"));
    }

    QL_TEST(AnAmateurNetStillTakesACallSignOnce)
    {
        GmrsFixture f;
        std::int64_t net_id = f.AddNet("Skywarn", NetService::kAmateur);
        f.state.active_instance.net_id = net_id;
        f.state.active_instance.instance_date = "2026-09-24";
        f.state.active_instance.started_at = 1000;
        f.state.active_instance.id = f.db()->CreateNetInstance(f.state.active_instance);
        f.state.active_net_service = NetService::kAmateur;
        f.state.modal_station = MakeStation("K4AAA", "Ann");
        CHECK(LogStationCheckIn(&f.state));
        f.state.modal_station = MakeStation("K4AAA", "Bob");
        CHECK(!LogStationCheckIn(&f.state));
        CHECK(f.db()->GetCheckInsForNetInstance(f.state.active_instance.id)[0].name.empty());
        REQUIRE(f.db()->GetSavedNetEntries(net_id).size() == 1);
        CHECK(f.db()->GetSavedNetEntries(net_id)[0].name.empty());
    }

    QL_TEST(EditingAGmrsCheckInsNameMovesItsSavedEntry)
    {
        GmrsFixture f;
        std::int64_t net_id = StartGmrsSession(&f);
        REQUIRE(LogGmrs(&f, "WRAA123", "Pat"));
        REQUIRE(LogGmrs(&f, "WRAA123", "Sam"));
        std::string licensee = f.db()->FindStationByCallsign("WRAA123")->name;

        OpenEditCheckInForm(&f.state, f.state.active_check_ins[0]);
        CHECK_EQ(f.state.edit_checkin_station.name, std::string("Pat"));
        f.state.edit_checkin_station.name = "Patricia";
        CHECK(SaveEditCheckInForm(&f.state));
        CHECK_EQ(f.state.active_check_ins[0].name, std::string("Patricia"));
        CHECK_EQ(f.db()->FindStationByCallsign("WRAA123")->name, licensee);
        std::vector<SavedNetStation> saved = f.db()->GetSavedNetEntries(net_id);
        REQUIRE(saved.size() == 2);
        CHECK_EQ(saved[0].name, std::string("Patricia"));

        OpenEditCheckInForm(&f.state, f.state.active_check_ins[1]);
        f.state.edit_checkin_station.name = "PATRICIA";
        CHECK(!SaveEditCheckInForm(&f.state));
        CHECK_EQ(f.state.form_error, std::string("WRAA123 (PATRICIA) is already in this session's log, as #1."));
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(f.state.active_instance.id)[1].name, std::string("Sam"));
    }

    QL_TEST(EntryNamesIgnoreCase)
    {
        GmrsFixture f;
        std::int64_t net_id = f.AddNet("Family Net", NetService::kGmrs);
        f.db()->SaveNetStation(net_id, MakeStation("WRAA123", "Pat"), "", 1, "Jane");
        f.db()->SaveNetStation(net_id, MakeStation("WRAA123", "Pat"), "mobile", 2, "JANE");
        std::vector<SavedNetStation> saved = f.db()->GetSavedNetEntries(net_id);
        REQUIRE(saved.size() == 1);
        CHECK_EQ(saved[0].default_remarks, std::string("mobile"));
    }

    QL_TEST(EditNetSavesOneCallSignOncePerName)
    {
        GmrsFixture f;
        std::int64_t net_id = f.AddNet("Family Net", NetService::kGmrs);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        f.state.saved_station = MakeStation("WRAA123", "Pat");
        CHECK(SaveNetStationForm(&f.state));
        CHECK_EQ(f.state.status_message, std::string("Saved WRAA123 (Pat)."));
        f.state.saved_station = MakeStation("WRAA123", "Sam");
        CHECK(SaveNetStationForm(&f.state));
        REQUIRE(f.state.edit_net_saved_stations.size() == 2);
        CHECK_EQ(f.state.edit_net_saved_stations[1].name, std::string("Sam"));

        // Sam renamed to Pat: taken. To Samuel: fine.
        LoadSavedStationIntoForm(&f.state, 1);
        f.state.saved_station.name = "Pat";
        CHECK(!SaveNetStationForm(&f.state));
        CHECK_EQ(f.state.form_error, std::string("WRAA123 (Pat) is already saved to this net."));
        f.state.saved_station.name = "Samuel";
        f.state.saved_station_remarks = "mobile";
        CHECK(SaveNetStationForm(&f.state));
        std::vector<SavedNetStation> saved = f.db()->GetSavedNetEntries(net_id);
        REQUIRE(saved.size() == 2);
        CHECK_EQ(saved[1].name, std::string("Samuel"));
        CHECK_EQ(saved[1].default_remarks, std::string("mobile"));
        CHECK_EQ(f.db()->FindStationByCallsign("WRAA123")->name, std::string("Pat"));

        // Removing one leaves the other.
        f.state.selected_saved_station_index = 0;
        RemoveSelectedSavedNetStation(&f.state);
        REQUIRE(f.state.edit_net_saved_stations.size() == 1);
        CHECK_EQ(f.state.edit_net_saved_stations[0].name, std::string("Samuel"));
    }

    QL_TEST(AFamilyKeepsItsNamesThroughExportImportAndMerge)
    {
        GmrsFixture f;
        std::int64_t net_id = StartGmrsSession(&f);
        REQUIRE(LogGmrs(&f, "WRAA123", "Pat"));
        REQUIRE(LogGmrs(&f, "WRAA123", "Sam"));
        std::string file = f.dir().File("Family.qlnet");
        std::string error;
        REQUIRE(WriteNetSliceFile(file, GatherNetSlice(f.db(), net_id), &error));
        std::optional<NetSlice> slice = ReadNetSliceFile(file, &error);
        REQUIRE(slice.has_value());
        REQUIRE(slice->saved_stations.size() == 2);

        TempDir other_dir;
        Database other(other_dir.File("other.db"));
        std::int64_t imported = ApplyNetSlice(&other, *slice, 1800000000);
        std::vector<SavedNetStation> saved = other.GetSavedNetEntries(imported);
        REQUIRE(saved.size() == 2);
        CHECK_EQ(saved[0].name, std::string("Pat"));
        CHECK_EQ(saved[1].name, std::string("Sam"));
        std::vector<CheckIn> check_ins = other.GetCheckInsForNet(imported);
        REQUIRE(check_ins.size() == 2);
        CHECK_EQ(check_ins[1].name, std::string("Sam"));

        // Merged back into itself: nothing new.
        NetMergePlan plan = PlanNetMerge(f.db(), *slice, net_id);
        CHECK_EQ(plan.known_saved_stations, 2);
        CHECK_EQ(plan.new_saved_stations, 0);
        CHECK(plan.station_conflicts.empty());
        REQUIRE(plan.sessions.size() == 1);
        CHECK(plan.sessions[0].kind == MergeSessionKind::kAlreadyHere);
    }

    // A GMRS net on repeater channel 20R.
    static Net RepeaterNet(const std::string& name)
    {
        Net net;
        net.name = name;
        net.service = NetService::kGmrs;
        net.mode = "FM";
        net.default_frequency = "462.6750";
        net.repeater_offset = "+5";
        net.pl_tone = "141.3";
        return net;
    }

    QL_TEST(AGmrsNetKeepsItsChannelThroughExportAndImport)
    {
        GmrsFixture f;
        std::int64_t net_id = f.db()->CreateNet(RepeaterNet("Family Net"));
        std::string file = f.dir().File("Family.qlnet");
        std::string error;
        REQUIRE(WriteNetSliceFile(file, GatherNetSlice(f.db(), net_id), &error));
        std::optional<NetSlice> slice = ReadNetSliceFile(file, &error);
        REQUIRE(slice.has_value());
        CHECK_EQ(slice->net.default_frequency, std::string("462.6750"));
        CHECK(slice->net.comments.empty());

        TempDir other_dir;
        Database other(other_dir.File("other.db"));
        std::int64_t imported = ApplyNetSlice(&other, *slice, 1800000000);
        std::optional<Net> net = other.GetNetById(imported);
        REQUIRE(net.has_value());
        CHECK(net->service == NetService::kGmrs);
        CHECK_EQ(net->default_frequency, std::string("462.6750"));
        CHECK_EQ(net->repeater_offset, std::string("+5"));
        CHECK(net->comments.empty());
    }

    QL_TEST(AnUpgradeLeavesGmrsFrequenciesAlone)
    {
        TempDir dir;
        std::string path = dir.File("q.db");
        std::int64_t net_id = 0;
        {
            Database db(path);
            net_id = db.CreateNet(RepeaterNet("Family Net"));
        }
        // As if it were older, so every upgrade step runs again.
        sqlite3* raw = nullptr;
        sqlite3_open(path.c_str(), &raw);
        sqlite3_exec(raw, "PRAGMA user_version = 10;", nullptr, nullptr, nullptr);
        sqlite3_close(raw);
        Database db(path);
        std::optional<Net> net = db.GetNetById(net_id);
        REQUIRE(net.has_value());
        CHECK_EQ(net->default_frequency, std::string("462.6750"));
        CHECK(net->comments.empty());
    }

}  // namespace ql
