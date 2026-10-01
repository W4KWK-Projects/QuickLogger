// GMRS nets: the service a net is on, its channels, and keeping Amateur
// Radio and GMRS apart.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

    QL_TEST(GmrsCheckInsTakeGmrsCallSigns)
    {
        GmrsFixture f;
        std::int64_t net_id = f.AddNet("Family Net", NetService::kGmrs);
        NetInstance instance;
        instance.net_id = net_id;
        instance.instance_date = "2026-09-24";
        instance.started_at = 1000;
        instance.net_control_callsign = "WSIP663";
        instance.id = f.db()->CreateNetInstance(instance);
        f.state.active_instance = instance;
        f.state.active_net_name = "Family Net";
        f.state.active_net_service = NetService::kGmrs;
        f.state.operator_callsign = "WSIP663";

        f.state.modal_station = MakeStation("W4KWK", "Wes");
        CHECK(!LogStationCheckIn(&f.state));
        CHECK_EQ(f.state.form_error, std::string("W4KWK isn't a valid GMRS call sign."));

        f.state.form_error.clear();
        f.state.modal_station = MakeStation("WRAA123", "Pat");
        CHECK(LogStationCheckIn(&f.state));
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(instance.id).size(), std::size_t(1));
    }

}  // namespace ql
