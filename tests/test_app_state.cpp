// The app's behavior below the screen: logging, editing and deleting
// check-ins, numbered picks, autocomplete, county fill-in, import/export.

#include <optional>
#include <string>
#include <vector>

#include "../src/date_utils.hpp"
#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/net_slice.hpp"
#include "../src/ui/app_state.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // A database in a temp directory and an AppState wired to it, with no
    // screen. The operator is W4KWK at ZIP 37415.
    class Fixture
    {
    public:
        Fixture() : db_(dir_.File("quicklogger.db"))
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

        // Starts a session of a new net the way the Start Net flow does, with
        // the operator in `role`, and logs the operator.
        std::int64_t StartNet(const std::string& name, int role = kRoleNetControl)
        {
            std::int64_t net_id = AddTestNet(&db_, name);
            RefreshNets(&state);
            NetInstance instance;
            instance.net_id = net_id;
            instance.instance_date = "2026-09-24";
            instance.started_at = 1000;
            instance.operator_role = role;
            if (role == kRoleNetControl)
            {
                instance.net_control_callsign = "W4KWK";
            }
            else if (role == kRoleLogger)
            {
                instance.logger_callsign = "W4KWK";
            }
            else
            {
                instance.alternate_net_control_callsign = "W4KWK";
            }
            instance.id = db_.CreateNetInstance(instance);
            state.active_instance = instance;
            state.active_net_name = name;
            state.operator_callsign = "W4KWK";
            state.selected_role_index = role;
            LogOperatorCheckIn(&state);
            ClearModalFields(&state);
            return net_id;
        }

        // Logs `callsign` through the New Station modal.
        bool Log(const std::string& callsign, const std::string& name = "",
                 int role_choice_index = 0)
        {
            ClearModalFields(&state);
            state.modal_station.callsign = callsign;
            state.modal_station.name = name;
            state.modal_role_choice_index = role_choice_index;
            return LogStationCheckIn(&state);
        }

        AppState state;

    private:
        TempDir dir_;
        Database db_;
    };

    static std::vector<int> Sequences(const AppState& state)
    {
        std::vector<int> numbers;
        for (const CheckIn& check_in : state.active_check_ins)
        {
            numbers.push_back(check_in.sequence_number);
        }
        return numbers;
    }

    // ---- Logging -------------------------------------------------------------------

    QL_TEST(StartingANetLogsTheOperatorFirst)
    {
        Fixture f;
        f.StartNet("Skywarn");
        REQUIRE(f.state.active_check_ins.size() == 1);
        CHECK_EQ(f.state.active_check_ins[0].callsign, std::string("W4KWK"));
        CHECK_EQ(f.state.active_check_ins[0].sequence_number, 1);
        CHECK_EQ(f.state.active_check_ins[0].designated_role, kRoleNetControl);
    }

    QL_TEST(OnlyValidCallsignsCanBeLogged)
    {
        Fixture f;
        f.StartNet("Skywarn");
        CHECK(!f.Log("G4ABC"));
        CHECK(f.state.form_error.find("valid US or Canadian") != std::string::npos);
        CHECK(!f.Log("W4"));
        REQUIRE(f.state.active_check_ins.size() == 1);
        CHECK(f.Log("ve3abc"));
        CHECK(f.Log("W4KWK/M"));
        CHECK_EQ(f.state.active_check_ins.size(), std::size_t{3});

        f.state.saved_station.callsign = "G4ABC";
        CHECK(!SaveNetStationForm(&f.state));
        f.state.settings_form.callsign = "G4ABC";
        f.state.settings_form.location = "37415";
        CHECK(!SaveSettingsForm(&f.state));
    }

    QL_TEST(OperatorDetailsComeFromFccDataWhenUnknown)
    {
        Fixture f;
        f.db()->BulkUpsertUlsStations({MakeStation("W4KWK", "KEENE, WES", "37415")}, 0, 1, 1);
        f.db()->ReplaceZipCountyData({{"37415", "Hamilton"}}, {});
        f.StartNet("Skywarn");
        std::optional<Station> station = f.db()->FindStationByCallsign("W4KWK");
        REQUIRE(station.has_value());
        CHECK_EQ(station->name, std::string("KEENE, WES"));
        CHECK_EQ(station->county, std::string("Hamilton"));
    }

    QL_TEST(LoggingNeedsACallsign)
    {
        Fixture f;
        f.StartNet("Skywarn");
        CHECK(!f.Log(""));
        CHECK(!f.state.form_error.empty());
        CHECK(!f.Log("  "));
        CHECK_EQ(f.state.active_check_ins.size(), std::size_t{1});
    }

    QL_TEST(LoggedCallsignsAreCleanedUp)
    {
        Fixture f;
        f.StartNet("Skywarn");
        REQUIRE(f.Log(" k4abc\t"));
        REQUIRE(f.Log("w4kwk/m"));
        CHECK_EQ(f.state.active_check_ins[1].callsign, std::string("K4ABC"));
        CHECK_EQ(f.state.active_check_ins[2].callsign, std::string("W4KWK/M"));
        CHECK(f.db()->FindStationByCallsign("K4ABC").has_value());
    }

    QL_TEST(CheckInNumbersAreNeverReusedAfterADelete)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.Log("K4AAA");
        f.Log("K4BBB");
        f.Log("K4CCC");
        // Delete #2.
        f.state.selected_check_in_index = 1;
        RemoveSelectedCheckIn(&f.state);
        f.Log("K4DDD");
        CHECK(Sequences(f.state) == std::vector<int>({1, 3, 4, 5}));
    }

    QL_TEST(LoggingSavesTheStationToTheNetWithItsRemarks)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "K4AAA";
        f.state.modal_remarks = "portable";
        REQUIRE(LogStationCheckIn(&f.state));
        CHECK_EQ(f.db()->GetSavedStationsForNet(net_id).size(), std::size_t{2});
        CHECK_EQ(f.db()->GetSavedNetStationRemarks(net_id, "K4AAA"), std::string("portable"));
    }

    // ---- Roles -------------------------------------------------------------------

    QL_TEST(DesignatingARoleMovesItBetweenCheckIns)
    {
        Fixture f;
        f.StartNet("Skywarn");  // Operator is Net Control.
        // Choices: 0 none, 1 Alternate NC, 2 Logger.
        CHECK_EQ(RoleChoiceLabels(&f.state).size(), std::size_t{3});
        f.Log("K4AAA", "", 2);
        CHECK_EQ(f.state.active_instance.logger_callsign, std::string("K4AAA"));
        f.Log("K4BBB", "", 2);
        CHECK_EQ(f.state.active_instance.logger_callsign, std::string("K4BBB"));
        CHECK_EQ(f.state.active_check_ins[1].designated_role, kRoleNone);
        CHECK_EQ(f.state.active_check_ins[2].designated_role, kRoleLogger);
    }

    QL_TEST(EditingTheOperatorsOwnCheckInKeepsTheirRole)
    {
        Fixture f;
        f.StartNet("Skywarn", kRoleNetControl);
        OpenEditCheckInForm(&f.state, f.state.active_check_ins[0]);
        f.state.edit_checkin_remarks = "fixing a typo";
        SaveEditCheckInForm(&f.state);
        CHECK_EQ(f.state.active_instance.net_control_callsign, std::string("W4KWK"));
        CHECK_EQ(f.db()->GetNetInstanceById(f.state.active_instance.id)->net_control_callsign,
                 std::string("W4KWK"));
        CHECK_EQ(f.state.active_check_ins[0].designated_role, kRoleNetControl);
        CHECK_EQ(f.state.active_check_ins[0].remarks, std::string("fixing a typo"));
    }

    QL_TEST(EditingACheckInCanClearAField)
    {
        Fixture f;
        f.StartNet("Skywarn");
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "K4AAA";
        f.state.modal_station.member_id = "SP-1";
        REQUIRE(LogStationCheckIn(&f.state));
        OpenEditCheckInForm(&f.state, f.state.active_check_ins[1]);
        CHECK_EQ(f.state.edit_checkin_station.member_id, std::string("SP-1"));
        f.state.edit_checkin_station.member_id.clear();
        SaveEditCheckInForm(&f.state);
        CHECK_EQ(f.db()->FindStationByCallsign("K4AAA")->member_id, std::string(""));
    }

    QL_TEST(DeletingARoleHoldersCheckInClearsTheRole)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.Log("K4AAA", "", 1);  // Alternate NC.
        CHECK_EQ(f.state.active_instance.alternate_net_control_callsign, std::string("K4AAA"));
        f.state.selected_check_in_index = 1;
        RemoveSelectedCheckIn(&f.state);
        CHECK_EQ(f.state.active_instance.alternate_net_control_callsign, std::string(""));
    }

    // ---- Numbered picks ------------------------------------------------------------

    QL_TEST(PickingAnEmptyListExplainsWhy)
    {
        Fixture f;
        StartRowPick(&f.state, RowPickAction::kEditNet);
        CHECK_EQ(f.state.form_error, std::string("There's no net to edit."));
        CHECK(f.state.row_pick_action == RowPickAction::kNone);
    }

    QL_TEST(DeletingACheckInByItsNumber)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.Log("K4AAA", "Ann");
        f.Log("K4BBB");
        f.state.selected_check_in_index = 1;
        RemoveSelectedCheckIn(&f.state);  // Leaves #1 and #3.

        StartRowPick(&f.state, RowPickAction::kDeleteCheckIn);
        CHECK(f.state.row_pick_numbers == std::vector<int>({1, 3}));

        // A number that isn't on the list stays in pick mode and says so.
        TypeRowPickDigit(&f.state, '2');
        FinishRowPick(&f.state);
        CHECK_EQ(f.state.form_error, std::string("There's no check-in #2."));
        CHECK(f.state.row_pick_action == RowPickAction::kDeleteCheckIn);

        TypeRowPickDigit(&f.state, '3');
        CHECK_EQ(f.state.selected_check_in_index, 1);  // Highlight follows.
        FinishRowPick(&f.state);
        REQUIRE(f.state.show_row_delete_confirm_modal);
        CHECK(f.state.row_delete_lines[0].find("#3: K4BBB") != std::string::npos);

        // Esc backs out; nothing is deleted.
        CancelRowDelete(&f.state);
        CHECK_EQ(f.state.active_check_ins.size(), std::size_t{2});

        StartRowPick(&f.state, RowPickAction::kDeleteCheckIn);
        TypeRowPickDigit(&f.state, '3');
        FinishRowPick(&f.state);
        ConfirmRowDelete(&f.state);
        CHECK(Sequences(f.state) == std::vector<int>({1}));
        CHECK_EQ(f.state.status_message, std::string("Check-in deleted."));
    }

    QL_TEST(PickDigitsAreCappedAndErasable)
    {
        Fixture f;
        f.StartNet("Skywarn");
        StartRowPick(&f.state, RowPickAction::kEditCheckIn);
        for (char digit : std::string("123456"))
        {
            TypeRowPickDigit(&f.state, digit);
        }
        CHECK_EQ(f.state.row_pick_digits, std::string("1234"));
        EraseRowPickDigit(&f.state);
        CHECK_EQ(f.state.row_pick_digits, std::string("123"));
        MoveRowPickHighlight(&f.state, 5);
        CHECK(f.state.row_pick_digits.empty());
        CHECK_EQ(f.state.selected_check_in_index, 0);  // Clamped to the list.
        FinishRowPick(&f.state);  // Enter with nothing typed: the highlighted row.
        CHECK(f.state.show_edit_checkin_modal);
    }

    QL_TEST(AnOpenSessionCantBeDeletedFromHistory)
    {
        Fixture f;
        f.StartNet("Skywarn");
        RefreshNetHistory(&f.state);
        StartRowPick(&f.state, RowPickAction::kDeleteNetInstance);
        FinishRowPick(&f.state);
        CHECK(!f.state.show_row_delete_confirm_modal);
        CHECK(f.state.form_error.find("still open") != std::string::npos);
    }

    QL_TEST(DeletingAClosedSessionFromHistory)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.Log("K4AAA");
        f.db()->CloseNetInstance(f.state.active_instance.id, 2000);
        RefreshNetHistory(&f.state);
        StartRowPick(&f.state, RowPickAction::kDeleteNetInstance);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        REQUIRE(f.state.show_row_delete_confirm_modal);
        // Closed on another day than it started, so the end shows its date.
        CHECK(f.state.row_delete_lines[0].find("(ended " + FormatLocalDate(2000) + " " +
                                               FormatLocalTimeOfDay(2000) + ", 2 check-ins)") !=
              std::string::npos);
        ConfirmRowDelete(&f.state);
        CHECK(f.state.history_instances.empty());
        CHECK_EQ(f.state.status_message, std::string("Net session deleted."));
    }

    QL_TEST(HistoryShowsWhenEachSessionEnded)
    {
        Fixture f;
        f.StartNet("Skywarn");
        RefreshNetHistory(&f.state);
        std::string header = FormatNetInstanceHeaderRow();
        std::string::size_type end_column = header.find("End") - 2;  // Menu gutter.
        REQUIRE(f.state.history_instance_labels.size() == 1);
        CHECK_EQ(f.state.history_instance_labels[0].substr(end_column, 8), std::string(8, ' '));

        std::int64_t ended = 1790003600;
        f.db()->CloseNetInstance(f.state.active_instance.id, ended);
        RefreshNetHistory(&f.state);
        CHECK_EQ(f.state.history_instance_labels[0].substr(end_column, 8),
                 FormatLocalTimeOfDay(ended));
        CHECK_EQ(header.find("Net Control") - header.find("End"), std::size_t{9});
    }

    QL_TEST(ExportedLogIncludesTheEndTime)
    {
        Fixture f;
        f.StartNet("Skywarn");
        ExportNetLog(&f.state, "Skywarn", f.state.active_instance, f.state.active_check_ins);
        std::string open_log = ReadTextFile(f.dir().File("exports/Skywarn_2026-09-24_log.txt"));
        CHECK(open_log.find("End Time:") == std::string::npos);

        NetInstance closed = f.state.active_instance;
        closed.instance_date = FormatLocalDate(1790000000);
        closed.closed_at = 1790000000 + 3600;
        ExportNetLog(&f.state, "Skywarn", closed, f.state.active_check_ins);
        std::string log =
            ReadTextFile(f.dir().File("exports/Skywarn_" + closed.instance_date + "_log.txt"));
        CHECK(log.find("End Time: " + FormatLocalTimeOfDay(closed.closed_at) + "\n") !=
              std::string::npos);

        // Past midnight: the end date is shown too.
        closed.closed_at = 1790000000 + 86400;
        ExportNetLog(&f.state, "Skywarn", closed, f.state.active_check_ins);
        log = ReadTextFile(f.dir().File("exports/Skywarn_" + closed.instance_date + "_log.txt"));
        CHECK(log.find("End Time: " + FormatLocalDate(closed.closed_at) + " " +
                       FormatLocalTimeOfDay(closed.closed_at)) != std::string::npos);
    }

    QL_TEST(DeletingAHistoryCheckInClearsItsRole)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.Log("K4AAA", "", 2);  // Logger.
        f.db()->CloseNetInstance(f.state.active_instance.id, 2000);
        RefreshNetHistory(&f.state);
        StartRowPick(&f.state, RowPickAction::kDeleteHistoryCheckIn);
        TypeRowPickDigit(&f.state, '2');
        FinishRowPick(&f.state);
        REQUIRE(f.state.show_row_delete_confirm_modal);
        CHECK(f.state.row_delete_lines[0].find("2026-09-24 session") != std::string::npos);
        ConfirmRowDelete(&f.state);
        CHECK_EQ(f.state.history_check_ins.size(), std::size_t{1});
        CHECK_EQ(f.state.history_instances[0].logger_callsign, std::string(""));
    }

    QL_TEST(RemovingASavedStationSaysWhetherItsDetailsWent)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        f.db()->SaveNetStation(net_id, MakeStation("TYPO1", "Oops"), "", 1);
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA", "Ann"), "", 1);
        std::int64_t other = AddTestNet(f.db(), "Other");
        f.db()->SaveNetStation(other, MakeStation("K4AAA"), "", 1);
        RefreshNets(&f.state);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        REQUIRE(f.state.edit_net_saved_stations.size() == 2);  // K4AAA, TYPO1

        StartRowPick(&f.state, RowPickAction::kRemoveSavedStation);
        TypeRowPickDigit(&f.state, '2');
        FinishRowPick(&f.state);
        REQUIRE(f.state.show_row_delete_confirm_modal);
        CHECK(f.state.row_delete_lines[1].find("deleted too") != std::string::npos);
        ConfirmRowDelete(&f.state);
        CHECK(f.state.status_message.find("deleted too") != std::string::npos);
        CHECK(!f.db()->FindStationByCallsign("TYPO1").has_value());

        StartRowPick(&f.state, RowPickAction::kRemoveSavedStation);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        CHECK(f.state.row_delete_lines[1].find("stay") != std::string::npos);
        ConfirmRowDelete(&f.state);
        CHECK_EQ(f.state.status_message,
                 std::string("Removed K4AAA from this net's saved stations."));
        CHECK(f.state.edit_net_saved_stations.empty());
    }

    QL_TEST(EditingASavedStationByNumber)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA", "Ann"), "base", 1);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        StartRowPick(&f.state, RowPickAction::kEditSavedStation);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        CHECK_EQ(f.state.saved_station.callsign, std::string("K4AAA"));
        CHECK_EQ(f.state.saved_station_remarks, std::string("base"));
        // Clearing a field on an already-saved station really clears it.
        f.state.saved_station.name.clear();
        REQUIRE(SaveNetStationForm(&f.state));
        CHECK_EQ(f.db()->FindStationByCallsign("K4AAA")->name, std::string(""));
    }

    // A throwaway key made with ssh-keygen for the tests; nothing uses it.
    static const char* kTestKey =
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINM3eCDBCkdxto9OIGli2KKno"
        "rIhCylrEpYHnMPxdkAI test@quicklogger";

    QL_TEST(AnInvalidPublicKeyIsRefusedWithAnExample)
    {
        Fixture f;
        f.state.new_user_username = "wes";
        f.state.new_user_public_key = "AAAAC3NzaC1lZDI1NTE5AAAAINM3eCDBCkdxto9OIGli2KKno";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.find("key type is missing") != std::string::npos);
        CHECK(f.state.form_error.find("ssh-ed25519 AAAA") != std::string::npos);
        CHECK(f.db()->ListUsers().empty());
        CHECK_EQ(f.state.new_user_username, std::string("wes"));  // Form kept for fixing.

        f.state.new_user_public_key = std::string("  ") + kTestKey + "\n";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.empty());
        CHECK_EQ(f.db()->GetUserByUsername("wes")->public_key, std::string(kTestKey));
    }

    QL_TEST(RemovingAUserByNumber)
    {
        Fixture f;
        f.state.new_user_username = "wes";
        f.state.new_user_public_key = kTestKey;
        AddUserFromForm(&f.state);
        CHECK_EQ(f.state.manage_users.size(), std::size_t{1});
        AddUserFromForm(&f.state);  // Blank form now.
        CHECK(!f.state.form_error.empty());
        StartRowPick(&f.state, RowPickAction::kRemoveUser);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        ConfirmRowDelete(&f.state);
        CHECK(f.state.manage_users.empty());
    }

    // ---- Starting, resuming and closing --------------------------------------------

    QL_TEST(StartingANetWithNoOpenSessionGoesStraightToRoles)
    {
        Fixture f;
        StartSelectedNet(&f.state);
        CHECK_EQ(f.state.form_error, std::string("Create a recurring net first."));
        AddTestNet(f.db(), "Skywarn");
        RefreshNets(&f.state);
        StartSelectedNet(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK_EQ(f.state.page, kPageSelectRole);
    }

    QL_TEST(AnOpenSessionCanBeResumed)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        f.Log("K4AAA", "Ann");
        std::int64_t session = f.state.active_instance.id;
        // The connection drops: this session's state is gone.
        f.state.active_instance = NetInstance();
        f.state.active_check_ins.clear();
        f.state.operator_callsign.clear();
        f.state.page = kPageNetList;
        RefreshNets(&f.state);
        CHECK(f.state.net_names[0].find("session open") != std::string::npos);

        StartSelectedNet(&f.state);
        REQUIRE(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kResumeNet);
        CHECK(f.state.confirm_prompt_lines[0].find("2 check-ins") != std::string::npos);

        ResumeOpenNet(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK_EQ(f.state.page, kPageActiveNet);
        CHECK_EQ(f.state.active_instance.id, session);
        CHECK_EQ(f.state.active_instance.net_id, net_id);
        CHECK_EQ(f.state.active_net_name, std::string("Skywarn"));
        CHECK_EQ(f.state.operator_callsign, std::string("W4KWK"));
        CHECK_EQ(f.state.selected_role_index, kRoleNetControl);
        CHECK_EQ(f.state.active_check_ins.size(), std::size_t{2});
        CHECK(f.state.status_message.find("Resumed") == 0);
        f.Log("K4BBB");
        CHECK(Sequences(f.state) == std::vector<int>({1, 2, 3}));
    }

    QL_TEST(AnOpenSessionCanBeClosedToStartANewOne)
    {
        Fixture f;
        f.StartNet("Skywarn");
        std::int64_t old_session = f.state.active_instance.id;
        f.state.page = kPageNetList;
        StartSelectedNet(&f.state);
        REQUIRE(f.state.show_confirm_prompt);
        CloseOpenNetAndStartNew(&f.state);
        std::optional<NetInstance> old = f.db()->GetNetInstanceById(old_session);
        CHECK(old->status == NetInstanceStatus::kClosed);
        // Ended when its last check-in was logged, not whenever it got closed.
        CHECK_EQ(old->closed_at, f.db()->GetCheckInsForNetInstance(old_session)[0].checked_in_at);
        CHECK_EQ(f.state.page, kPageSelectRole);
        CHECK(f.state.net_names[0].find("session open") == std::string::npos);
    }

    QL_TEST(ClosingTheNetAsksFirst)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.Log("K4AAA");
        RequestCloseActiveNet(&f.state);
        REQUIRE(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kCloseNet);
        CHECK(f.state.confirm_prompt_lines[0].find("Skywarn (2 check-ins)") != std::string::npos);

        CancelConfirmPrompt(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK(f.db()->GetNetInstanceById(f.state.active_instance.id)->status ==
              NetInstanceStatus::kOpen);

        RequestCloseActiveNet(&f.state);
        CloseActiveNet(&f.state);
        CHECK(f.db()->GetNetInstanceById(f.state.active_instance.id)->status ==
              NetInstanceStatus::kClosed);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK(f.state.status_message.find("History") != std::string::npos);
    }

    QL_TEST(TheNetListShowsWhenEachNetWasCreatedOrImported)
    {
        Fixture f;
        Net mine;
        mine.name = "Skywarn";
        mine.created_at = 1790000000;
        f.db()->CreateNet(mine);
        Net theirs = mine;
        theirs.imported_at = 1790100000;
        f.db()->CreateNet(theirs);
        Net old;
        old.name = "Old";
        f.db()->CreateNet(old);
        RefreshNets(&f.state);
        REQUIRE(f.state.net_names.size() == 3);
        CHECK_EQ(f.state.net_names[0], std::string("Old"));
        CHECK(f.state.net_names[1].find("Skywarn  created " + FormatLocalDate(1790000000)) == 0);
        CHECK(f.state.net_names[2].find("Skywarn  imported " + FormatLocalDate(1790100000)) == 0);
    }

    // ---- Autocomplete --------------------------------------------------------------

    static void LoadZipData(Database* db)
    {
        std::vector<ZipCentroid> centroids = {{"37415", 35.10, -85.28},
                                              {"37402", 35.05, -85.31},
                                              {"30752", 34.87, -85.51},
                                              {"37201", 36.16, -86.78},
                                              {"90210", 34.09, -118.40}};
        db->BulkUpsertZipCentroids(centroids);
        db->ReplaceZipCountyData({{"37415", "Hamilton"}, {"37402", "Hamilton"}, {"30752", "Dade"}},
                                 {{"30752", "RISING FAWN", "Walker"}});
    }

    QL_TEST(AutocompleteOrdersThisNetThenOtherNetsThenNearbyLicenses)
    {
        Fixture f;
        LoadZipData(f.db());
        std::int64_t other = AddTestNet(f.db(), "Other");
        f.db()->SaveNetStation(other, MakeStation("K4AAB", "Other Net Guy"), "", 1);
        f.db()->BulkUpsertUlsStations({MakeStation("K4AAA", "ULS DUPLICATE", "37415"),
                                       MakeStation("K4AAC", "NEARBY, NED", "37402"),
                                       MakeStation("K4AAD", "FAR, FRED", "90210"),
                                       MakeStation("K4AAE", "NASHVILLE, NAN", "37201")},
                                      0, 4, 1);
        f.StartNet("Skywarn");
        f.Log("K4AAA", "Ann");

        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "k4aa";
        RefreshCallsignSuggestions(&f.state);
        std::vector<Station>& found = f.state.modal_callsign_suggestions;
        REQUIRE(found.size() == 3);
        CHECK_EQ(found[0].callsign, std::string("K4AAA"));
        CHECK_EQ(found[1].callsign, std::string("K4AAB"));
        CHECK_EQ(found[2].callsign, std::string("K4AAC"));
        REQUIRE(f.state.modal_callsign_suggestion_labels.size() == 3);
        CHECK(f.state.modal_callsign_suggestion_labels[0].find("(this net)") != std::string::npos);
        CHECK(f.state.modal_callsign_suggestion_labels[1].find("(other net)") != std::string::npos);
        CHECK(f.state.modal_callsign_suggestion_labels[2].find("(ULS, ~") != std::string::npos);

        // Picking the ULS match fills in its county from its ZIP.
        f.state.selected_suggestion_index = 2;
        ApplySelectedCallsignSuggestion(&f.state);
        CHECK_EQ(f.state.modal_station.name, std::string("NEARBY, NED"));
        CHECK_EQ(f.state.modal_station.county, std::string("Hamilton"));
        CHECK(f.state.modal_callsign_suggestions.empty());
    }

    QL_TEST(LicenseSuggestionsAreNearestFirst)
    {
        Fixture f;
        LoadZipData(f.db());
        // Alphabetical order is the reverse of distance order.
        f.db()->BulkUpsertUlsStations(
            {MakeStation("K4AAA", "TRENTON", "30752"), MakeStation("K4BBB", "DOWNTOWN", "37402"),
             MakeStation("K4CCC", "HOME", "37415")},
            0, 3, 1);
        f.StartNet("Skywarn");
        f.state.modal_station.callsign = "K4";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 3);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("K4CCC"));
        CHECK_EQ(f.state.modal_callsign_suggestions[1].callsign, std::string("K4BBB"));
        CHECK_EQ(f.state.modal_callsign_suggestions[2].callsign, std::string("K4AAA"));
        CHECK(f.state.modal_callsign_suggestion_labels[0].find("(ULS, ~0 mi)") !=
              std::string::npos);
    }

    QL_TEST(AutocompleteShowsAtMostEight)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        for (int i = 0; i < 12; ++i)
        {
            f.db()->SaveNetStation(net_id, MakeStation("K4X" + std::to_string(10 + i)), "", 1);
        }
        f.state.modal_station.callsign = "K4X";
        RefreshCallsignSuggestions(&f.state);
        CHECK_EQ(f.state.modal_callsign_suggestions.size(), std::size_t{8});
        CHECK_EQ(f.state.modal_callsign_suggestion_labels.size(), std::size_t{8});
        f.state.modal_station.callsign.clear();
        RefreshCallsignSuggestions(&f.state);
        CHECK(f.state.modal_callsign_suggestions.empty());
    }

    QL_TEST(PickingASavedStationPrefillsItsRemarks)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA", "Ann"), "mobile", 1);
        f.state.modal_station.callsign = "AAA";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(!f.state.modal_callsign_suggestions.empty());
        ApplySelectedCallsignSuggestion(&f.state);
        CHECK_EQ(f.state.modal_station.callsign, std::string("K4AAA"));
        CHECK_EQ(f.state.modal_remarks, std::string("mobile"));
    }

    QL_TEST(SavedStationFormAutocompleteHasTheSameTiers)
    {
        Fixture f;
        LoadZipData(f.db());
        f.db()->BulkUpsertUlsStations({MakeStation("K4AAC", "NEARBY, NED", "37402")}, 0, 1, 1);
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA"), "", 1);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        f.state.saved_station.callsign = "K4AA";
        RefreshSavedStationSuggestions(&f.state);
        REQUIRE(f.state.saved_station_suggestions.size() == 2);
        CHECK_EQ(f.state.saved_station_suggestions[1].callsign, std::string("K4AAC"));
        f.state.selected_saved_station_suggestion_index = 1;
        ApplySelectedSavedStationSuggestion(&f.state);
        CHECK_EQ(f.state.saved_station.county, std::string("Hamilton"));
    }

    QL_TEST(NoHomeZipMeansNoLicenseSuggestions)
    {
        Fixture f;
        LoadZipData(f.db());
        f.db()->BulkUpsertUlsStations({MakeStation("K4AAC", "NEARBY, NED", "37402")}, 0, 1, 1);
        f.state.settings.location = "00000";
        f.StartNet("Skywarn");
        f.state.modal_station.callsign = "K4AAC";
        RefreshCallsignSuggestions(&f.state);
        CHECK(f.state.modal_callsign_suggestions.empty());
    }

    QL_TEST(LicenseSuggestionsCenterOnTheNetsZip)
    {
        Fixture f;
        LoadZipData(f.db());
        f.db()->BulkUpsertUlsStations({MakeStation("K4AAC", "CHATTANOOGA", "37402"),
                                       MakeStation("K4AAE", "NASHVILLE", "37201")},
                                      0, 2, 1);
        f.StartNet("Skywarn");
        f.state.active_net_zip = "37201";
        f.state.modal_station.callsign = "K4AA";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("K4AAE"));
        CHECK(f.state.modal_callsign_suggestion_labels[0].find("(ULS, ~0 mi)") !=
              std::string::npos);

        // A net ZIP with no location on file, or none at all, falls back to
        // the operator's home ZIP.
        f.state.active_net_zip = "99999";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("K4AAC"));
        f.state.active_net_zip.clear();
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("K4AAC"));
    }

    QL_TEST(SavedStationFormCentersOnTheNetsZip)
    {
        Fixture f;
        LoadZipData(f.db());
        f.db()->BulkUpsertUlsStations({MakeStation("K4AAC", "CHATTANOOGA", "37402"),
                                       MakeStation("K4AAE", "NASHVILLE", "37201")},
                                      0, 2, 1);
        Net net;
        net.name = "Nashville Net";
        net.default_location = "37201";
        std::int64_t net_id = f.db()->CreateNet(net);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        f.state.saved_station.callsign = "K4AA";
        RefreshSavedStationSuggestions(&f.state);
        REQUIRE(f.state.saved_station_suggestions.size() == 1);
        CHECK_EQ(f.state.saved_station_suggestions[0].callsign, std::string("K4AAE"));
    }

    // ---- County fill-in ------------------------------------------------------------

    QL_TEST(CountyComesFromTheTownInAStraddlingZip)
    {
        Fixture f;
        LoadZipData(f.db());
        Station station = MakeStation("K4AAA", "", "30752", "Rising Fawn");
        BackfillCountyFromZip(&f.state, &station);
        CHECK_EQ(station.county, std::string("Walker"));

        Station elsewhere = MakeStation("K4BBB", "", "30752", "Trenton");
        BackfillCountyFromZip(&f.state, &elsewhere);
        CHECK_EQ(elsewhere.county, std::string("Dade"));

        Station zip_plus_four = MakeStation("K4CCC", "", "374152623");
        BackfillCountyFromZip(&f.state, &zip_plus_four);
        CHECK_EQ(zip_plus_four.county, std::string("Hamilton"));

        Station already = MakeStation("K4DDD", "", "37415");
        already.county = "Marion";
        BackfillCountyFromZip(&f.state, &already);
        CHECK_EQ(already.county, std::string("Marion"));

        Station unknown = MakeStation("K4EEE", "", "99999");
        BackfillCountyFromZip(&f.state, &unknown);
        CHECK_EQ(unknown.county, std::string(""));
    }

    // ---- Formatting ----------------------------------------------------------------

    QL_TEST(CheckInRowsLineUpUnderTheirHeader)
    {
        CheckIn check_in;
        check_in.sequence_number = 12;
        check_in.callsign = "W4KWK";
        check_in.designated_role = kRoleLogger;
        check_in.remarks = "remarks here";
        std::string header = FormatCheckInHeaderRow(false);
        std::string row =
            FormatCheckInRow(check_in, "A Name That Is Far Too Long To Fit", "M123", "Hamilton");
        CHECK_EQ(header.find("Callsign"), row.find("W4KWK"));
        CHECK_EQ(header.find("Member ID"), row.find("M123"));
        CHECK_EQ(header.find("County"), row.find("Hamilton"));
        CHECK_EQ(header.find("Role"), row.find("Log "));
        CHECK_EQ(header.find("Remarks"), row.find("remarks here"));
        CHECK(row.find("Too Long To Fit") == std::string::npos);  // Truncated.
        CHECK_EQ(FormatCheckInHeaderRow(true), "  " + header);
    }

    // ---- Settings -------------------------------------------------------------------

    QL_TEST(ChoosingThe24HourClockAppliesEverywhere)
    {
        Fixture f;
        f.state.settings_path = f.dir().File("settings.txt");
        OpenSettingsForm(&f.state);
        CHECK_EQ(f.state.settings_time_format_index, 0);
        f.state.settings_time_format_index = 1;
        REQUIRE(SaveSettingsForm(&f.state));
        bool applied = Use24HourClock();
        std::string shown = FormatLocalTimeOfDay(1790000000);
        SetUse24HourClock(false);  // Back to the default for other tests.

        CHECK(applied);
        CHECK(f.state.settings.use_24_hour_clock);
        CHECK(shown.find('M') == std::string::npos);  // No AM/PM.
        CHECK(LoadSettings(f.state.settings_path).use_24_hour_clock);
        OpenSettingsForm(&f.state);
        CHECK_EQ(f.state.settings_time_format_index, 1);

        f.state.settings_form.location = "374";
        CHECK(!SaveSettingsForm(&f.state));
        CHECK(!f.state.form_error.empty());
    }

    // ---- Import and export ---------------------------------------------------------

    QL_TEST(ExportedLogLandsInExportsNextToTheDatabase)
    {
        Fixture f;
        f.StartNet("Sky/warn: Net");
        f.Log("K4AAA", "Ann");
        ExportNetLog(&f.state, "Sky/warn: Net", f.state.active_instance, f.state.active_check_ins);
        CHECK(f.state.form_error.empty());
        std::vector<std::string> files = ListFilesWithExtension(f.dir().File("exports"), ".txt");
        REQUIRE(files.size() == 1);
        std::string text = ReadTextFile(f.dir().File("exports/" + files[0]));
        CHECK(text.find("Net: Sky/warn: Net") != std::string::npos);
        CHECK(text.find("K4AAA") != std::string::npos);
        CHECK(text.find("Ann") != std::string::npos);
        CHECK(f.state.status_message.find("Saved to") == 0);
    }

    QL_TEST(ImportingANetFileAddsItAsANewNet)
    {
        Fixture f;
        {
            Database other(f.dir().File("other.db"));
            std::int64_t net_id = AddTestNet(&other, "Their Net");
            std::int64_t session = AddTestInstance(&other, net_id, "2026-01-01", 1, "N0XYZ");
            AddTestCheckIn(&other, session, "N0XYZ", 1);
            std::string error;
            REQUIRE(WriteNetSliceFile(ImportsDir(f.state.db_path) + "/their.qlnet",
                                      GatherNetSlice(&other, net_id), &error));
        }
        WriteTextFile(ImportsDir(f.state.db_path) + "/broken.qlnet", "not a database at all...");

        RefreshImportNetFiles(&f.state);
        REQUIRE(f.state.import_net_files.size() == 2);  // broken, their

        f.state.selected_import_file_index = 0;
        ImportSelectedNetSlice(&f.state);
        CHECK(!f.state.form_error.empty());
        CHECK(f.state.nets.empty());

        f.state.selected_import_file_index = 1;
        ImportSelectedNetSlice(&f.state);
        CHECK(f.state.form_error.empty());
        REQUIRE(f.state.nets.size() == 1);
        CHECK_EQ(f.state.nets[0].name, std::string("Their Net"));
        CHECK_EQ(f.state.page, kPageNetList);
    }

    QL_TEST(DeletingANetFromEditNet)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Doomed");
        f.db()->CloseNetInstance(f.state.active_instance.id, 2000);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        RequestDeleteNet(&f.state);
        CHECK(f.state.show_delete_net_confirm_modal);
        CancelDeleteNet(&f.state);
        CHECK_EQ(f.state.nets.size(), std::size_t{1});
        RequestDeleteNet(&f.state);
        ConfirmDeleteNet(&f.state);
        CHECK(f.state.nets.empty());
        CHECK(f.state.status_message.find("Doomed") != std::string::npos);
        CHECK(!f.db()->FindStationByCallsign("W4KWK").has_value());
    }

    QL_TEST(EditNetNeedsAName)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Keep");
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        f.state.edit_net_name.clear();
        CHECK(!SaveEditNetForm(&f.state));
        CHECK_EQ(f.db()->GetNetById(net_id)->name, std::string("Keep"));
        f.state.edit_net_name = "Renamed";
        CHECK(SaveEditNetForm(&f.state));
        CHECK_EQ(f.state.nets[0].name, std::string("Renamed"));
    }

    QL_TEST(NetZipMustBeFiveDigitsOrBlank)
    {
        Fixture f;
        Net net;
        net.name = "Old";
        net.default_location = "Chattanooga, TN";
        std::int64_t net_id = f.db()->CreateNet(net);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        CHECK(!SaveEditNetForm(&f.state));
        CHECK(!f.state.form_error.empty());
        f.state.edit_net_location = "3740";
        CHECK(!SaveEditNetForm(&f.state));
        f.state.edit_net_location = "37402";
        CHECK(SaveEditNetForm(&f.state));
        CHECK_EQ(f.db()->GetNetById(net_id)->default_location, std::string("37402"));
        f.state.edit_net_location.clear();
        CHECK(SaveEditNetForm(&f.state));
        CHECK(f.db()->GetNetById(net_id)->default_location.empty());
    }

}  // namespace ql
