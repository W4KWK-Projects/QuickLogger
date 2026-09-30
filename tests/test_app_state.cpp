// The app's behavior below the screen: logging, editing and deleting
// check-ins, numbered picks, autocomplete, county fill-in, import/export.

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../src/date_utils.hpp"
#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/net_slice.hpp"
#include "../src/show_folder.hpp"
#include "../src/ui/app_state.hpp"
#include "../src/ui/handlers.hpp"
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

    QL_TEST(LoginHighlightsTheNetLastLogged)
    {
        Fixture f;
        AddTestNet(f.db(), "ARES");
        std::int64_t skywarn = AddTestNet(f.db(), "Skywarn");
        std::int64_t tag = AddTestNet(f.db(), "TAG");
        RefreshNets(&f.state);
        HighlightLastLoggedNet(&f.state);
        CHECK_EQ(f.state.selected_net_index, 0);  // Never logged one: left alone.

        AddTestInstance(f.db(), tag, "2026-01-01", 1, "W4KWK");
        AddTestInstance(f.db(), skywarn, "2026-01-02", 2, "W4KWK");
        HighlightLastLoggedNet(&f.state);
        CHECK_EQ(f.state.nets[static_cast<std::size_t>(f.state.selected_net_index)].id, skywarn);
    }

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
        CHECK(f.Log("K4ABC/M"));
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
        REQUIRE(f.Log("k4abd/m"));
        CHECK_EQ(f.state.active_check_ins[1].callsign, std::string("K4ABC"));
        CHECK_EQ(f.state.active_check_ins[2].callsign, std::string("K4ABD/M"));
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

    QL_TEST(CheckInsSomeoneElseLogsShowUpOnTheActiveNet)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.state.page = kPageActiveNet;
        RefreshActiveCheckIns(&f.state);
        CHECK_EQ(f.state.watched_instance_id.load(), f.state.active_instance.id);
        CHECK_EQ(f.state.shown_check_in_count.load(), std::int64_t{1});

        // Another operator on the same session logs one.
        AddTestCheckIn(f.db(), f.state.active_instance.id, "K4OTH", 2);
        std::int64_t count = 0;
        std::int64_t newest = 0;
        f.db()->GetCheckInSummary(f.state.active_instance.id, &count, &newest);
        CHECK_EQ(count, std::int64_t{2});
        CHECK(newest != f.state.shown_newest_check_in_id.load());

        // Not while the Edit Check-In dialog is working from the list.
        f.state.show_edit_checkin_modal = true;
        RefreshActiveCheckInsFromOthers(&f.state, f.state.active_instance.id);
        CHECK_EQ(f.state.active_check_ins.size(), std::size_t{1});
        f.state.show_edit_checkin_modal = false;
        RefreshActiveCheckInsFromOthers(&f.state, f.state.active_instance.id);
        REQUIRE(f.state.active_check_ins.size() == 2);
        CHECK_EQ(f.state.active_check_ins[1].callsign, std::string("K4OTH"));
        CHECK_EQ(f.state.shown_newest_check_in_id.load(), newest);

        // Leaving the net stops the watching.
        RequestCloseActiveNet(&f.state);
        CloseActiveNet(&f.state);
        CHECK_EQ(f.state.watched_instance_id.load(), std::int64_t{0});
    }

    // The Net Closed prompt's text, one line after another.
    static std::string PromptText(const AppState& state)
    {
        std::string text;
        for (const std::string& line : state.confirm_prompt_lines)
        {
            text += line + "\n";
        }
        return text;
    }

    QL_TEST(NothingIsLoggedToASessionSomeoneElseClosed)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.Log("K4AAA");
        // Another operator sharing the session closes it.
        CHECK(f.db()->CloseNetInstance(f.state.active_instance.id, 2000));
        f.state.show_new_station_modal = true;
        CHECK(!f.Log("K4BBB"));
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(f.state.active_instance.id).size(),
                 std::size_t{2});
        CHECK(!f.state.show_new_station_modal);
        CHECK(f.state.form_error.empty());
        REQUIRE(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kSessionClosed);
        CHECK_EQ(f.state.confirm_prompt_title, std::string("Net Closed"));
        std::string text = PromptText(f.state);
        CHECK(text.find("Another user has closed this net at ") == 0);
        CHECK(text.find(FormatLocalTimeOfDay(2000) + ".\n") != std::string::npos);
        CHECK(text.find("K4BBB was not logged.\n") != std::string::npos);
        CHECK(text.find("You will be returned to the Recurring Nets list when you press Enter.") !=
              std::string::npos);
        CHECK_EQ(f.state.watched_instance_id.load(), std::int64_t{0});
        // Enter returns to the net list.
        LeaveClosedSession(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK(f.state.form_error.empty());
        // Nor can the check-in window be opened again.
        f.state.page = kPageActiveNet;
        CHECK(!EnsureActiveSessionOpen(&f.state, ""));
        CHECK(PromptText(f.state).find("was not logged") == std::string::npos);
    }

    QL_TEST(ANetClosedWhileTypingACheckInNamesTheCallsign)
    {
        Fixture f;
        f.StartNet("Skywarn");
        CHECK(f.db()->CloseNetInstance(f.state.active_instance.id, 2000));
        // Seen by the ticker, not by logging: what was typed is named.
        f.state.show_new_station_modal = true;
        f.state.modal_station.callsign = "K4CCC";
        CHECK(!EnsureActiveSessionOpen(&f.state, ""));
        CHECK(!f.state.show_new_station_modal);
        CHECK(PromptText(f.state).find("K4CCC was not logged.") != std::string::npos);
    }

    QL_TEST(NothingIsLoggedToASessionSomeoneElseDeleted)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.db()->DeleteNetInstance(f.state.active_instance.id);
        CHECK(!f.Log("K4BBB"));
        REQUIRE(f.state.show_confirm_prompt);
        CHECK_EQ(f.state.confirm_prompt_title, std::string("Session Deleted"));
        CHECK(PromptText(f.state).find("Another user has deleted this net's session.") == 0);
        LeaveClosedSession(&f.state);
        CHECK_EQ(f.state.page, kPageNetList);
    }

    QL_TEST(ClosingASessionSomeoneElseClosedKeepsTheirEndTime)
    {
        Fixture f;
        f.StartNet("Skywarn");
        CHECK(f.db()->CloseNetInstance(f.state.active_instance.id, 2000));
        RequestCloseActiveNet(&f.state);
        CloseActiveNet(&f.state);
        CHECK_EQ(f.db()->GetNetInstanceById(f.state.active_instance.id)->closed_at,
                 std::int64_t{2000});
        REQUIRE(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kSessionClosed);
        CHECK(PromptText(f.state).find("Another user has closed this net at ") == 0);
        CHECK(PromptText(f.state).find(FormatLocalTimeOfDay(2000)) != std::string::npos);
        LeaveClosedSession(&f.state);
        CHECK_EQ(f.state.page, kPageNetList);
    }

    QL_TEST(ClosingASessionSomeoneElseDeletedSaysSo)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.db()->DeleteNetInstance(f.state.active_instance.id);
        RequestCloseActiveNet(&f.state);
        CloseActiveNet(&f.state);
        REQUIRE(f.state.show_confirm_prompt);
        CHECK_EQ(f.state.confirm_prompt_title, std::string("Session Deleted"));
    }

    QL_TEST(WhoeverClosesTheNetIsNotToldSomeoneElseDid)
    {
        Fixture f;
        f.StartNet("Skywarn");
        RequestCloseActiveNet(&f.state);
        CloseActiveNet(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK(f.state.status_message.find("Closed Skywarn") == 0);
        // Their own close stopped the watching, so the ticker says nothing.
        CHECK_EQ(f.state.watched_instance_id.load(), std::int64_t{0});
    }

    QL_TEST(AViewerIsToldWhenTheNetIsClosed)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        f.state.resume_instance = f.state.active_instance;
        f.state.start_net = *f.db()->GetNetById(net_id);
        ViewOpenNet(&f.state);
        REQUIRE(f.state.viewing_only);
        CHECK(f.db()->CloseNetInstance(f.state.active_instance.id, 2000));
        CHECK(!EnsureActiveSessionOpen(&f.state, ""));
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kSessionClosed);
        LeaveClosedSession(&f.state);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK(!f.state.viewing_only);
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
        std::string header = NetInstanceListHeader(80, false);
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
        f.state.new_user_username = "K4WES";
        f.state.new_user_public_key = "AAAAC3NzaC1lZDI1NTE5AAAAINM3eCDBCkdxto9OIGli2KKno";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.find("key type is missing") != std::string::npos);
        CHECK(f.state.form_error.find("ssh-ed25519 AAAA") != std::string::npos);
        CHECK(f.db()->ListUsers().empty());
        CHECK_EQ(f.state.new_user_username, std::string("K4WES"));  // Form kept for fixing.

        f.state.new_user_public_key = std::string("  ") + kTestKey + "\n";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.empty());
        std::vector<User> keys = f.db()->GetUserKeys("K4WES");
        REQUIRE(keys.size() == 1);
        CHECK_EQ(keys[0].public_key, std::string(kTestKey));
    }

    // A second throwaway key, for a user with two.
    static const char* kOtherTestKey =
        "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBPYZZ9VZA4tifTMUe"
        "aD4+NLAlPM4vzya7Gu9uPVDpEo2sNfAt3I7zE92dNSClawZGhwfo1iPr+IIYJgRU6d/hzc= desktop";

    QL_TEST(UsersAreListedOnceWithTheirKeysInAWindow)
    {
        Fixture f;
        f.state.new_user_username = "K4WES";
        f.state.new_user_public_key = kTestKey;
        AddUserFromForm(&f.state);
        f.state.new_user_username = "K4WES";
        f.state.new_user_public_key = kOtherTestKey;
        AddUserFromForm(&f.state);
        CHECK_EQ(f.state.status_message,
                 std::string("Added another key for \"K4WES\" (full access)."));
        // One row for the user, however many keys.
        REQUIRE(f.state.manage_users_labels.size() == 1);
        CHECK(f.state.manage_users_labels[0].find("Full") != std::string::npos);
        CHECK(f.state.manage_users_labels[0].find(" 2 ") != std::string::npos);
        CHECK(UserListHeader(80).find("Keys") != std::string::npos);

        // F4 by number opens the Edit User window, with their keys told
        // apart by type, fingerprint and comment.
        StartRowPick(&f.state, RowPickAction::kEditUser);
        CHECK(RowPickPrompt(&f.state).find("Edit which user?") == 0);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        REQUIRE(f.state.show_user_keys_modal);
        CHECK_EQ(f.state.user_keys_username, std::string("K4WES"));
        REQUIRE(f.state.user_keys_labels.size() == 2);
        CHECK(f.state.user_keys_labels[0].find("ED25519 SHA256:zSpp/") != std::string::npos);
        CHECK(f.state.user_keys_labels[0].find("test@quicklogger") != std::string::npos);
        CHECK(f.state.user_keys_labels[0].find("never") != std::string::npos);
        CHECK(f.state.user_keys_labels[1].find("ECDSA   SHA256:19j6m") != std::string::npos);
        CHECK(UserKeyListHeader(80).find("Fingerprint") != std::string::npos);

        // The same key again doesn't add one; a bad one is explained.
        f.state.new_key_text = kOtherTestKey;
        AddKeyToShownUser(&f.state);
        CHECK_EQ(f.state.status_message, std::string("\"K4WES\" already has that key."));
        CHECK_EQ(f.state.user_keys.size(), std::size_t{2});
        f.state.new_key_text = "not a key";
        AddKeyToShownUser(&f.state);
        CHECK(!f.state.form_error.empty());

        // Removing one key leaves the user and their other key.
        StartRowPick(&f.state, RowPickAction::kRemoveUserKey);
        TypeRowPickDigit(&f.state, '2');
        FinishRowPick(&f.state);
        CHECK_EQ(f.state.row_delete_title, std::string("Remove SSH Key"));
        CHECK(f.state.row_delete_lines[0].find("(desktop)") != std::string::npos);
        ConfirmRowDelete(&f.state);
        REQUIRE(f.state.user_keys.size() == 1);
        CHECK_EQ(f.state.user_keys[0].public_key, std::string(kTestKey));
        CHECK(f.state.manage_users_labels[0].find(" 1 ") != std::string::npos);

        // Their last key: removing it removes them, and closes the window.
        StartRowPick(&f.state, RowPickAction::kRemoveUserKey);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        CHECK_EQ(f.state.row_delete_title, std::string("Remove SSH User"));
        ConfirmRowDelete(&f.state);
        CHECK(!f.state.show_user_keys_modal);
        CHECK(f.state.manage_users.empty());
        CHECK(f.state.manage_user_names.empty());
    }

    QL_TEST(AUsersAccessIsChosenWhenAddedAndSwitchedByNumber)
    {
        Fixture f;
        f.state.new_user_username = "KB4VEW";
        f.state.new_user_public_key = kTestKey;
        f.state.new_user_access_index = 1;
        AddUserFromForm(&f.state);
        CHECK_EQ(f.state.status_message, std::string("Added \"KB4VEW\" (view-only)."));
        CHECK(f.db()->IsUserViewOnly("KB4VEW"));
        CHECK_EQ(f.state.new_user_access_index, 0);  // The form resets to full access.
        REQUIRE(f.state.manage_users_labels.size() == 1);
        CHECK(f.state.manage_users_labels[0].find("View-Only") != std::string::npos);

        // Another key keeps the user's access, whatever the form says.
        f.state.new_user_username = "KB4VEW";
        f.state.new_user_public_key = kOtherTestKey;
        AddUserFromForm(&f.state);
        CHECK_EQ(f.state.status_message,
                 std::string("Added another key for \"KB4VEW\" (view-only)."));

        // Editing them switches every key of the username; the window
        // starts on their current access.
        REQUIRE(f.state.manage_users_labels.size() == 1);
        CHECK(f.state.manage_users_labels[0].find("View-Only") != std::string::npos);
        StartRowPick(&f.state, RowPickAction::kEditUser);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        REQUIRE(f.state.show_user_keys_modal);
        CHECK_EQ(f.state.edit_user_access_index, 1);
        f.state.edit_user_access_index = 0;
        SaveEditedUser(&f.state);
        CHECK(!f.state.show_user_keys_modal);
        CHECK(!f.db()->IsUserViewOnly("KB4VEW"));
        for (const User& key : f.db()->GetUserKeys("KB4VEW"))
        {
            CHECK(!key.view_only);
        }
        CHECK(f.state.status_message.find("a full user") != std::string::npos);
    }

    QL_TEST(AViewOnlyUserCanOnlyWatchAndChangeTheirSettings)
    {
        Fixture f;
        f.state.view_only_user = true;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        RefreshNets(&f.state);

        // No open session: nothing to view, and nothing is started.
        StartSelectedNet(&f.state);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK_EQ(f.state.form_error, std::string("No session of Skywarn is open to view."));

        // An open one: straight to watching it, no resume prompt.
        NetInstance session;
        session.net_id = net_id;
        session.instance_date = "2026-09-27";
        session.net_control_callsign = "K4AAA";
        session.id = f.db()->CreateNetInstance(session);
        StartSelectedNet(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK_EQ(f.state.page, kPageActiveNet);
        CHECK(f.state.viewing_only);
        CHECK_EQ(f.state.active_instance.id, session.id);

        // Logging, editing, closing and deleting are all refused, whatever
        // leads there.
        f.state.modal_station.callsign = "K4BBB";
        CHECK(!LogStationCheckIn(&f.state));
        CHECK(f.state.form_error.find("View-only users can't") == 0);
        CHECK(f.db()->GetCheckInsForNetInstance(session.id).empty());
        CloseActiveNet(&f.state);
        CHECK(f.db()->GetNetInstanceById(session.id)->status == NetInstanceStatus::kOpen);
        StopViewing(&f.state);

        StartRowPick(&f.state, RowPickAction::kEditNet);
        CHECK(f.state.row_pick_action == RowPickAction::kNone);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        CHECK(f.state.edit_net_id != net_id);
        f.state.edit_net_id = net_id;
        f.state.saved_station.callsign = "K4AAA";
        CHECK(!SaveNetStationForm(&f.state));
        CHECK(f.db()->GetSavedStationsForNet(net_id).empty());
        f.state.new_net_name = "Tailgate";
        StartAdHocNet(&f.state);
        CHECK_EQ(f.db()->GetAllNets().size(), std::size_t{1});
        ShowCreateNetPageHandler show_create(&f.state);
        show_create();
        CHECK_EQ(f.state.page, kPageNetList);
        ShowImportNetPageHandler show_import(&f.state);
        show_import();
        CHECK_EQ(f.state.page, kPageNetList);

        // Their own settings they can change.
        OpenSettingsForm(&f.state);
        f.state.settings_path = f.dir().File("viewer.txt");
        f.state.settings_form.location = "37402";
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK_EQ(f.state.settings.location, std::string("37402"));
    }

    QL_TEST(AViewOnlyUserSeesOnlyTheirKeys)
    {
        Fixture f;
        f.state.view_only_user = true;
        AddTestNet(f.db(), "Skywarn");
        RefreshNets(&f.state);
        // New, Edit and Import do nothing.
        NetListKeyHandler keys(&f.state);
        CHECK(keys(ftxui::Event::F2));
        CHECK(keys(ftxui::Event::F7));
        CHECK(keys(ftxui::Event::F9));
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK(f.state.row_pick_action == RowPickAction::kNone);
        // Help lists only what they can do.
        OpenHelp(&f.state);
        bool lists_new = false;
        for (const std::vector<std::string>& row : f.state.info_cells)
        {
            lists_new = lists_new || row[0] == "F2";
        }
        CHECK(!lists_new);
    }

    QL_TEST(UsernamesAreCallSigns)
    {
        Fixture f;
        f.state.new_user_public_key = kTestKey;
        f.state.new_user_username = "tester";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.find("TESTER isn't a valid US or Canadian call sign") !=
              std::string::npos);
        f.state.new_user_username = "k4wes/m";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.find("without /M") != std::string::npos);
        CHECK(f.db()->ListUsers().empty());

        // Canadian call signs too, as everywhere else.
        f.state.new_user_username = "VE3ABC";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.empty());
        CHECK_EQ(f.db()->GetUserKeys("VE3ABC").size(), std::size_t{1});

        // Typed in lowercase, kept in capitals, and found either way.
        f.state.new_user_username = " k4wes ";
        f.state.new_user_public_key = kOtherTestKey;
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.empty());
        REQUIRE(f.db()->GetUserKeys("k4wes").size() == 1);
        CHECK_EQ(f.db()->GetUserKeys("k4wes")[0].username, std::string("K4WES"));
    }

    QL_TEST(RenamingAUserTakesTheirSettingsAndFilesAlong)
    {
        Fixture f;
        for (const char* username : {"K4WES", "KB4VEW"})
        {
            User user;
            user.username = username;
            user.public_key = kTestKey;
            f.db()->CreateUser(user);
        }
        EnsureDirectory(f.dir().File("settings"));
        WriteTextFile(SshUserSettingsPath(f.state.db_path, "K4WES"), "location=37402\n");
        EnsureDirectory(SessionExportsDir(f.state.db_path, "K4WES"));
        WriteTextFile(SessionExportsDir(f.state.db_path, "K4WES") + "/log.txt", "x");
        RefreshUsers(&f.state);
        OpenUserKeys(&f.state, 0);
        REQUIRE(f.state.user_keys_username == "K4WES");
        CHECK_EQ(f.state.rename_username, std::string("K4WES"));

        // Not a call sign, or someone else's: refused, and nothing saved.
        f.state.edit_user_access_index = 1;
        f.state.rename_username = "wes";
        SaveEditedUser(&f.state);
        CHECK(f.state.form_error.find("isn't a valid") != std::string::npos);
        f.state.rename_username = "kb4vew";
        SaveEditedUser(&f.state);
        CHECK_EQ(f.state.form_error, std::string("KB4VEW is already a user."));
        CHECK(f.state.show_user_keys_modal);
        CHECK_EQ(f.db()->GetUserKeys("K4WES").size(), std::size_t{1});
        CHECK(!f.db()->IsUserViewOnly("K4WES"));

        // Renamed and made view-only in one save.
        f.state.rename_username = "w4new";
        SaveEditedUser(&f.state);
        CHECK(f.state.form_error.empty());
        CHECK_EQ(f.state.status_message,
                 std::string("Renamed K4WES to W4NEW, now view-only, from their next login."));
        CHECK(!f.state.show_user_keys_modal);
        CHECK(f.db()->GetUserKeys("K4WES").empty());
        REQUIRE(f.db()->GetUserKeys("W4NEW").size() == 1);
        CHECK_EQ(f.db()->GetUserKeys("W4NEW")[0].username, std::string("W4NEW"));
        CHECK(f.db()->IsUserViewOnly("W4NEW"));
        CHECK_EQ(f.state.manage_user_names[static_cast<std::size_t>(f.state.selected_user_index)],
                 std::string("W4NEW"));
        // Their settings and files moved with them.
        CHECK(!std::filesystem::exists(SshUserSettingsPath(f.state.db_path, "K4WES")));
        CHECK_EQ(ReadTextFile(SshUserSettingsPath(f.state.db_path, "W4NEW")),
                 std::string("location=37402\n"));
        CHECK_EQ(ListFilesWithExtension(SessionExportsDir(f.state.db_path, "W4NEW"), ".txt").size(),
                 std::size_t{1});
        CHECK(!std::filesystem::exists(SessionExportsDir(f.state.db_path, "K4WES")));
    }

    QL_TEST(AnSshUsersCallsignIsTheirUsername)
    {
        Fixture f;
        f.state.ssh_username = "K4WES";
        f.state.callsign_editable = false;
        f.state.settings_path = f.dir().File("k4wes.txt");
        OpenSettingsForm(&f.state);
        f.state.settings_form.callsign = "W4KWK";  // However it got there.
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK_EQ(f.state.settings.callsign, std::string("K4WES"));
    }

    QL_TEST(RemovingAUserByNumber)
    {
        Fixture f;
        f.state.new_user_username = "K4WES";
        f.state.new_user_public_key = kTestKey;
        AddUserFromForm(&f.state);
        CHECK_EQ(f.state.manage_users.size(), std::size_t{1});
        AddUserFromForm(&f.state);  // Blank form now.
        CHECK(!f.state.form_error.empty());
        f.state.new_user_username = "K4WES";
        f.state.new_user_public_key = kOtherTestKey;
        AddUserFromForm(&f.state);
        // The user goes, with every key.
        StartRowPick(&f.state, RowPickAction::kRemoveUser);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        CHECK_EQ(f.state.row_delete_lines[0], std::string("Remove K4WES and all 2 of their keys?"));
        ConfirmRowDelete(&f.state);
        CHECK(f.state.manage_users.empty());
        CHECK(f.db()->GetUserKeys("K4WES").empty());
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

    // Starts an ad hoc net through the Ad Hoc Net page's form.
    static void StartAdHoc(Fixture* f, const std::string& name)
    {
        ResetCreateNetForm(&f->state);
        f->state.new_net_name = name;
        StartAdHocNet(&f->state);
    }

    QL_TEST(AdHocNetsStayOffTheNetList)
    {
        Fixture f;
        AddTestNet(f.db(), "Skywarn");
        StartAdHoc(&f, "Tailgate");
        CHECK_EQ(f.state.page, kPageSelectRole);
        CHECK_EQ(f.state.start_net.name, std::string("Tailgate"));
        CHECK(f.state.start_net.is_ad_hoc);
        REQUIRE(f.db()->GetNetById(f.state.start_net.id).has_value());
        CHECK(f.db()->GetNetById(f.state.start_net.id)->is_ad_hoc);

        RefreshNets(&f.state);
        REQUIRE(f.state.nets.size() == 1);
        CHECK_EQ(f.state.nets[0].name, std::string("Skywarn"));
    }

    QL_TEST(AnAdHocSessionLeftOpenCanBeResumedFromTheAdHocPage)
    {
        Fixture f;
        StartAdHoc(&f, "Tailgate");
        std::int64_t session =
            AddTestInstance(f.db(), f.state.start_net.id, "2026-09-24", 1000, "W4KWK");
        AddTestCheckIn(f.db(), session, "K4AAA", 1);
        f.state.page = kPageAdHocNet;
        RefreshOpenAdHocSessions(&f.state);
        REQUIRE(f.state.open_ad_hoc_labels.size() == 1);
        CHECK(f.state.open_ad_hoc_labels[0].find("Tailgate  started 2026-09-24") == 0);
        CHECK(f.state.open_ad_hoc_labels[0].find("1 check-in") != std::string::npos);

        f.state.start_net = Net();
        StartRowPick(&f.state, RowPickAction::kResumeAdHocSession);
        TypeRowPickDigit(&f.state, '1');
        FinishRowPick(&f.state);
        REQUIRE(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kResumeNet);
        CHECK(f.state.confirm_prompt_lines[0].find("Tailgate has a session") == 0);

        ResumeOpenNet(&f.state);
        CHECK_EQ(f.state.page, kPageActiveNet);
        CHECK_EQ(f.state.active_net_name, std::string("Tailgate"));
        CHECK(f.state.active_net_is_ad_hoc);
        RequestCloseActiveNet(&f.state);
        CHECK(f.state.confirm_prompt_lines[1].find("F6 on the Ad Hoc Net page") !=
              std::string::npos);
        CloseActiveNet(&f.state);
        CHECK(f.state.status_message.find("F6 on the Ad Hoc Net page") != std::string::npos);
        RefreshOpenAdHocSessions(&f.state);
        CHECK(f.state.open_ad_hoc_sessions.empty());
    }

    QL_TEST(AdHocHistoryListsEveryAdHocSession)
    {
        Fixture f;
        std::int64_t recurring = AddTestNet(f.db(), "Skywarn");
        AddTestInstance(f.db(), recurring, "2026-09-20", 500, "W4KWK");
        StartAdHoc(&f, "Tailgate");
        AddTestInstance(f.db(), f.state.start_net.id, "2026-09-22", 900, "W4KWK");
        StartAdHoc(&f, "Field Day Practice");
        AddTestInstance(f.db(), f.state.start_net.id, "2026-09-23", 1000, "K4BBB");

        f.state.history_ad_hoc = true;
        RefreshNetHistory(&f.state);
        REQUIRE(f.state.history_instance_labels.size() == 2);
        std::string header = NetInstanceListHeader(80, true);
        std::string::size_type net_column = header.find("Net ") - 2;  // Menu gutter.
        CHECK(f.state.history_instance_labels[0].find("2026-09-23") == 0);
        CHECK(f.state.history_instance_labels[0].substr(net_column).find("Field Day Practice") ==
              0);
        CHECK(f.state.history_instance_labels[1].substr(net_column).find("Tailgate") == 0);
        CHECK_EQ(header.find("Net Control") - header.find("Net "), std::size_t{25});
        CHECK(header.size() <= 78);

        f.state.history_ad_hoc = false;
        RefreshNets(&f.state);
        RefreshNetHistory(&f.state);
        CHECK_EQ(f.state.history_instances.size(), std::size_t{1});
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

    QL_TEST(TheNearbyRadiusSettingDecidesWhichLicensesAreSuggested)
    {
        Fixture f;
        LoadZipData(f.db());
        f.state.settings_path = f.dir().File("settings.txt");
        f.db()->BulkUpsertUlsStations(
            {MakeStation("K4AAA", "TRENTON", "30752"), MakeStation("K4BBB", "DOWNTOWN", "37402"),
             MakeStation("K4CCC", "NASHVILLE", "37201")},
            0, 3, 1);
        f.StartNet("Skywarn");
        f.state.modal_station.callsign = "K4";

        // The default 70 miles leaves out Nashville, about 110 miles away.
        RefreshCallsignSuggestions(&f.state);
        CHECK_EQ(f.state.modal_callsign_suggestions.size(), std::size_t{2});

        OpenSettingsForm(&f.state);
        CHECK_EQ(f.state.settings_radius_text, std::string("70"));
        f.state.settings_radius_text = "150";
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK_EQ(LoadSettings(f.state.settings_path).nearby_radius_miles, 150);
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 3);
        CHECK_EQ(f.state.modal_callsign_suggestions[2].callsign, std::string("K4CCC"));

        // 10 miles keeps only downtown.
        f.state.settings_radius_text = "10";
        REQUIRE(SaveSettingsForm(&f.state));
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("K4BBB"));
    }

    QL_TEST(ANearbyRadiusOutsideOneTo250IsRefusedAndBlankMeans70)
    {
        Fixture f;
        f.state.settings_path = f.dir().File("settings.txt");
        OpenSettingsForm(&f.state);
        f.state.settings_radius_text = "0";
        CHECK(!SaveSettingsForm(&f.state));
        CHECK(f.state.form_error.find("1 to 250") != std::string::npos);
        f.state.settings_radius_text = "251";
        CHECK(!SaveSettingsForm(&f.state));
        f.state.settings_radius_text = "250";
        CHECK(SaveSettingsForm(&f.state));
        CHECK_EQ(f.state.settings.nearby_radius_miles, 250);

        // Clearing the field goes back to the default its placeholder shows.
        f.state.settings_radius_text = "";
        CHECK(SaveSettingsForm(&f.state));
        CHECK_EQ(f.state.settings.nearby_radius_miles, 70);
    }

    QL_TEST(LicenseSuggestionsSkipStationsRemovedSinceTheyLoaded)
    {
        Fixture f;
        LoadZipData(f.db());
        f.db()->BulkUpsertUlsStations(
            {MakeStation("K4AAA", "STAYS", "37415"), MakeStation("K4AAB", "EXPIRES", "37402")}, 0,
            2, 1);
        f.StartNet("Skywarn");
        f.state.modal_station.callsign = "K4AA";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 2);
        CHECK_EQ(f.state.modal_callsign_suggestions[1].name, std::string("EXPIRES"));

        // A station data refresh drops K4AAB; the in-memory list still has it.
        f.db()->DeleteUlsStationsNotIn({MakeStation("K4AAA")});
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("K4AAA"));
    }

    QL_TEST(ThePartlyTypedCallOfAMarkedMatchIsNotTaken)
    {
        Fixture f;
        LoadZipData(f.db());
        // KE4R is a real, nearby licensee; KE4RP is saved for this net, so
        // it's the top match for "KE4R" and marked -- the match is taken,
        // not the licensee whose call was partly typed.
        f.db()->BulkUpsertUlsStations({MakeStation("KE4R", "NEARBY, KAY", "37402")}, 0, 1, 1);
        std::int64_t net_id = f.StartNet("Skywarn");
        f.db()->SaveNetStation(net_id, MakeStation("KE4RP", "Pete"), "", 1);

        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "KE4R";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 2);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("KE4RP"));
        CHECK_EQ(f.state.modal_callsign_suggestions[1].callsign, std::string("KE4R"));
        // The exact call is listed, so it's the one marked: the screen shows
        // which will be taken.
        CHECK_EQ(f.state.selected_suggestion_index, 1);
        f.state.selected_suggestion_index = 0;
        ApplySelectedCallsignSuggestion(&f.state);
        CHECK_EQ(f.state.modal_station.callsign, std::string("KE4RP"));
        CHECK_EQ(f.state.modal_station.name, std::string("Pete"));
    }

    QL_TEST(AStationNotAmongTheMatchesIsStillFilledIn)
    {
        Fixture f;
        LoadZipData(f.db());
        // K4NAS is in Nashville: licensed, but beyond the 70-mile radius, so
        // never offered as a match. AK4NAS is nearby, and matches "K4NAS".
        f.db()->BulkUpsertUlsStations({MakeStation("K4NAS", "NASHVILLE, NAN", "37201"),
                                       MakeStation("K4NAT", "NASHVILLE, NAT", "37201"),
                                       MakeStation("AK4NAS", "NEARBY, AL", "37402")},
                                      0, 3, 1);
        f.StartNet("Skywarn");

        // The callsign typed in full is listed below the matches, with its
        // distance, but the match marked ">" is still the one Enter takes.
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "K4NAS";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 2);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("AK4NAS"));
        CHECK_EQ(f.state.modal_callsign_suggestions[1].callsign, std::string("K4NAS"));
        CHECK(f.state.modal_callsign_suggestion_sources[1].find("mi)") != std::string::npos);
        CHECK_EQ(f.state.selected_suggestion_index, 0);
        CallsignLookupHandler enter(&f.state);
        enter();
        CHECK_EQ(f.state.modal_station.callsign, std::string("AK4NAS"));
        CHECK(f.state.modal_callsign_suggestions.empty());

        // Moved down to it, it's the one taken.
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "K4NAS";
        RefreshCallsignSuggestions(&f.state);
        f.state.selected_suggestion_index = 1;
        enter();
        CHECK_EQ(f.state.modal_station.callsign, std::string("K4NAS"));
        CHECK_EQ(f.state.modal_station.name, std::string("NASHVILLE, NAN"));

        // Logged without picking anything or pressing Enter: the FCC details
        // are used all the same.
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "K4NAT";
        RefreshCallsignSuggestions(&f.state);
        CHECK(f.state.modal_callsign_suggestions.empty());
        REQUIRE(LogStationCheckIn(&f.state));
        std::optional<Station> logged = f.db()->FindStationByCallsign("K4NAT");
        REQUIRE(logged.has_value());
        CHECK_EQ(logged->name, std::string("NASHVILLE, NAT"));

        // A partial callsign isn't looked up, and what the operator typed
        // isn't overwritten; a portable indicator is looked past.
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "K4NA";
        CHECK(FillCheckInFromKnownStation(&f.state) == false);
        f.state.modal_station.callsign = "K4NAS/M";
        f.state.modal_station.city = "Mobile";
        CHECK(FillCheckInFromKnownStation(&f.state));
        CHECK_EQ(f.state.modal_station.name, std::string("NASHVILLE, NAN"));
        CHECK_EQ(f.state.modal_station.city, std::string("Mobile"));
        CHECK_EQ(f.state.modal_station.callsign, std::string("K4NAS/M"));
    }

    QL_TEST(ASavedStationNotAmongTheMatchesIsStillFilledIn)
    {
        Fixture f;
        LoadZipData(f.db());
        // As above: K4NAS and K4NAT are beyond the radius; AK4NAS is nearby.
        f.db()->BulkUpsertUlsStations({MakeStation("K4NAS", "NASHVILLE, NAN", "37201"),
                                       MakeStation("K4NAT", "NASHVILLE, NAT", "37201"),
                                       MakeStation("AK4NAS", "NEARBY, AL", "37402")},
                                      0, 3, 1);
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));

        // As in the New Check-In window: listed below, and taken only once
        // it's marked.
        f.state.saved_station.callsign = "K4NAS";
        RefreshSavedStationSuggestions(&f.state);
        REQUIRE(f.state.saved_station_suggestions.size() == 2);
        CHECK_EQ(f.state.saved_station_suggestions[0].callsign, std::string("AK4NAS"));
        CHECK_EQ(f.state.saved_station_suggestions[1].callsign, std::string("K4NAS"));
        ApplySelectedSavedStationSuggestion(&f.state);
        CHECK_EQ(f.state.saved_station.callsign, std::string("AK4NAS"));
        f.state.saved_station = Station();
        f.state.saved_station.callsign = "K4NAS";
        RefreshSavedStationSuggestions(&f.state);
        f.state.selected_saved_station_suggestion_index = 1;
        ApplySelectedSavedStationSuggestion(&f.state);
        CHECK_EQ(f.state.saved_station.callsign, std::string("K4NAS"));
        CHECK_EQ(f.state.saved_station.name, std::string("NASHVILLE, NAN"));
        CHECK(f.state.saved_station_suggestions.empty());

        // Saved without picking anything or pressing Enter: the FCC details
        // are used all the same.
        f.state.saved_station = Station();
        f.state.saved_station.callsign = "K4NAT";
        RefreshSavedStationSuggestions(&f.state);
        CHECK(f.state.saved_station_suggestions.empty());
        REQUIRE(SaveNetStationForm(&f.state));
        std::optional<Station> saved = f.db()->FindStationByCallsign("K4NAT");
        REQUIRE(saved.has_value());
        CHECK_EQ(saved->name, std::string("NASHVILLE, NAT"));

        // What the operator typed isn't overwritten.
        f.state.saved_station = Station();
        f.state.saved_station.callsign = "K4NAS";
        f.state.saved_station.name = "Nan";
        CHECK(FillSavedStationFromKnownStation(&f.state));
        CHECK_EQ(f.state.saved_station.name, std::string("Nan"));
    }

    QL_TEST(CanadianCallSignsComeFromIsedWithoutDistance)
    {
        Fixture f;
        LoadZipData(f.db());
        Station ann = MakeStation("VA3ABC", "Able, Ann");
        ann.city = "OTTAWA";
        ann.state = "ON";
        ann.zip = "K1A 0B1";
        ann.license_class = "Advanced";
        f.db()->ReplaceIsedStations(
            {ann, MakeStation("VE3XYZ", "Zed, Zoe"), MakeStation("VE3XZZ", "Zulu, Zak")}, 1);
        f.db()->BulkUpsertUlsStations({MakeStation("K4AAC", "NEARBY, NED", "37402")}, 0, 1, 1);
        f.StartNet("Skywarn");

        // A call sign that looks Canadian: ISED's matches, tagged, in order.
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "ve3x";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 2);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("VE3XYZ"));
        CHECK_EQ(f.state.modal_callsign_suggestion_sources[0], std::string("(ISED)"));
        CHECK(f.state.modal_callsign_suggestion_labels[1].find("(ISED)") != std::string::npos);

        // Anything else doesn't look in ISED's data at all.
        f.state.modal_station.callsign = "3X";
        RefreshCallsignSuggestions(&f.state);
        CHECK(f.state.modal_callsign_suggestions.empty());
        f.state.modal_station.callsign = "K4AA";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestion_sources[0].find("(ULS"), std::size_t{0});

        // A whole Canadian call sign is filled in, as a US one is.
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "VA3ABC/P";
        CHECK(FillCheckInFromKnownStation(&f.state));
        CHECK_EQ(f.state.modal_station.name, std::string("Able, Ann"));
        CHECK_EQ(f.state.modal_station.zip, std::string("K1A 0B1"));

        // The saved-station form gets the same.
        OpenEditNetForm(&f.state, *f.db()->GetNetById(f.state.active_instance.net_id));
        f.state.saved_station.callsign = "VA3";
        RefreshSavedStationSuggestions(&f.state);
        REQUIRE(f.state.saved_station_suggestions.size() == 1);
        CHECK_EQ(f.state.saved_station_suggestion_sources[0], std::string("(ISED)"));

        OpenStationCard(&f.state, "VA3ABC");
        bool shows_class = false;
        for (const std::string& line : f.state.info_summary)
        {
            shows_class = shows_class || line.find("Advanced") != std::string::npos;
        }
        CHECK(shows_class);
    }

    QL_TEST(PartialMatchingIsSetPerNetToUsOrCanadianData)
    {
        Fixture f;
        LoadZipData(f.db());
        f.db()->ReplaceIsedStations({MakeStation("VE3EVA", "Able, Eva")}, 1);
        f.db()->BulkUpsertUlsStations({MakeStation("KQ4EVW", "NEAR, NED", "37402"),
                                       MakeStation("EV4AA", "EARLY, EVE", "37402")},
                                      0, 2, 1);
        f.StartNet("Skywarn");

        // A US net, as every net is unless changed: US call signs match
        // anywhere, Canadian ones only from the start.
        CHECK(!f.state.active_net_partial_match_canada);
        f.state.modal_station.callsign = "ev";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 2);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("EV4AA"));
        CHECK_EQ(f.state.modal_callsign_suggestions[1].callsign, std::string("KQ4EVW"));
        f.state.modal_station.callsign = "3e";
        RefreshCallsignSuggestions(&f.state);
        CHECK(f.state.modal_callsign_suggestions.empty());
        f.state.modal_station.callsign = "ve3";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("VE3EVA"));

        // A Canadian net: the other way round.
        f.state.active_net_partial_match_canada = true;
        f.state.modal_station.callsign = "ev";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 2);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("EV4AA"));
        CHECK_EQ(f.state.modal_callsign_suggestions[1].callsign, std::string("VE3EVA"));
        CHECK_EQ(f.state.modal_callsign_suggestion_sources[1], std::string("(ISED)"));
        f.state.modal_station.callsign = "3e";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("VE3EVA"));

        // Set on Edit Net, and used by its Saved Station window.
        OpenEditNetForm(&f.state, *f.db()->GetNetById(f.state.active_instance.net_id));
        CHECK_EQ(f.state.edit_net_partial_match_index, 0);
        f.state.saved_station.callsign = "3E";
        RefreshSavedStationSuggestions(&f.state);
        CHECK(f.state.saved_station_suggestions.empty());
        f.state.edit_net_partial_match_index = 1;
        RefreshSavedStationSuggestions(&f.state);
        REQUIRE(f.state.saved_station_suggestions.size() == 1);
        CHECK_EQ(f.state.saved_station_suggestions[0].callsign, std::string("VE3EVA"));
        CHECK(SaveEditNetForm(&f.state));
        std::optional<Net> saved = f.db()->GetNetById(f.state.edit_net_id);
        REQUIRE(saved.has_value());
        CHECK(saved->partial_match_canada);
    }

    QL_TEST(NewNetsSavePartialMatching)
    {
        Fixture f;
        // New recurring nets: US unless changed.
        ResetCreateNetForm(&f.state);
        CHECK_EQ(f.state.new_net_partial_match_index, 0);
        f.state.new_net_name = "Canadian Net";
        f.state.new_net_partial_match_index = 1;
        CreateNetSubmitHandler create(&f.state);
        create();
        REQUIRE(f.state.nets.size() == 1);
        CHECK(f.state.nets[0].partial_match_canada);
        // The form is back to US for the next net.
        CHECK_EQ(f.state.new_net_partial_match_index, 0);

        // Ad hoc nets too.
        f.state.operator_callsign = "W4KWK";
        f.state.new_net_name = "Tailgate";
        f.state.new_net_partial_match_index = 1;
        StartAdHocNet(&f.state);
        CHECK(f.state.start_net.partial_match_canada);
        std::optional<Net> ad_hoc = f.db()->GetNetById(f.state.start_net.id);
        REQUIRE(ad_hoc.has_value());
        CHECK(ad_hoc->partial_match_canada);
    }

    QL_TEST(AutocompleteShowsAsManyAsTheScreenHasRoomFor)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        for (int i = 0; i < 30; ++i)
        {
            f.db()->SaveNetStation(net_id, MakeStation("K4X" + std::to_string(10 + i)), "", 1);
        }
        f.state.modal_station.callsign = "K4X";
        // 24 rows: 11 fit under the window's other 13.
        RefreshCallsignSuggestions(&f.state);
        CHECK_EQ(f.state.modal_callsign_suggestions.size(), std::size_t{11});
        CHECK_EQ(f.state.modal_callsign_suggestion_labels.size(), std::size_t{11});
        f.state.screen_height = 40;
        RefreshCallsignSuggestions(&f.state);
        CHECK_EQ(f.state.modal_callsign_suggestions.size(), std::size_t{27});
        // Never fewer than 8, however short the screen.
        f.state.screen_height = 15;
        RefreshCallsignSuggestions(&f.state);
        CHECK_EQ(f.state.modal_callsign_suggestions.size(), std::size_t{8});
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
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        f.db()->SaveNetStation(
            net_id,
            MakeStation("K4LOG", "A Name That Is Far Too Long To Fit", "37415", "Chattanooga"), "",
            1);
        CheckIn check_in;
        check_in.net_instance_id = f.state.active_instance.id;
        check_in.callsign = "K4LOG";
        check_in.designated_role = kRoleLogger;
        check_in.remarks = "remarks here";
        check_in.signal_report = "59";
        check_in.comment = "a comment";
        check_in.checked_in_at = 1790003600;
        f.db()->AddCheckInAtNextSequence(check_in);
        std::vector<std::vector<std::string>> cells =
            CheckInCells(f.db(), f.db()->GetCheckInsForNetInstance(check_in.net_instance_id));
        REQUIRE(cells.size() == 2);

        for (int width : {80, 100, 120, 160, 250})
        {
            std::string header = CheckInListHeader(width).substr(2);  // Menu gutter.
            std::string row = FormatCheckInList(cells, width)[1];
            CHECK_EQ(header.find("Callsign"), row.find("K4LOG"));
            CHECK_EQ(header.find("Role"), row.find("Log "));
            CHECK_EQ(header.find("Remarks"), row.find("remarks here"));
            CHECK(row.find("Too Long To Fit") == std::string::npos);  // Truncated.
            if (header.find("Time") != std::string::npos)
            {
                CHECK_EQ(header.find("Time"), row.find(FormatLocalTimeOfDay(1790003600)));
            }
            if (header.find("Signal") != std::string::npos)
            {
                CHECK_EQ(header.find("Signal"), row.find("59 "));
            }
            // A row never outgrows the list, except a long last column.
            CHECK(static_cast<int>(header.size()) <= width - 4);
        }
        // More shows as the terminal widens.
        CHECK(CheckInListHeader(80).find("Time") == std::string::npos);
        CHECK(CheckInListHeader(120).find("Time") != std::string::npos);
        CHECK(CheckInListHeader(120).find("City, State") != std::string::npos);
        CHECK(CheckInListHeader(160).find("Comment") != std::string::npos);
        CHECK(FormatCheckInList(cells, 160)[1].find("a comment") != std::string::npos);
    }

    // At 80 columns every list is character for character what it was before
    // lists followed the terminal's width.
    QL_TEST(ListsAt80ColumnsAreUnchanged)
    {
        Fixture f;
        LoadZipData(f.db());
        std::int64_t net_id = f.StartNet("Skywarn");
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA", "ANN AMATEUR", "37415", "City"), "", 1);
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "K4AAA";
        f.state.modal_remarks = "on time";
        REQUIRE(LogStationCheckIn(&f.state));
        char expected[256];

        // Check-ins: "#, Callsign, Name, Member ID, County, Role, Remarks".
        REQUIRE(f.state.active_check_in_cells.size() == 2);
        const std::vector<std::string>& cells = f.state.active_check_in_cells[1];
        std::snprintf(expected, sizeof(expected),
                      "%-3d %-10.10s %-20.20s %-10.10s %-14.14s %-6.6s %s", 2, cells[2].c_str(),
                      cells[3].c_str(), cells[4].c_str(), cells[6].c_str(), cells[7].c_str(),
                      cells[9].c_str());
        CHECK_EQ(f.state.active_display_rows[1], std::string(expected));
        CHECK_EQ(cells[9], std::string("on time"));
        std::snprintf(expected, sizeof(expected),
                      "  %-3.3s %-10.10s %-20.20s %-10.10s %-14.14s %-6.6s %s", "#", "Callsign",
                      "Name", "Member ID", "County", "Role", "Remarks");
        CHECK_EQ(CheckInListHeader(80), std::string(expected));

        // History sessions.
        std::snprintf(expected, sizeof(expected),
                      "  %-10.10s %-8.8s %-8.8s %-12.12s %-12.12s %-12.12s %s", "Date", "Start",
                      "End", "Net Control", "Alternate NC", "Logger", "Status");
        CHECK_EQ(NetInstanceListHeader(80, false), std::string(expected));
        std::snprintf(expected, sizeof(expected), "  %-10.10s %-8.8s %-8.8s %-24.24s %-12.12s %s",
                      "Date", "Start", "End", "Net", "Net Control", "Status");
        CHECK_EQ(NetInstanceListHeader(80, true), std::string(expected));

        // Saved stations and autocomplete matches.
        std::snprintf(expected, sizeof(expected), "  %-10.10s %-20.20s %s", "Callsign", "Name",
                      "Member ID");
        CHECK_EQ(SavedStationListHeader(80), std::string(expected));
        std::snprintf(expected, sizeof(expected), "  %-10.10s %-20.20s %s", "Callsign", "Name",
                      "Source");
        CHECK_EQ(MatchListHeader(80), std::string(expected));
        f.state.modal_station.callsign = "K4AA";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(!f.state.modal_callsign_suggestion_labels.empty());
        std::snprintf(expected, sizeof(expected), "%-10.10s %-20.20s %s", "K4AAA", "ANN AMATEUR",
                      "(this net)");
        CHECK_EQ(f.state.modal_callsign_suggestion_labels[0], std::string(expected));

        // The net list: the name padded to the longest, then when it was made.
        RefreshNets(&f.state);
        REQUIRE(f.state.net_names.size() == 1);
        CHECK_EQ(f.state.net_names[0], std::string("Skywarn  session open"));
    }

    QL_TEST(AWiderTerminalShowsMoreOfEveryList)
    {
        Fixture f;
        Net net;
        net.name = "Skywarn";
        net.mode = "FM";
        net.default_frequency = "146.940";
        net.recurrence_description = "Tuesdays 8pm";
        std::int64_t net_id = f.db()->CreateNet(net);
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA", "Ann", "37415", "Chattanooga"),
                               "mobile", 1);
        RefreshNets(&f.state);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        CHECK(f.state.net_names[0].find("146.940") == std::string::npos);
        CHECK(f.state.edit_net_saved_station_labels[0].find("mobile") == std::string::npos);

        UpdateListWidths(&f.state, 130);
        CHECK_EQ(f.state.list_width, 130);
        CHECK(f.state.net_names[0].find("FM") != std::string::npos);
        CHECK(f.state.net_names[0].find("146.940") != std::string::npos);
        CHECK(f.state.net_names[0].find("Tuesdays 8pm") != std::string::npos);
        CHECK(f.state.edit_net_saved_station_labels[0].find("mobile") != std::string::npos);
        CHECK(SavedStationListHeader(130).find("Default Remarks") != std::string::npos);

        // Narrower than 80 lays out as at 80.
        UpdateListWidths(&f.state, 60);
        CHECK_EQ(f.state.list_width, 80);
        CHECK(f.state.net_names[0].find("146.940") == std::string::npos);
    }

    QL_TEST(ExportedLogsHaveEveryColumnWhateverTheTerminal)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.state.modal_signal_report = "59";
        f.state.modal_remarks = "a remark longer than thirty characters, kept whole";
        f.state.modal_comment = "comment";
        f.state.modal_station.callsign = "K4AAA";
        REQUIRE(LogStationCheckIn(&f.state));
        NetInstance instance = *f.db()->GetNetInstanceById(f.state.active_instance.id);
        std::vector<CheckIn> check_ins = f.db()->GetCheckInsForNetInstance(instance.id);
        ExportNetLog(&f.state, "Skywarn", instance, check_ins);
        std::string first = ReadTextFile(f.dir().File("exports/Skywarn_2026-09-24_log.txt"));
        UpdateListWidths(&f.state, 200);
        ExportNetLog(&f.state, "Skywarn", instance, check_ins);
        std::string second = ReadTextFile(f.dir().File("exports/Skywarn_2026-09-24_log.txt"));
        CHECK_EQ(first, second);
        CHECK(first.find("Signal") != std::string::npos);
        CHECK(first.find("Comment") != std::string::npos);
        // Remarks get 40 characters in a file (more than the screen's 30).
        CHECK(first.find("a remark longer than thirty characters, ") != std::string::npos);
        CHECK(first.find("a remark longer than thirty characters, kept whole") ==
              std::string::npos);
        // Fixed columns, whatever the data: the header and each row put a
        // column at the same position.
        std::string::size_type header_start = first.find("#  ");
        REQUIRE(header_start != std::string::npos);
        std::string header =
            first.substr(header_start, first.find('\n', header_start) - header_start);
        CHECK_EQ(header.find("Callsign"), std::string::size_type{4 + 2 + 8 + 2});
        CHECK_EQ(header.find("Comment"),
                 std::string::size_type{4 + 2 + 8 + 2 + 13 + 2 + 30 + 2 + 10 + 2 + 30 + 2 + 20 + 2 +
                                        6 + 2 + 6 + 2 + 40 + 2});
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

    QL_TEST(ALocalExportOffersTheFolderNotZmodem)
    {
        Fixture f;
        f.StartNet("Skywarn");
        {
            LocalTerminalScope local;
            CHECK(IsLocalTerminal(true));
            CHECK(!IsLocalTerminal(false));
            ExportNetLog(&f.state, "Skywarn", f.state.active_instance, f.state.active_check_ins);
            CHECK(f.state.status_message.find("Saved to") == 0);
            // Show Folder where there's a desktop to show it on; never ZMODEM.
            CHECK_EQ(f.state.show_zmodem_confirm_modal, CanShowInFileManager());
            if (f.state.show_zmodem_confirm_modal)
            {
                CHECK(f.state.zmodem_action == ZmodemAction::kShowFolder);
                CHECK_EQ(f.state.zmodem_send_paths.size(), std::size_t{2});
                CancelZmodemAction(&f.state);
                CHECK(!f.state.show_zmodem_confirm_modal);
                CHECK(f.state.status_message.find("Saved to") == 0);
            }

            // Nor is there anyone to receive a file from.
            StartZmodemReceive(&f.state);
            CHECK(!f.state.show_zmodem_confirm_modal);
        }
        // Inside an ordinary SSH login, it's a remote terminal as before.
        // (Where there's no ZMODEM at all, as on Windows, nothing is offered.)
        CHECK(!IsLocalTerminal(true));
        f.state.zmodem_action = ZmodemAction::kReceive;
        ExportNetLog(&f.state, "Skywarn", f.state.active_instance, f.state.active_check_ins);
        CHECK(f.state.zmodem_action != ZmodemAction::kShowFolder);
        f.state.show_zmodem_confirm_modal = false;
    }

    QL_TEST(AnSshUsersFilesAreTheirOwnAndCleanedUpAfterAWeek)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.state.ssh_username = "wes";
        ExportNetLog(&f.state, "Skywarn", f.state.active_instance, f.state.active_check_ins);
        CHECK(f.state.form_error.empty());
        std::string mine = f.dir().File("exports/ssh-users/wes");
        CHECK_EQ(ListFilesWithExtension(mine, ".txt").size(), std::size_t{1});
        CHECK_EQ(ListFilesWithExtension(mine, ".qlsession").size(), std::size_t{1});
        CHECK(ListFilesWithExtension(f.dir().File("exports"), ".txt").empty());

        // Only their own received files are offered for import.
        EnsureDirectory(ImportsDir(f.state.db_path));
        EnsureDirectory(SessionImportsDir(f.state.db_path, "wes"));
        EnsureDirectory(SessionImportsDir(f.state.db_path, "ann"));
        WriteTextFile(ImportsDir(f.state.db_path) + "/console.qlnet", "x");
        WriteTextFile(SessionImportsDir(f.state.db_path, "wes") + "/mine.qlnet", "x");
        WriteTextFile(SessionImportsDir(f.state.db_path, "ann") + "/hers.qlnet", "x");
        RefreshImportNetFiles(&f.state);
        REQUIRE(f.state.import_net_files.size() == 1);
        CHECK_EQ(f.state.import_net_files[0], std::string("mine.qlnet"));

        // A week on, the SSH users' files go; the console's stay.
        f.state.ssh_username.clear();
        ExportNetLog(&f.state, "Skywarn", f.state.active_instance, f.state.active_check_ins);
        CHECK_EQ(RemoveOldSshUserFiles(f.state.db_path, kSshUserFileMaxAgeSeconds), 0);
        // Wes's log and .qlsession, and both users' received files.
        CHECK_EQ(RemoveOldSshUserFiles(f.state.db_path, -60), 4);
        CHECK(!std::filesystem::exists(mine));
        CHECK_EQ(ListFilesWithExtension(f.dir().File("exports"), ".txt").size(), std::size_t{1});
        CHECK_EQ(ListFilesWithExtension(f.dir().File("imports"), ".qlnet").size(), std::size_t{1});
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

    QL_TEST(NoTwoRecurringNetsShareAName)
    {
        Fixture f;
        AddTestNet(f.db(), "TAG Skywarn");
        std::int64_t ares = AddTestNet(f.db(), "Hamilton County ARES");
        RefreshNets(&f.state);

        // A new net: refused, whatever the capitals and spaces.
        ResetCreateNetForm(&f.state);
        f.state.new_net_name = " tag  SKYWARN ";
        CreateNetSubmitHandler create(&f.state);
        create();
        CHECK(f.state.form_error.find("\"TAG Skywarn\"") != std::string::npos);
        CHECK_EQ(f.db()->GetAllNets().size(), std::size_t{2});

        // Renaming another net to it: refused.
        OpenEditNetForm(&f.state, *f.db()->GetNetById(ares));
        f.state.edit_net_name = "TAG Skywarn";
        CHECK(!SaveEditNetForm(&f.state));
        CHECK_EQ(f.db()->GetNetById(ares)->name, std::string("Hamilton County ARES"));

        // Ad hoc nets don't count, and a net that already shared its name
        // (from before the rule) can still be saved unrenamed.
        Net ad_hoc;
        ad_hoc.name = "Tailgate Net";
        ad_hoc.is_ad_hoc = true;
        f.db()->CreateNet(ad_hoc);
        f.db()->CreateNet(ad_hoc);
        std::int64_t older_twin = AddTestNet(f.db(), "Hamilton County ARES");
        RefreshNets(&f.state);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(older_twin));
        f.state.edit_net_comments = "Backup on 147.000";
        CHECK(SaveEditNetForm(&f.state));
    }

    QL_TEST(ImportingANetFileNamedLikeOneHereIsRefused)
    {
        Fixture f;
        {
            Database other(f.dir().File("other.db"));
            std::int64_t net_id = AddTestNet(&other, "TAG SKYWARN");
            std::string error;
            REQUIRE(WriteNetSliceFile(ImportsDir(f.state.db_path) + "/tag.qlnet",
                                      GatherNetSlice(&other, net_id), &error));
        }
        AddTestNet(f.db(), "TAG Skywarn");
        RefreshNets(&f.state);
        RefreshImportNetFiles(&f.state);
        REQUIRE(f.state.import_net_files.size() == 1);

        // Not asked about: refused outright, nothing imported.
        ImportSelectedNetSlice(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK(f.state.form_error.find("\"TAG Skywarn\"") != std::string::npos);
        CHECK_EQ(f.db()->GetAllNets().size(), std::size_t{1});
    }

    QL_TEST(ImportingANetFileLikeANetAlreadyHereAsksFirst)
    {
        Fixture f;
        {
            Database other(f.dir().File("other.db"));
            std::int64_t net_id = AddTestNet(&other, "Hamilton County ARES");
            std::string error;
            REQUIRE(WriteNetSliceFile(ImportsDir(f.state.db_path) + "/ares.qlnet",
                                      GatherNetSlice(&other, net_id), &error));
        }
        AddTestNet(f.db(), "Hamilton Co. ARES Net");
        AddTestNet(f.db(), "TAG Skywarn");
        RefreshNets(&f.state);
        RefreshImportNetFiles(&f.state);
        REQUIRE(f.state.import_net_files.size() == 1);

        // Asked first, naming only the net that looks like it; Esc imports
        // nothing.
        ImportSelectedNetSlice(&f.state);
        CHECK(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kImportLookAlikeNet);
        REQUIRE(!f.state.confirm_prompt_lines.empty());
        CHECK(f.state.confirm_prompt_lines[0].find("\"Hamilton Co. ARES Net\"") !=
              std::string::npos);
        CHECK(f.state.confirm_prompt_lines[0].find("TAG") == std::string::npos);
        CancelConfirmPrompt(&f.state);
        CHECK_EQ(f.state.nets.size(), std::size_t{2});

        // Imported as a new net anyway.
        ImportSelectedNetSlice(&f.state);
        REQUIRE(f.state.show_confirm_prompt);
        ImportSelectedNetSliceAnyway(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK_EQ(f.state.nets.size(), std::size_t{3});
        CHECK_EQ(f.state.page, kPageNetList);
    }

    QL_TEST(ExportingASessionAlsoWritesItsSessionFile)
    {
        Fixture f;
        f.StartNet("Skywarn");
        Station ann = MakeStation("K4AAA", "Ann Able", "37415", "Chattanooga");
        ann.street_address = "1 Main St";
        ann.grid_square = "EM75";
        f.state.modal_station = ann;
        f.state.modal_remarks = "mobile";
        REQUIRE(LogStationCheckIn(&f.state));
        ExportNetLog(&f.state, "Skywarn", f.state.active_instance, f.state.active_check_ins);
        CHECK(f.state.form_error.empty());
        std::vector<std::string> files =
            ListFilesWithExtension(f.dir().File("exports"), ".qlsession");
        REQUIRE(files.size() == 1);
        CHECK_EQ(files[0], std::string("Skywarn_2026-09-24.qlsession"));

        // It holds the session exactly, with its stations' details.
        std::string error;
        std::optional<NetSlice> slice =
            ReadSessionSliceFile(f.dir().File("exports/" + files[0]), &error);
        REQUIRE(slice.has_value());
        CHECK_EQ(slice->net.name, std::string("Skywarn"));
        REQUIRE(slice->instances.size() == 1);
        CHECK_EQ(slice->instances[0].started_at, std::int64_t{1000});
        CHECK_EQ(slice->check_ins.size(), std::size_t{2});
        bool has_ann = false;
        for (const Station& station : slice->other_stations)
        {
            has_ann =
                has_ann || (station.callsign == "K4AAA" && station.street_address == "1 Main St" &&
                            station.grid_square == "EM75");
        }
        CHECK(has_ann);
    }

    // A session logged on another QuickLogger, exported to a .qlsession file
    // in this one's imports/.
    static void WriteSessionFileFromElsewhere(const Fixture& f, const std::string& net_name,
                                              const std::string& file_name)
    {
        Database other(f.dir().File("elsewhere-" + file_name + ".db"));
        std::int64_t net_id = AddTestNet(&other, net_name);
        std::int64_t session = AddTestInstance(&other, net_id, "2026-09-20", 5000, "N0XYZ");
        AddTestCheckIn(&other, session, "N0XYZ", 1);
        Station bob = MakeStation("K4BBB", "Bob Baker", "37402", "Chattanooga");
        other.RecordManualCheckInStation(bob, 1);
        CheckIn check_in;
        check_in.net_instance_id = session;
        check_in.callsign = "K4BBB";
        check_in.sequence_number = 2;
        check_in.remarks = "portable";
        check_in.checked_in_at = 5100;
        other.AddCheckIn(check_in);
        std::string error;
        EnsureDirectory(ImportsDir(f.state.db_path));
        REQUIRE(WriteNetSliceFile(ImportsDir(f.state.db_path) + "/" + file_name,
                                  GatherSessionSlice(&other, session), &error));
    }

    QL_TEST(ImportingASessionAddsItToTheNetBeingViewed)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        WriteSessionFileFromElsewhere(f, "Skywarn (laptop)", "offline.qlsession");
        // A whole net's export isn't offered, and isn't a session.
        EnsureDirectory(ImportsDir(f.state.db_path));
        WriteTextFile(ImportsDir(f.state.db_path) + "/whole.qlnet", "x");

        RefreshNets(&f.state);
        f.state.selected_net_index = 0;
        f.state.history_ad_hoc = false;
        OpenSessionImport(&f.state);
        CHECK_EQ(f.state.page, kPageImportNet);
        REQUIRE(f.state.import_net_files.size() == 1);
        CHECK_EQ(f.state.import_net_files[0], std::string("offline.qlsession"));

        ImportSelectedSession(&f.state);
        CHECK(f.state.form_error.empty());
        CHECK_EQ(f.state.page, kPageNetHistory);
        CHECK(f.state.status_message.find("2026-09-20") != std::string::npos);
        CHECK(f.state.status_message.find("logged as \"Skywarn (laptop)\"") != std::string::npos);
        std::vector<NetInstance> sessions = f.db()->GetNetInstancesForNet(net_id);
        REQUIRE(sessions.size() == 2);
        // It's the one highlighted, with its check-ins.
        const NetInstance& imported =
            f.state.history_instances[static_cast<std::size_t>(f.state.selected_history_index)];
        CHECK_EQ(imported.instance_date, std::string("2026-09-20"));
        CHECK_EQ(imported.started_at, std::int64_t{5000});
        REQUIRE(f.state.history_check_ins.size() == 2);
        CHECK_EQ(f.state.history_check_ins[1].remarks, std::string("portable"));
        // Its stations are saved to the net, with their details.
        std::optional<Station> bob = f.db()->FindStationByCallsign("K4BBB");
        REQUIRE(bob.has_value());
        CHECK_EQ(bob->name, std::string("Bob Baker"));
        CHECK_EQ(f.db()->GetSavedNetStationRemarks(net_id, "K4BBB"), std::string("portable"));

        // The same session again is refused.
        OpenSessionImport(&f.state);
        ImportSelectedSession(&f.state);
        CHECK(f.state.form_error.find("already has that session") != std::string::npos);
        CHECK_EQ(f.state.page, kPageImportNet);
        CHECK_EQ(f.db()->GetNetInstancesForNet(net_id).size(), std::size_t{2});
        LeaveImportPage(&f.state);
        CHECK_EQ(f.state.page, kPageNetHistory);
        CHECK(!f.state.import_session);
    }

    QL_TEST(ImportingASessionFromANetWithAnUnlikeNameAsksFirst)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("TAG Skywarn");
        WriteSessionFileFromElsewhere(f, "Hamilton County ARES", "ares.qlsession");
        RefreshNets(&f.state);
        f.state.selected_net_index = 0;
        f.state.history_ad_hoc = false;
        OpenSessionImport(&f.state);

        // Asked first; nothing imported yet, and Esc imports nothing.
        ImportSelectedSession(&f.state);
        CHECK(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kImportOtherNet);
        REQUIRE(!f.state.confirm_prompt_lines.empty());
        CHECK(f.state.confirm_prompt_lines[0].find("\"Hamilton County ARES\"") !=
              std::string::npos);
        CHECK_EQ(f.state.page, kPageImportNet);
        CHECK_EQ(f.db()->GetNetInstancesForNet(net_id).size(), std::size_t{1});
        CancelConfirmPrompt(&f.state);
        CHECK_EQ(f.db()->GetNetInstancesForNet(net_id).size(), std::size_t{1});

        // Asked again; this time imported anyway.
        ImportSelectedSession(&f.state);
        REQUIRE(f.state.show_confirm_prompt);
        ImportSelectedSessionAnyway(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK_EQ(f.state.page, kPageNetHistory);
        CHECK_EQ(f.db()->GetNetInstancesForNet(net_id).size(), std::size_t{2});
        CHECK(f.state.status_message.find("logged as \"Hamilton County ARES\"") !=
              std::string::npos);
    }

    QL_TEST(ImportingASessionFromAdHocHistoryMakesANewAdHocNet)
    {
        Fixture f;
        WriteSessionFileFromElsewhere(f, "Tailgate", "tailgate.qlsession");
        f.state.history_ad_hoc = true;
        OpenSessionImport(&f.state);
        ImportSelectedSession(&f.state);
        CHECK(f.state.form_error.empty());
        std::vector<NetInstance> ad_hoc = f.db()->GetAdHocNetInstances();
        REQUIRE(ad_hoc.size() == 1);
        std::optional<Net> net = f.db()->GetNetById(ad_hoc[0].net_id);
        REQUIRE(net.has_value());
        CHECK_EQ(net->name, std::string("Tailgate"));
        CHECK(net->is_ad_hoc);
        CHECK(net->imported_at > 0);

        // Again: refused, not a second ad hoc net.
        OpenSessionImport(&f.state);
        ImportSelectedSession(&f.state);
        CHECK(f.state.form_error.find("already has that session") != std::string::npos);
        CHECK_EQ(f.db()->GetAdHocNetInstances().size(), std::size_t{1});
    }

    QL_TEST(AViewOnlyUserCanExportButNeverImport)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        f.db()->CloseNetInstance(f.state.active_instance.id, 2000);
        WriteSessionFileFromElsewhere(f, "Skywarn", "offline.qlsession");
        {
            Database other(f.dir().File("other.db"));
            std::int64_t other_net = AddTestNet(&other, "Their Net");
            std::string error;
            REQUIRE(WriteNetSliceFile(ImportsDir(f.state.db_path) + "/their.qlnet",
                                      GatherNetSlice(&other, other_net), &error));
        }
        f.state.view_only_user = true;
        RefreshNets(&f.state);
        f.state.selected_net_index = 0;
        f.state.history_ad_hoc = false;
        RefreshNetHistory(&f.state);
        f.state.page = kPageNetHistory;

        // Exporting works: the log and its .qlsession.
        ExportNetHistoryLogHandler export_log(&f.state);
        export_log();
        CHECK(f.state.form_error.empty());
        CHECK_EQ(ListFilesWithExtension(f.dir().File("exports"), ".txt").size(), std::size_t{1});
        CHECK_EQ(ListFilesWithExtension(f.dir().File("exports"), ".qlsession").size(),
                 std::size_t{1});
        f.state.show_zmodem_confirm_modal = false;
        // And a whole net (F8 on Recurring Nets).
        f.state.page = kPageNetList;
        NetListKeyHandler net_list_keys(&f.state);
        CHECK(net_list_keys(ftxui::Event::F8));
        CHECK(f.state.form_error.empty());
        CHECK_EQ(ListFilesWithExtension(f.dir().File("exports"), ".qlnet").size(), std::size_t{1});
        f.state.show_zmodem_confirm_modal = false;
        f.state.page = kPageNetHistory;

        // No way into importing a session: F6 in History, or directly.
        NetHistoryKeyHandler keys(&f.state);
        CHECK(keys(ftxui::Event::F6));
        CHECK_EQ(f.state.page, kPageNetHistory);
        OpenSessionImport(&f.state);
        CHECK_EQ(f.state.page, kPageNetHistory);
        CHECK(f.state.form_error.find("View-only users can't") == 0);

        // Nor any import that gets there anyway: a session, a net, or a
        // ZMODEM receive.
        f.state.import_session = true;
        f.state.import_session_net_id = net_id;
        f.state.page = kPageImportNet;
        RefreshImportNetFiles(&f.state);
        REQUIRE(f.state.import_net_files.size() == 1);
        ImportSelectedNetSliceHandler import_selected(&f.state);
        import_selected();
        CHECK_EQ(f.db()->GetNetInstancesForNet(net_id).size(), std::size_t{1});
        f.state.import_session = false;
        RefreshImportNetFiles(&f.state);
        REQUIRE(f.state.import_net_files.size() == 1);
        import_selected();
        CHECK_EQ(f.db()->GetAllNets().size(), std::size_t{1});
        StartZmodemReceive(&f.state);
        CHECK(!f.state.show_zmodem_confirm_modal);
        f.state.zmodem_action = ZmodemAction::kReceive;
        ConfirmZmodemAction(&f.state);
        CHECK(f.state.form_error.find("View-only users can't import") == 0);
    }

    QL_TEST(AWholeNetFileIsNotASession)
    {
        Fixture f;
        {
            Database other(f.dir().File("other.db"));
            std::int64_t net_id = AddTestNet(&other, "Their Net");
            AddTestInstance(&other, net_id, "2026-01-01", 1, "N0XYZ");
            AddTestInstance(&other, net_id, "2026-01-08", 2, "N0XYZ");
            std::string error;
            REQUIRE(WriteNetSliceFile(f.dir().File("their.qlsession"),
                                      GatherNetSlice(&other, net_id), &error));
        }
        std::string error;
        CHECK(!ReadSessionSliceFile(f.dir().File("their.qlsession"), &error).has_value());
        CHECK(error.find("more than one session") != std::string::npos);
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
        f.state.page = kPageEditNet;
        f.state.edit_net_name.clear();
        CHECK(!SaveEditNetForm(&f.state));
        CHECK_EQ(f.db()->GetNetById(net_id)->name, std::string("Keep"));
        CHECK_EQ(f.state.page, kPageEditNet);  // Stays put to fix the error.
        f.state.edit_net_name = "Renamed";
        CHECK(SaveEditNetForm(&f.state));
        CHECK_EQ(f.state.nets[0].name, std::string("Renamed"));
        // Saving closes the page.
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK_EQ(f.state.status_message, std::string("Saved Renamed."));
    }

    QL_TEST(SavedStationsAreEditedInTheirOwnWindow)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        CHECK(!f.state.show_saved_station_modal);

        OpenNewSavedStationForm(&f.state);
        CHECK(f.state.show_saved_station_modal);
        f.state.saved_station.callsign = "K4AAA";
        f.state.saved_station.name = "Ann";
        f.state.saved_station_remarks = "mobile";
        CHECK(SaveNetStationForm(&f.state));
        CHECK_EQ(f.state.status_message, std::string("Saved K4AAA."));
        CHECK(f.state.saved_station.callsign.empty());  // Ready for the next one.
        REQUIRE(f.state.edit_net_saved_stations.size() == 1);

        CloseSavedStationForm(&f.state);
        CHECK(!f.state.show_saved_station_modal);

        LoadSavedStationIntoForm(&f.state, f.state.edit_net_saved_stations[0]);
        CHECK(f.state.show_saved_station_modal);
        CHECK_EQ(f.state.saved_station.name, std::string("Ann"));
        CHECK_EQ(f.state.saved_station_remarks, std::string("mobile"));
        f.state.saved_station.name = "Changed";
        CloseSavedStationForm(&f.state);  // Esc: nothing saved.
        CHECK_EQ(f.db()->FindStationByCallsign("K4AAA")->name, std::string("Ann"));
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

    QL_TEST(NetFrequencyMustBeAnAmateurFrequency)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        f.state.edit_net_frequency = "162.550";
        CHECK(!SaveEditNetForm(&f.state));
        CHECK(f.state.form_error.find("amateur band") != std::string::npos);
        f.state.edit_net_frequency = "146.940";
        f.state.edit_net_comments = "-600, PL 100.0; backup 442.100";
        CHECK(SaveEditNetForm(&f.state));
        std::optional<Net> saved = f.db()->GetNetById(net_id);
        CHECK_EQ(saved->default_frequency, std::string("146.940"));
        CHECK_EQ(saved->comments, std::string("-600, PL 100.0; backup 442.100"));
        OpenEditNetForm(&f.state, *saved);
        CHECK_EQ(f.state.edit_net_comments, std::string("-600, PL 100.0; backup 442.100"));

        // Offset and PL tone: checked, the tone saved in its usual form.
        f.state.edit_net_offset = "-600";
        CHECK(!SaveEditNetForm(&f.state));
        CHECK(f.state.form_error.find("type -0.6") != std::string::npos);
        f.state.edit_net_offset = "-0.6";
        f.state.edit_net_tone = "101";
        CHECK(!SaveEditNetForm(&f.state));
        CHECK(f.state.form_error.find("CTCSS") != std::string::npos);
        f.state.edit_net_tone = "100";
        CHECK(SaveEditNetForm(&f.state));
        saved = f.db()->GetNetById(net_id);
        CHECK_EQ(saved->repeater_offset, std::string("-0.6"));
        CHECK_EQ(saved->pl_tone, std::string("100.0"));
        OpenEditNetForm(&f.state, *saved);
        CHECK_EQ(f.state.edit_net_offset, std::string("-0.6"));
        CHECK_EQ(f.state.edit_net_tone, std::string("100.0"));

        CHECK_EQ(DescribeNetRadio(*saved), std::string("146.940 MHz  -0.6  PL 100.0"));
        Net tone_only;
        tone_only.pl_tone = "88.5";
        CHECK_EQ(DescribeNetRadio(tone_only), std::string("PL 88.5"));
        CHECK(DescribeNetRadio(Net()).empty());

        // A new recurring net takes its comments from the New Recurring Net
        // page.
        ResetCreateNetForm(&f.state);
        f.state.new_net_name = "Club";
        f.state.new_net_frequency = "147.000";
        f.state.new_net_comments = "Backup 442.100";
        CreateNetSubmitHandler submit(&f.state);
        submit();
        CHECK(f.state.form_error.empty());
        bool found = false;
        for (const Net& net : f.db()->GetAllNets())
        {
            if (net.name == "Club")
            {
                found = true;
                CHECK_EQ(net.comments, std::string("Backup 442.100"));
            }
        }
        CHECK(found);
        CHECK(f.state.new_net_comments.empty());

        // New nets, recurring or ad hoc, are checked too.
        ResetCreateNetForm(&f.state);
        f.state.new_net_name = "Tailgate";
        f.state.new_net_frequency = "27.185";
        StartAdHocNet(&f.state);
        CHECK(f.state.form_error.find("amateur band") != std::string::npos);
        CHECK(!f.state.start_net.is_ad_hoc);
    }

    // ---- Help and the seldom-used windows ----------------------------------------

    // A closed earlier session of `net_id` on `date` with these check-ins.
    static void AddPastSession(Database* db, std::int64_t net_id, const std::string& date,
                               const std::vector<std::string>& callsigns)
    {
        std::int64_t session = AddTestInstance(db, net_id, date, 500, "W4KWK");
        int number = 1;
        for (const std::string& callsign : callsigns)
        {
            AddTestCheckIn(db, session, callsign, number++);
        }
        db->CloseNetInstance(session, 600);
    }

    QL_TEST(StationHistoryShowsTheirOtherCheckInsToThisNet)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        AddPastSession(f.db(), net_id, "2026-09-10", {"K4AAA", "K4BBB"});
        AddPastSession(f.db(), net_id, "2026-09-17", {"K4BBB"});
        f.Log("K4AAA");
        StartRowPick(&f.state, RowPickAction::kViewStationHistory);
        TypeRowPickDigit(&f.state, '2');
        FinishRowPick(&f.state);
        CHECK(f.state.info_window == InfoWindow::kStationHistory);
        CHECK(f.state.show_info_window);
        CHECK_EQ(f.state.info_title, std::string("Station History: K4AAA"));
        REQUIRE(f.state.info_rows.size() == 1);  // Not today's.
        CHECK(f.state.info_rows[0].find("2026-09-10") == 0);
        CHECK(f.state.info_summary.back().find("1 of 2") != std::string::npos);
        CloseInfoWindow(&f.state);
        CHECK(!f.state.show_info_window);
    }

    QL_TEST(RegularsAreThoseInHalfTheRecentSessionsNotYetHeard)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        // Four earlier sessions: K4AAA in all, K4BBB in two (half), K4CCC in one.
        AddPastSession(f.db(), net_id, "2026-09-01", {"K4AAA", "K4BBB"});
        AddPastSession(f.db(), net_id, "2026-09-08", {"K4AAA", "K4BBB", "K4CCC"});
        AddPastSession(f.db(), net_id, "2026-09-15", {"K4AAA"});
        AddPastSession(f.db(), net_id, "2026-09-22", {"K4AAA"});
        OpenRegulars(&f.state);
        REQUIRE(f.state.info_rows.size() == 2);
        CHECK(f.state.info_rows[0].find("K4AAA") == 0);
        CHECK(f.state.info_rows[0].find("4 of 4") != std::string::npos);
        CHECK(f.state.info_rows[1].find("K4BBB") == 0);
        CHECK(f.state.info_rows[1].find("2 of 4") != std::string::npos);

        // Enter on one checks it in: New Check-In opens with it filled in.
        f.state.info_selected = 1;
        CheckInSelectedRegular(&f.state);
        CHECK(!f.state.show_info_window);
        CHECK(f.state.show_new_station_modal);
        CHECK_EQ(f.state.modal_station.callsign, std::string("K4BBB"));

        // Once heard, a regular drops off the list.
        REQUIRE(LogStationCheckIn(&f.state));
        OpenRegulars(&f.state);
        REQUIRE(f.state.info_rows.size() == 1);
        CHECK(f.state.info_rows[0].find("K4AAA") == 0);
    }

    QL_TEST(SessionSummaryNamesFirstTimers)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        AddPastSession(f.db(), net_id, "2026-09-10", {"K4AAA", "K4BBB", "K4CCC"});
        f.Log("K4AAA");
        f.Log("K4NEW");
        OpenSessionSummary(&f.state);
        CHECK(f.state.info_summary[0].find("3 check-ins so far") == 0);
        CHECK(f.state.info_summary[1].find("averaged 3.0 check-ins") != std::string::npos);
        // W4KWK, the operator, has only this session too.
        REQUIRE(f.state.info_rows.size() == 2);
        CHECK(f.state.info_rows[0].find("W4KWK") != std::string::npos);
        CHECK(f.state.info_rows[1].find("K4NEW") != std::string::npos);
    }

    QL_TEST(NetStatisticsAndStationSearch)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        AddPastSession(f.db(), net_id, "2026-08-10", {"K4AAA", "K4BBB"});
        AddPastSession(f.db(), net_id, "2026-09-10", {"K4AAA", "K4BBB", "K4CCC", "K4DDD"});
        RefreshNets(&f.state);
        OpenNetStatistics(&f.state);
        CHECK(f.state.info_window == InfoWindow::kNetStatistics);
        CHECK(f.state.info_summary[0].find("2 sessions, 2026-08-10 to 2026-09-10") == 0);
        CHECK(f.state.info_summary[1].find("Average 3.0 check-ins a session; most 4") == 0);
        REQUIRE(f.state.info_rows.size() == 4);
        CHECK(f.state.info_rows[0].find("K4AAA") == 0);

        OpenStationSearch(&f.state);
        CHECK(f.state.info_rows.empty());
        f.state.info_query = "k4a";
        RefreshStationSearch(&f.state);
        CHECK_EQ(f.state.info_query, std::string("K4A"));
        REQUIRE(f.state.info_rows.size() == 2);
        CHECK(f.state.info_rows[0].find("2026-09-10  Skywarn") == 0);

        // The window's table fits 80 columns, and widens with the terminal.
        for (const std::string& row : f.state.info_rows)
        {
            CHECK(row.size() <= 70);
        }
        CHECK(f.state.info_header.find("Name") == std::string::npos);
        UpdateListWidths(&f.state, 140);
        CHECK(f.state.info_header.find("Name") != std::string::npos);
        CHECK(f.state.info_rows[0].find("2026-09-10  Skywarn") == 0);
    }

    QL_TEST(StationSearchTagsAdHocNets)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        Net ad_hoc;
        ad_hoc.name = "Saturday Tailgate";
        ad_hoc.is_ad_hoc = true;
        std::int64_t ad_hoc_id = f.db()->CreateNet(ad_hoc);
        AddPastSession(f.db(), net_id, "2026-08-10", {"K4AAA"});
        AddPastSession(f.db(), ad_hoc_id, "2026-09-10", {"K4AAA"});

        OpenStationSearch(&f.state);
        f.state.info_query = "K4AAA";
        RefreshStationSearch(&f.state);
        REQUIRE(f.state.info_rows.size() == 2);
        // At 80 columns the name is cut to keep the tag; wider, it all shows.
        CHECK(f.state.info_rows[0].find(" (ad hoc)") != std::string::npos);
        CHECK(f.state.info_rows[0].find("2026-09-10  Saturda (ad hoc)") == 0);
        CHECK(f.state.info_rows[1].find("(ad hoc)") == std::string::npos);
        CHECK(f.state.info_rows[0].size() <= 70);
        UpdateListWidths(&f.state, 140);
        CHECK(f.state.info_rows[0].find("Saturday Tailgate (ad hoc)") != std::string::npos);
        CHECK(f.state.info_rows[1].find("(ad hoc)") == std::string::npos);
    }

    QL_TEST(QuietStationsHaventCheckedInForSixMonths)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        f.db()->SaveNetStation(net_id, MakeStation("K4NEV"), "", 1);
        f.db()->SaveNetStation(net_id, MakeStation("K4OLD"), "", 1);
        f.db()->SaveNetStation(net_id, MakeStation("K4NOW"), "", 1);
        AddPastSession(f.db(), net_id, "2020-01-01", {"K4OLD"});
        AddPastSession(f.db(), net_id,
                       FormatLocalDate(static_cast<std::int64_t>(std::time(nullptr))), {"K4NOW"});
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        OpenQuietStations(&f.state);
        REQUIRE(f.state.info_rows.size() == 2);
        CHECK(f.state.info_rows[0].find("K4NEV") == 0);
        CHECK(f.state.info_rows[0].find("never") != std::string::npos);
        CHECK(f.state.info_rows[1].find("K4OLD") == 0);
        CHECK(f.state.info_summary[0].find("2 of 3 saved stations") == 0);
    }

    QL_TEST(HelpExplainsEveryKeyOnThePage)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.state.page = kPageActiveNet;
        OpenHelp(&f.state);
        CHECK(f.state.info_window == InfoWindow::kHelp);
        bool found_extra = false;
        for (const std::string& row : f.state.info_rows)
        {
            found_extra =
                found_extra || (row.find("F8") == 0 && row.find(" *") != std::string::npos);
        }
        CHECK(found_extra);
        CHECK(!f.state.info_summary.empty());  // The note about extra keys.
        f.state.page = kPageNetList;
        OpenHelp(&f.state);
        CHECK(f.state.info_rows[0].find("F2") == 0);
        CHECK(f.state.info_summary.empty());  // No extra keys there.
    }

    QL_TEST(StationCardGathersWhatsKnown)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        f.db()->BulkUpsertUlsStations({MakeStation("K4AAA", "ANN", "37415", "Chattanooga")}, 0, 1,
                                      1);
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA", "Ann", "37415", "Chattanooga"), "", 1);
        f.Log("K4AAA");
        OpenStationCard(&f.state, "K4AAA");
        CHECK_EQ(f.state.info_title, std::string("Station: K4AAA"));
        CHECK(f.state.info_summary[0].find("Ann") != std::string::npos);
        bool saved_to = false;
        bool one_check_in = false;
        for (const std::string& line : f.state.info_summary)
        {
            saved_to = saved_to || line.find("Saved to:       Skywarn") == 0;
            one_check_in = one_check_in || line.find("Check-ins:      1 (") == 0;
        }
        CHECK(saved_to);
        CHECK(one_check_in);
    }

    // ---- Viewer ----------------------------------------------------------------------

    QL_TEST(AViewerWatchesAnOpenSessionWithoutChangingIt)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        f.Log("K4AAA");
        std::int64_t session = f.state.active_instance.id;

        // Someone else picks the net and chooses Viewer.
        f.state.active_instance = NetInstance();
        f.state.active_check_ins.clear();
        f.state.start_net = *f.db()->GetNetById(net_id);
        f.state.selected_role_index = kRoleViewer;
        ViewStartNet(&f.state);
        CHECK_EQ(f.state.page, kPageActiveNet);
        CHECK(f.state.viewing_only);
        CHECK_EQ(f.state.active_instance.id, session);
        CHECK_EQ(f.state.active_check_ins.size(), std::size_t{2});
        CHECK(f.state.status_message.find("Watching the") == 0);

        // Regulars can be looked at, but Enter doesn't check anyone in.
        OpenRegulars(&f.state);
        f.state.info_stations = {MakeStation("K4ZZZ")};
        CheckInSelectedRegular(&f.state);
        CHECK(!f.state.show_new_station_modal);
        CloseInfoWindow(&f.state);

        // Help shows only what a Viewer can do.
        OpenHelp(&f.state);
        bool check_in_key = false;
        for (const std::string& row : f.state.info_rows)
        {
            check_in_key = check_in_key || row.find("Check in a station") != std::string::npos;
        }
        CHECK(!check_in_key);
        CloseInfoWindow(&f.state);

        // Leaving changes nothing: the session is still open, with its two check-ins.
        StopViewing(&f.state);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK(!f.state.viewing_only);
        CHECK(f.db()->GetNetInstanceById(session)->status == NetInstanceStatus::kOpen);
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(session).size(), std::size_t{2});
    }

    QL_TEST(ThereIsNothingToViewWithoutAnOpenSession)
    {
        Fixture f;
        std::int64_t net_id = AddTestNet(f.db(), "Skywarn");
        f.state.page = kPageSelectRole;
        f.state.start_net = *f.db()->GetNetById(net_id);
        ViewStartNet(&f.state);
        CHECK_EQ(f.state.page, kPageSelectRole);
        CHECK(f.state.form_error.find("No session of Skywarn is open to watch") == 0);
    }

    QL_TEST(TheResumePromptCanJustView)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        f.state.resume_instance = f.state.active_instance;
        f.state.start_net = *f.db()->GetNetById(net_id);
        ViewOpenNet(&f.state);
        CHECK(f.state.viewing_only);
        CHECK_EQ(f.state.selected_role_index, kRoleViewer);
        // Joining to log afterwards is back to normal.
        ResumeOpenNet(&f.state);
        CHECK(!f.state.viewing_only);
    }

    QL_TEST(AStationCanOnlyCheckInOncePerSession)
    {
        Fixture f;
        f.StartNet("Skywarn");
        REQUIRE(f.Log("K4AAA"));
        CHECK(!f.Log("k4aaa"));
        CHECK_EQ(f.state.form_error, std::string("K4AAA is already in this session's log, as #2."));
        // Mobile, portable or operating from elsewhere, it's the same station.
        CHECK(!f.Log("K4AAA/M"));
        CHECK_EQ(f.state.form_error,
                 std::string("K4AAA/M is already in this session's log, as K4AAA #2."));
        CHECK(!f.Log("VE3/K4AAA"));
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(f.state.active_instance.id).size(),
                 std::size_t{2});
        // The operator is in it too.
        CHECK(!f.Log("W4KWK"));
        CHECK(!f.Log("W4KWK/M"));
        CHECK(f.Log("K4BBB/P"));
        CHECK(!f.Log("K4BBB"));
    }

    QL_TEST(ManageUsersIsConsoleOnlyAndNeedsTheSshServer)
    {
        Fixture f;
        f.state.page = kPageSettings;
        SettingsKeyHandler settings_keys(&f.state);
        settings_keys(ftxui::Event::F4);
#if defined(QUICKLOGGER_WITH_SSH)
        CHECK(CanManageUsers(&f.state));
        CHECK_EQ(f.state.page, kPageManageUsers);
#else
        // No SSH server (Windows): no users to manage.
        CHECK(!CanManageUsers(&f.state));
        CHECK_EQ(f.state.page, kPageSettings);
#endif

        f.state.page = kPageSettings;
        f.state.is_console_session = false;
        CHECK(!CanManageUsers(&f.state));
        settings_keys(ftxui::Event::F4);
        CHECK_EQ(f.state.page, kPageSettings);
    }

}  // namespace ql
