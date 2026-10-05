// The app's behavior below the screen: logging, editing and deleting
// check-ins, numbered picks, autocomplete, county fill-in, import/export.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../src/date_utils.hpp"
#include "../src/public_key.hpp"
#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/mode_rules.hpp"
#include "../src/net_slice.hpp"
#include "../src/show_folder.hpp"
#include "../src/update_check.hpp"
#include "../src/ui/mouse.hpp"
#include "../src/zmodem_send.hpp"
#include "../src/ui/app_state.hpp"
#include "../src/ui/pages.hpp"
#include <ftxui/screen/screen.hpp>
#include "../src/ui/handlers.hpp"
#include "precise_grid_fakes.hpp"
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
        bool Log(const std::string& callsign, const std::string& name = "", int role_choice_index = 0)
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
        CHECK_EQ(f.db()->GetNetInstanceById(f.state.active_instance.id)->net_control_callsign, std::string("W4KWK"));
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
        FinishRowPick(&f.state);                       // Enter with nothing typed: the highlighted row.
        CHECK(f.state.show_edit_checkin_modal);
    }

    QL_TEST(SessionNotesAreEditedWithF12AndSavedWithF2)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.state.page = kPageActiveNet;
        AppKeyHandler keys(&f.state);
        CHECK(keys(ftxui::Event::F12));
        CHECK(f.state.show_session_notes_modal);
        CHECK(!f.state.session_notes_read_only);
        CHECK(f.state.session_notes_text.empty());

        // Esc leaves the notes as they were: none.
        f.state.session_notes_text = "Not kept";
        CHECK(keys(ftxui::Event::Escape));
        CHECK(!f.state.show_session_notes_modal);
        CHECK(f.db()->GetNetInstanceById(f.state.active_instance.id)->notes.empty());

        // F2 saves them, without trailing blank lines; other F-keys do
        // nothing behind the window.
        CHECK(keys(ftxui::Event::F12));
        f.state.session_notes_text = "Tornado touched down.\nMany check-ins.\n\n";
        CHECK(keys(ftxui::Event::F4));
        CHECK(!f.state.show_confirm_prompt);
        CHECK(keys(ftxui::Event::F2));
        CHECK(!f.state.show_session_notes_modal);
        CHECK(!f.state.show_new_station_modal);
        CHECK_EQ(f.db()->GetNetInstanceById(f.state.active_instance.id)->notes,
                 std::string("Tornado touched down.\nMany check-ins."));
        CHECK_EQ(f.state.status_message, std::string("Session notes saved."));

        // Opening again shows them, with the cursor at the end.
        CHECK(keys(ftxui::Event::F12));
        CHECK_EQ(f.state.session_notes_text, std::string("Tornado touched down.\nMany check-ins."));
        CHECK_EQ(f.state.session_notes_cursor, static_cast<int>(f.state.session_notes_text.size()));
        CloseSessionNotes(&f.state);

        // And from History, after the session closes.
        CloseActiveNet(&f.state);
        RefreshNetHistory(&f.state);
        f.state.page = kPageNetHistory;
        CHECK(keys(ftxui::Event::F12));
        CHECK(f.state.show_session_notes_modal);
        CHECK_EQ(f.state.session_notes_text, std::string("Tornado touched down.\nMany check-ins."));
        f.state.session_notes_text = "Edited later.";
        SaveSessionNotes(&f.state);
        CHECK_EQ(f.db()->GetNetInstanceById(f.state.history_instances[0].id)->notes, std::string("Edited later."));
    }

    QL_TEST(EditAndDeleteUseTheSameKeysAsTheActiveNet)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("Skywarn");
        // History: F5 deletes a check-in, as on the active net; F4 a session.
        RefreshNetHistory(&f.state);
        f.state.page = kPageNetHistory;
        NetHistoryKeyHandler history_keys(&f.state);
        CHECK(history_keys(ftxui::Event::F5));
        CHECK(f.state.row_pick_action == RowPickAction::kDeleteHistoryCheckIn);
        CancelRowPick(&f.state);
        CHECK(history_keys(ftxui::Event::F4));
        CHECK(f.state.row_pick_action == RowPickAction::kDeleteNetInstance);
        CancelRowPick(&f.state);

        // Edit Net: F3 edits a saved station, as F3 edits a check-in; F9
        // no longer does.
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA"), "", 1);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        f.state.page = kPageEditNet;
        EditNetKeyHandler edit_keys(&f.state);
        CHECK(!edit_keys(ftxui::Event::F9));
        CHECK(f.state.row_pick_action == RowPickAction::kNone);
        CHECK(edit_keys(ftxui::Event::F3));
        CHECK(f.state.row_pick_action == RowPickAction::kEditSavedStation);
    }

    QL_TEST(ClickingTheUpdateNoticeSaysWhereTheNewVersionIs)
    {
        Fixture f;
        // Never a desktop console, so no browser opens during the tests.
        f.state.is_console_session = false;
        AppKeyHandler keys(&f.state);
        // Nothing found: nothing happens.
        SetAvailableUpdate("");
        CHECK(keys(OpenUpdatePageEvent()));
        CHECK(f.state.status_message.empty());
        // Found, but not at a desktop console here (an SSH session): the
        // link, on whatever page.
        SetAvailableUpdate("9.9.9");
        f.state.page = kPageNetHistory;
        CHECK(keys(OpenUpdatePageEvent()));
        CHECK_EQ(f.state.status_message, "QuickLogger 9.9.9 is out: " + std::string(kReleasesPageUrl));
        SetAvailableUpdate("");
    }

    QL_TEST(AViewersSessionNotesAreReadOnly)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.db()->SetNetInstanceNotes(f.state.active_instance.id, "Theirs.");
        f.state.viewing_only = true;
        OpenActiveSessionNotes(&f.state);
        CHECK(f.state.session_notes_read_only);
        CHECK_EQ(f.state.session_notes_text, std::string("Theirs."));
        f.state.session_notes_text = "Changed";
        SaveSessionNotes(&f.state);
        CHECK(!f.state.show_session_notes_modal);
        CHECK_EQ(f.db()->GetNetInstanceById(f.state.active_instance.id)->notes, std::string("Theirs."));
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
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(f.state.active_instance.id).size(), std::size_t{2});
        CHECK(!f.state.show_new_station_modal);
        CHECK(f.state.form_error.empty());
        REQUIRE(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kSessionClosed);
        CHECK_EQ(f.state.confirm_prompt_title, std::string("Net Closed"));
        std::string text = PromptText(f.state);
        CHECK(text.find("Another user has closed this net at ") == 0);
        CHECK(text.find(FormatLocalTimeOfDay(2000) + ".\n") != std::string::npos);
        CHECK(text.find("K4BBB was not logged.\n") != std::string::npos);
        CHECK(text.find("You will be returned to the Recurring Nets list when you press Enter.") != std::string::npos);
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
        CHECK_EQ(f.db()->GetNetInstanceById(f.state.active_instance.id)->closed_at, std::int64_t{2000});
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
        CHECK(f.state.row_delete_lines[0].find("(ended " + FormatLocalDate(2000) + " " + FormatLocalTimeOfDay(2000) +
                                               ", 2 check-ins)") != std::string::npos);
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
        CHECK_EQ(f.state.history_instance_labels[0].substr(end_column, 8), FormatLocalTimeOfDay(ended));
        // End is 9 wide (a time and a space), then the one-space gap.
        CHECK_EQ(header.find("Net Control") - header.find("End"), std::size_t{10});
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
        std::string log = ReadTextFile(f.dir().File("exports/Skywarn_" + closed.instance_date + "_log.txt"));
        CHECK(log.find("End Time: " + FormatLocalTimeOfDay(closed.closed_at) + "\n") != std::string::npos);

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
        CHECK_EQ(f.state.status_message, std::string("Removed K4AAA from this net's saved stations."));
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
        f.state.new_user_amateur_callsign = "K4WES";
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
        f.state.new_user_amateur_callsign = "K4WES";
        f.state.new_user_public_key = kTestKey;
        AddUserFromForm(&f.state);
        f.state.new_user_username = "K4WES";
        f.state.new_user_public_key = kOtherTestKey;
        AddUserFromForm(&f.state);
        CHECK_EQ(f.state.status_message, std::string("Added another key for \"K4WES\" (full access)."));
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
        CHECK(f.state.user_keys_labels[0].find("ED25519  SHA256:zSpp/") != std::string::npos);
        CHECK(f.state.user_keys_labels[0].find("test@quicklogger") != std::string::npos);
        CHECK(f.state.user_keys_labels[0].find("never") != std::string::npos);
        CHECK(f.state.user_keys_labels[1].find("ECDSA    SHA256:19j6m") != std::string::npos);
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
        f.state.new_user_amateur_callsign = "KB4VEW";
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
        CHECK_EQ(f.state.status_message, std::string("Added another key for \"KB4VEW\" (view-only)."));

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

    QL_TEST(UsernamesAreLoginNamesWithCallSignsOfTheirOwn)
    {
        Fixture f;
        f.state.new_user_public_key = kTestKey;
        // A username is any login name, but not just any characters.
        f.state.new_user_username = "k4wes/m";
        f.state.new_user_amateur_callsign = "K4WES";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.find("A username has 1 to 32 letters") == 0);
        // At least one call sign, each a valid one.
        f.state.new_user_username = "wes";
        f.state.new_user_amateur_callsign = "";
        AddUserFromForm(&f.state);
        CHECK_EQ(f.state.form_error, std::string("At least one call sign is required: Amateur Radio or GMRS."));
        f.state.new_user_amateur_callsign = "k4wes/m";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.find("K4WES/M isn't a valid") == 0);
        f.state.new_user_amateur_callsign = "";
        f.state.new_user_gmrs_callsign = "W4KWK";
        AddUserFromForm(&f.state);
        CHECK_EQ(f.state.form_error, std::string("W4KWK isn't a valid GMRS call sign."));
        CHECK(f.db()->ListUsers().empty());

        // A GMRS licensee with no amateur call sign.
        f.state.new_user_gmrs_callsign = "wsip663";
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.empty());
        std::vector<User> keys = f.db()->GetUserKeys("WES");
        REQUIRE(keys.size() == 1);
        CHECK_EQ(keys[0].username, std::string("wes"));
        CHECK_EQ(keys[0].gmrs_callsign, std::string("WSIP663"));
        CHECK(keys[0].amateur_callsign.empty());
        REQUIRE(f.state.manage_users_labels.size() == 1);
        CHECK(f.state.manage_users_labels[0].find("WSIP663") != std::string::npos);

        // Another key keeps their call signs, whatever the form says.
        f.state.new_user_username = "WES";
        f.state.new_user_public_key = kOtherTestKey;
        AddUserFromForm(&f.state);
        CHECK(f.state.form_error.empty());
        keys = f.db()->GetUserKeys("wes");
        REQUIRE(keys.size() == 2);
        CHECK_EQ(keys[1].gmrs_callsign, std::string("WSIP663"));
    }

    QL_TEST(RenamingAUserTakesTheirSettingsAndFilesAlong)
    {
        Fixture f;
        for (const char* username : {"K4WES", "KB4VEW"})
        {
            User user;
            user.username = username;
            user.public_key = kTestKey;
            user.amateur_callsign = username;
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

        CHECK_EQ(f.state.edit_user_amateur_callsign, std::string("K4WES"));

        // Not a username, or someone else's: refused, and nothing saved.
        f.state.edit_user_access_index = 1;
        f.state.rename_username = "wes!";
        SaveEditedUser(&f.state);
        CHECK(f.state.form_error.find("A username has") == 0);
        f.state.rename_username = "kb4vew";
        SaveEditedUser(&f.state);
        CHECK_EQ(f.state.form_error, std::string("kb4vew is already a user."));
        CHECK(f.state.show_user_keys_modal);
        CHECK_EQ(f.db()->GetUserKeys("K4WES").size(), std::size_t{1});
        CHECK(!f.db()->IsUserViewOnly("K4WES"));

        // Renamed, made view-only and given a GMRS call sign in one save.
        f.state.rename_username = "wes";
        f.state.edit_user_gmrs_callsign = "wsip663";
        SaveEditedUser(&f.state);
        CHECK(f.state.form_error.empty());
        CHECK_EQ(f.state.status_message, std::string("Renamed K4WES to wes and saved, from their next login."));
        CHECK(!f.state.show_user_keys_modal);
        CHECK(f.db()->GetUserKeys("K4WES").empty());
        REQUIRE(f.db()->GetUserKeys("wes").size() == 1);
        CHECK_EQ(f.db()->GetUserKeys("wes")[0].username, std::string("wes"));
        CHECK_EQ(f.db()->GetUserKeys("wes")[0].amateur_callsign, std::string("K4WES"));
        CHECK_EQ(f.db()->GetUserKeys("wes")[0].gmrs_callsign, std::string("WSIP663"));
        CHECK(f.db()->IsUserViewOnly("wes"));
        CHECK_EQ(f.state.manage_user_names[static_cast<std::size_t>(f.state.selected_user_index)], std::string("wes"));
        // Their settings and files moved with them.
        CHECK(!std::filesystem::exists(SshUserSettingsPath(f.state.db_path, "K4WES")));
        CHECK_EQ(ReadTextFile(SshUserSettingsPath(f.state.db_path, "wes")), std::string("location=37402\n"));
        CHECK_EQ(ListFilesWithExtension(SessionExportsDir(f.state.db_path, "wes"), ".txt").size(), std::size_t{1});
        CHECK(!std::filesystem::exists(SessionExportsDir(f.state.db_path, "K4WES")));
    }

    QL_TEST(AnSshUsersCallSignsAreManageUsersToSet)
    {
        Fixture f;
        f.state.ssh_username = "wes";
        f.state.callsign_editable = false;
        f.state.settings.callsign = "K4WES";
        f.state.settings.gmrs_callsign = "WSIP663";
        f.state.settings_path = f.dir().File("wes.txt");
        OpenSettingsForm(&f.state);
        f.state.settings_form.callsign = "W4KWK";  // However it got there.
        f.state.settings_form.gmrs_callsign = "";
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK_EQ(f.state.settings.callsign, std::string("K4WES"));
        CHECK_EQ(f.state.settings.gmrs_callsign, std::string("WSIP663"));
    }

    QL_TEST(TheConsoleSavesItsServerAddressAndRefusesABadOne)
    {
        Fixture f;
        f.state.is_console_session = true;
        f.state.settings_path = f.dir().File("settings.txt");
        f.state.settings.callsign = "W4KWK";
        f.state.settings.location = "02101";
        OpenSettingsForm(&f.state);
        CHECK(!f.state.settings_server_placeholder.empty());
        f.state.settings_server_text = "net.example.org:22x";
        CHECK(!SaveSettingsForm(&f.state));
        f.state.settings_server_text = "net.example.org:2200";
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK_EQ(f.state.settings.server_address, std::string("net.example.org"));
        CHECK_EQ(f.state.settings.server_port, 2200);
        CHECK_EQ(LoadSettings(f.dir().File("settings.txt")).server_port, 2200);
        // Blank puts it back to "work it out".
        OpenSettingsForm(&f.state);
        CHECK_EQ(f.state.settings_server_text, std::string("net.example.org:2200"));
        f.state.settings_server_text = "";
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK(f.state.settings.server_address.empty());
        CHECK_EQ(f.state.settings.server_port, 0);
    }

    QL_TEST(AnSshUsersSettingsNeverHoldAServerAddress)
    {
        Fixture f;
        f.state.is_console_session = false;
        f.state.ssh_username = "wes";
        f.state.callsign_editable = false;
        f.state.settings.callsign = "K4WES";
        f.state.settings.location = "02101";
        f.state.settings_path = f.dir().File("wes.txt");
        OpenSettingsForm(&f.state);
        f.state.settings_server_text = "net.example.org";
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK(f.state.settings.server_address.empty());
    }

    QL_TEST(ACommentReplacesOnlyTheCommentOfAKeyLine)
    {
        std::string line;
        std::string error;
        REQUIRE(WithPublicKeyComment("ssh-ed25519 AAAAC3Nza old name", "  My Mac ", &line, &error));
        CHECK_EQ(line, std::string("ssh-ed25519 AAAAC3Nza My Mac"));
        REQUIRE(WithPublicKeyComment("ssh-ed25519 AAAAC3Nza old name", "", &line, &error));
        CHECK_EQ(line, std::string("ssh-ed25519 AAAAC3Nza"));
        CHECK(!WithPublicKeyComment("ssh-ed25519 AAAAC3Nza", "bad\ncomment", &line, &error));
        CHECK(!WithPublicKeyComment("ssh-ed25519 AAAAC3Nza", std::string(65, 'x'), &line, &error));
        CHECK(!WithPublicKeyComment("garbage", "ok", &line, &error));
        CHECK(SamePublicKey("ssh-ed25519 AAAAC3Nza old", "ssh-ed25519 AAAAC3Nza new"));
    }

    QL_TEST(MyKeysMarksTheLoginAndRenamesAKey)
    {
        Fixture f;
        for (const char* key :
             {"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINM3eCDBCkdxto9OIGli2KKnorIhCylrEpYHnMPxdkAI laptop",
              "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJ3eCDBCkdxto9OIGli2KKnorIhCylrEpYHnMPxdkAI"})
        {
            User user;
            user.username = "K4WES";
            user.public_key = key;
            user.amateur_callsign = "K4WES";
            f.db()->CreateUser(user);
        }
        std::vector<User> keys = f.db()->GetUserKeys("K4WES");
        REQUIRE(keys.size() == 2);
        f.state.is_console_session = false;
        f.state.ssh_username = "K4WES";
        f.state.ssh_key_id = keys[1].id;
        CHECK(CanEditOwnKeys(&f.state));
        OpenMyKeys(&f.state);
        CHECK(f.state.show_my_keys_window);
        CHECK_EQ(f.state.selected_my_key_index, 1);
        CHECK(f.state.my_keys_labels[1].find("<- this login") != std::string::npos);
        CHECK(f.state.my_keys_labels[0].find("laptop") != std::string::npos);
        CHECK(f.state.my_keys_labels[1].find("(no comment)") != std::string::npos);
        CHECK(f.state.my_key_comment_text.empty());

        f.state.my_key_comment_text = "My Mac";
        SaveMyKeyComment(&f.state);
        CHECK(f.state.form_error.empty());
        CHECK(f.state.my_keys_labels[1].find("My Mac") != std::string::npos);
        CHECK(f.db()->GetUserKeys("K4WES")[1].public_key.find(" My Mac") != std::string::npos);

        f.state.my_key_comment_text = "bad\ncomment";
        SaveMyKeyComment(&f.state);
        CHECK(!f.state.form_error.empty());
        CloseMyKeys(&f.state);
        CHECK(!f.state.show_my_keys_window);
    }

    QL_TEST(AKeysTransferMethodIsSavedAndSftpKeysAreNotOfferedZmodem)
    {
        Fixture f;
        for (const char* key : {"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINM3eCDBCkdxto9OIGli2KKnorIhCylrEpYHnMPxdkAI one",
                                "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJ3eCDBCkdxto9OIGli2KKnorIhCylrEpYHnMPxdkAI two"})
        {
            User user;
            user.username = "K4WES";
            user.public_key = key;
            user.amateur_callsign = "K4WES";
            f.db()->CreateUser(user);
        }
        std::vector<User> keys = f.db()->GetUserKeys("K4WES");
        f.state.is_console_session = false;
        f.state.ssh_username = "K4WES";
        f.state.ssh_key_id = keys[0].id;
        f.state.console_settings_path = f.dir().File("settings.txt");
        SetTestEnvironment("SSH_CONNECTION", "");
        LoadSessionTransferMethod(&f.state);
        CHECK(!SessionPrefersSftp(&f.state));

        OpenMyKeys(&f.state);
        CHECK_EQ(f.state.my_key_transfer_index, kTransferAsk);
        f.state.my_key_transfer_index = kTransferSftp;
        SaveMyKeyComment(&f.state);
        CHECK(f.state.form_error.empty());
        CHECK(SessionPrefersSftp(&f.state));
        CHECK_EQ(f.db()->GetUserKeys("K4WES")[0].transfer_method, kTransferSftp);
        CHECK_EQ(f.db()->GetUserKeys("K4WES")[1].transfer_method, kTransferAsk);
        CHECK(f.state.my_keys_labels[0].find("SFTP") != std::string::npos);

        // An SFTP key is told the scp command, not offered ZMODEM.
        OfferZmodemSendFiles(&f.state, {"./exports/ssh-users/K4WES/Net.qlnet"});
        CHECK(!f.state.show_zmodem_confirm_modal);
        CHECK(f.state.status_message.find("scp") != std::string::npos);

        // The other key, still on Ask, is not affected on its next login.
        f.state.ssh_key_id = keys[1].id;
        LoadSessionTransferMethod(&f.state);
        CHECK(!SessionPrefersSftp(&f.state));
    }

    QL_TEST(TabMovesFromTheKeyListToItsCommentAndTransferFields)
    {
        Fixture f;
        for (const char* key : {"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINM3eCDBCkdxto9OIGli2KKnorIhCylrEpYHnMPxdkAI one",
                                "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJ3eCDBCkdxto9OIGli2KKnorIhCylrEpYHnMPxdkAI two"})
        {
            User user;
            user.username = "K4WES";
            user.public_key = key;
            user.amateur_callsign = "K4WES";
            f.db()->CreateUser(user);
        }
        f.state.is_console_session = false;
        f.state.ssh_username = "K4WES";
        ftxui::Component page = BuildSettingsPage(&f.state);
        OpenMyKeys(&f.state);
        REQUIRE(f.state.show_my_keys_window);
        // The list is first; Down picks the next key, Tab leaves for the fields.
        CHECK_EQ(f.state.my_keys_focus, 0);
        page->OnEvent(ftxui::Event::ArrowDown);
        CHECK_EQ(f.state.selected_my_key_index, 1);
        CHECK_EQ(f.state.my_keys_focus, 0);
        page->OnEvent(ftxui::Event::Tab);
        CHECK_EQ(f.state.selected_my_key_index, 1);
        CHECK_EQ(f.state.my_keys_focus, 1);
        page->OnEvent(ftxui::Event::Tab);
        CHECK_EQ(f.state.my_keys_focus, 2);
        page->OnEvent(ftxui::Event::TabReverse);
        page->OnEvent(ftxui::Event::TabReverse);
        CHECK_EQ(f.state.my_keys_focus, 0);
        CHECK_EQ(f.state.selected_my_key_index, 1);
    }

    QL_TEST(DownInMyKeysMovesToTheNextKeyNotTheLast)
    {
        Fixture f;
        const char* keys[] = {"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA one",
                              "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB two",
                              "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAICCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC three",
                              "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD four"};
        for (const char* key : keys)
        {
            User user;
            user.username = "K4WES";
            user.public_key = key;
            user.amateur_callsign = "K4WES";
            f.db()->CreateUser(user);
        }
        std::vector<User> stored = f.db()->GetUserKeys("K4WES");
        REQUIRE(stored.size() == 4);
        f.state.is_console_session = false;
        f.state.ssh_username = "K4WES";
        f.state.ssh_key_id = stored[0].id;
        ftxui::Component page = BuildSettingsPage(&f.state);
        OpenMyKeys(&f.state);
        // Drawn first, as the real screen is, before any key.
        ftxui::Screen drawn(100, 40);
        ftxui::Render(drawn, page->Render());
        CHECK_EQ(f.state.selected_my_key_index, 0);
        page->OnEvent(ftxui::Event::ArrowDown);
        CHECK_EQ(f.state.selected_my_key_index, 1);
        page->OnEvent(ftxui::Event::ArrowDown);
        CHECK_EQ(f.state.selected_my_key_index, 2);
        page->OnEvent(ftxui::Event::ArrowUp);
        CHECK_EQ(f.state.selected_my_key_index, 1);
        // The comment follows the highlight.
        CHECK_EQ(f.state.my_key_comment_text, std::string("two"));
    }

    QL_TEST(MyKeysShowsItsCursorOnTheKeyThisLoginUsed)
    {
        Fixture f;
        const char* keys[] = {"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA one",
                              "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB two",
                              "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAICCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC three",
                              "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD four"};
        for (const char* key : keys)
        {
            User user;
            user.username = "K4WES";
            user.public_key = key;
            user.amateur_callsign = "K4WES";
            f.db()->CreateUser(user);
        }
        std::vector<User> stored = f.db()->GetUserKeys("K4WES");
        f.state.is_console_session = false;
        f.state.ssh_username = "K4WES";
        f.state.ssh_key_id = stored[2].id;
        ftxui::Component page = BuildSettingsPage(&f.state);
        OpenMyKeys(&f.state);
        CHECK_EQ(f.state.selected_my_key_index, 2);
        ftxui::Screen drawn(100, 40);
        ftxui::Render(drawn, page->Render());
        // The drawn cursor (reverse video) is on the highlighted key's row,
        // not left on the first.
        int cursor_row = -1;
        int login_row = -1;
        for (int y = 0; y < drawn.dimy(); ++y)
        {
            std::string text;
            bool inverted = false;
            for (int x = 0; x < drawn.dimx(); ++x)
            {
                text += drawn.PixelAt(x, y).character;
                inverted = inverted || drawn.PixelAt(x, y).inverted;
            }
            if (text.find("this login") != std::string::npos)
            {
                login_row = y;
            }
            if (inverted && text.find("ED25519") != std::string::npos)
            {
                cursor_row = y;
            }
        }
        REQUIRE(login_row >= 0);
        CHECK_EQ(cursor_row, login_row);
        // And Down goes to the key below it.
        page->OnEvent(ftxui::Event::ArrowDown);
        CHECK_EQ(f.state.selected_my_key_index, 3);
    }

    QL_TEST(AStoredGridIsOnlyEverExtended)
    {
        Fixture f;
        const char* grids[] = {"", "FM18", "EM75", "FM18aa"};
        const char* calls[] = {"K4AAA", "K4BBB", "K4CCC", "K4DDD"};
        for (int i = 0; i < 4; ++i)
        {
            Station station = ArlingtonStation(calls[i]);
            station.grid_square = grids[i];
            f.db()->UpsertStation(station);
            f.db()->UpdateStationGrid(calls[i], "FM18mu");
        }
        CHECK_EQ(f.db()->FindStationByCallsign("K4AAA")->grid_square, std::string("FM18mu"));
        CHECK_EQ(f.db()->FindStationByCallsign("K4BBB")->grid_square, std::string("FM18mu"));
        // A different square, or 6 characters already: not changed.
        CHECK_EQ(f.db()->FindStationByCallsign("K4CCC")->grid_square, std::string("EM75"));
        CHECK_EQ(f.db()->FindStationByCallsign("K4DDD")->grid_square, std::string("FM18aa"));
        // Not 6 characters: nothing to give.
        f.db()->UpdateStationGrid("K4CCC", "EM75");
        f.db()->UpdateStationGrid("K4AAA", "FM18");
        CHECK_EQ(f.db()->FindStationByCallsign("K4AAA")->grid_square, std::string("FM18mu"));
    }

    QL_TEST(TheOperatorsOwnStationGetsItsGridAndThenTheExactOne)
    {
        Fixture f;
        ZipCentroid arlington;
        arlington.zip = "20233";
        arlington.lat = 38.845;
        arlington.lon = -76.928;
        f.db()->BulkUpsertZipCentroids({arlington});
        // The operator is a known station, with an address and no grid.
        Station self = ArlingtonStation("W4KWK");
        f.db()->UpsertStation(self);
        FakeFetcher fetcher;
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results);
        f.state.grid_lookup = &lookup;

        f.StartNet("Skywarn");
        // At once, the ZIP's grid, stored with the station...
        CHECK_EQ(f.db()->FindStationByCallsign("W4KWK")->grid_square, std::string("FM18"));
        // ...and a moment later the street's.
        REQUIRE(results.WaitForResults(1));
        REQUIRE(results.Found()[0] == "W4KWK=FM18mu");
        ApplyPreciseGrid(&f.state, "W4KWK", "FM18mu");
        CHECK_EQ(f.db()->FindStationByCallsign("W4KWK")->grid_square, std::string("FM18mu"));
        f.state.grid_lookup = nullptr;
    }

    QL_TEST(EditingASavedStationAsksForItsExactGrid)
    {
        Fixture f;
        FakeFetcher fetcher;
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results);
        f.state.grid_lookup = &lookup;
        Station saved = ArlingtonStation("K4ZZZ");
        saved.grid_square = "FM18";
        f.state.edit_net_saved_stations = {saved};
        f.state.edit_net_saved_remarks = {""};
        f.state.edit_net_saved_entry_names = {""};

        LoadSavedStationIntoForm(&f.state, 0);
        REQUIRE(results.WaitForResults(1));
        CHECK_EQ(results.Found()[0], std::string("K4ZZZ=FM18mu"));
        ApplyPreciseGrid(&f.state, "K4ZZZ", "FM18mu");
        CHECK_EQ(f.state.saved_station.grid_square, std::string("FM18mu"));
        f.state.grid_lookup = nullptr;
    }

    QL_TEST(TypingAKnownCallSignInTheSavedStationFormAsksForItsExactGrid)
    {
        Fixture f;
        Station known = ArlingtonStation("K4ZZZ");
        f.db()->UpsertStation(known);
        FakeFetcher fetcher;
        FakeResults results;
        PreciseGridLookup lookup(&fetcher, &results);
        f.state.grid_lookup = &lookup;
        f.state.show_saved_station_modal = true;
        f.state.saved_station = Station();
        f.state.saved_station.callsign = "K4ZZZ";
        // Enter with nothing marked in the matches: its details are looked up.
        ApplySelectedSavedStationSuggestion(&f.state);
        REQUIRE(results.WaitForResults(1));
        CHECK_EQ(results.Found()[0], std::string("K4ZZZ=FM18mu"));
        ApplyPreciseGrid(&f.state, "K4ZZZ", "FM18mu");
        CHECK_EQ(f.state.saved_station.grid_square, std::string("FM18mu"));
        f.state.grid_lookup = nullptr;
    }

    QL_TEST(OnlyAnSshUserHasMyKeys)
    {
        Fixture f;
        f.state.is_console_session = true;
        CHECK(!CanEditOwnKeys(&f.state));
        OpenMyKeys(&f.state);
        CHECK(!f.state.show_my_keys_window);
    }

    QL_TEST(TheConsoleNeedsOneCallSignOrTheOther)
    {
        Fixture f;
        f.state.settings_path = f.dir().File("settings.txt");
        OpenSettingsForm(&f.state);
        f.state.settings_form.callsign = "";
        f.state.settings_form.gmrs_callsign = "";
        CHECK(!SaveSettingsForm(&f.state));
        f.state.settings_form.gmrs_callsign = "wsip663";
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK(f.state.settings.callsign.empty());
        CHECK_EQ(f.state.settings.gmrs_callsign, std::string("WSIP663"));
        CHECK(SettingsAreComplete(f.state.settings));
    }

    QL_TEST(WithoutACallSignForItsServiceANetCanOnlyBeWatched)
    {
        Fixture f;
        f.state.settings.callsign = "W4KWK";
        f.state.settings.gmrs_callsign = "";
        Net family;
        family.name = "Family Net";
        family.service = NetService::kGmrs;
        f.db()->CreateNet(family);
        RefreshNets(&f.state);
        StartSelectedNet(&f.state);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK_EQ(f.state.form_error,
                 std::string("You have no GMRS call sign, so you can only watch GMRS nets. Add one in Settings (F4)."));

        // With one, the role page, prefilled with it.
        f.state.form_error.clear();
        f.state.settings.gmrs_callsign = "WSIP663";
        StartSelectedNet(&f.state);
        CHECK_EQ(f.state.page, kPageSelectRole);
        CHECK_EQ(f.state.operator_callsign, std::string("WSIP663"));
    }

    QL_TEST(RemovingAUserByNumber)
    {
        Fixture f;
        f.state.new_user_username = "K4WES";
        f.state.new_user_amateur_callsign = "K4WES";
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
        CHECK(f.db()->GetNetInstanceById(f.state.active_instance.id)->status == NetInstanceStatus::kOpen);

        RequestCloseActiveNet(&f.state);
        CloseActiveNet(&f.state);
        CHECK(f.db()->GetNetInstanceById(f.state.active_instance.id)->status == NetInstanceStatus::kClosed);
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
        std::int64_t session = AddTestInstance(f.db(), f.state.start_net.id, "2026-09-24", 1000, "W4KWK");
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
        CHECK(f.state.confirm_prompt_lines[1].find("F6 on the Ad Hoc Net page") != std::string::npos);
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
        CHECK(f.state.history_instance_labels[0].substr(net_column).find("Field Day Practice") == 0);
        CHECK(f.state.history_instance_labels[1].substr(net_column).find("Tailgate") == 0);
        CHECK_EQ(header.find("Net Control") - header.find("Net "), std::size_t{24});
        // The "> " gutter plus the 75 columns a list has at 80, so Status
        // isn't cut.
        CHECK(header.size() <= 77);

        f.state.history_ad_hoc = false;
        RefreshNets(&f.state);
        RefreshNetHistory(&f.state);
        CHECK_EQ(f.state.history_instances.size(), std::size_t{1});
    }

    QL_TEST(HistoryOpensOnTheNewestSession)
    {
        Fixture f;
        std::int64_t first = AddTestNet(f.db(), "Dixie Traders Net");
        std::int64_t second = AddTestNet(f.db(), "TAG Skywarn");
        for (int i = 0; i < 3; ++i)
        {
            AddTestInstance(f.db(), first, "2026-09-0" + std::to_string(i + 1), 100 + i, "W4KWK");
            AddTestInstance(f.db(), second, "2026-09-1" + std::to_string(i + 1), 200 + i, "W4KWK");
        }
        RefreshNets(&f.state);
        f.state.selected_net_index = 1;
        ViewNetHistoryHandler view_history(&f.state);
        view_history();
        f.state.selected_history_index = 2;

        // Back to the net list, on to the other net's History.
        f.state.selected_net_index = 0;
        view_history();
        CHECK_EQ(f.state.page, kPageNetHistory);
        CHECK_EQ(f.state.selected_history_index, 0);
        CHECK_EQ(f.state.history_instances[0].instance_date, std::string("2026-09-03"));

        // Ad hoc History likewise.
        f.state.selected_history_index = 2;
        StartAdHoc(&f, "Tailgate");
        AddTestInstance(f.db(), f.state.start_net.id, "2026-09-22", 900, "W4KWK");
        AddTestInstance(f.db(), f.state.start_net.id, "2026-09-23", 1000, "W4KWK");
        AddTestInstance(f.db(), f.state.start_net.id, "2026-09-24", 1100, "W4KWK");
        AdHocNetKeyHandler ad_hoc_keys(&f.state);
        CHECK(ad_hoc_keys(ftxui::Event::F6));
        CHECK(f.state.history_ad_hoc);
        CHECK_EQ(f.state.selected_history_index, 0);
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
        CHECK_EQ(f.state.net_names[0], "Old" + std::string(30 - 3 + 2, ' ') + "Amateur");
        // The names get a 30-column Net column (see RefreshNets), then Type,
        // the Frequency column (blank here) and the two-space gaps.
        std::string gap = std::string(30 - 7 + 2, ' ') + "Amateur" + std::string(2 + 10 + 2, ' ');
        CHECK(f.state.net_names[1].find("Skywarn" + gap + "created " + FormatLocalDate(1790000000)) == 0);
        CHECK(f.state.net_names[2].find("Skywarn" + gap + "imported " + FormatLocalDate(1790100000)) == 0);
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
        f.db()->BulkUpsertUlsStations(
            {MakeStation("K4AAA", "ULS DUPLICATE", "37415"), MakeStation("K4AAC", "NEARBY, NED", "37402"),
             MakeStation("K4AAD", "FAR, FRED", "90210"), MakeStation("K4AAE", "NASHVILLE, NAN", "37201")},
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
        CHECK(f.state.modal_callsign_suggestion_labels[0].find("(ULS, ~0 mi)") != std::string::npos);
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
            {MakeStation("K4AAA", "STAYS", "37415"), MakeStation("K4AAB", "EXPIRES", "37402")}, 0, 2, 1);
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
        f.db()->BulkUpsertUlsStations(
            {MakeStation("K4NAS", "NASHVILLE, NAN", "37201"), MakeStation("K4NAT", "NASHVILLE, NAT", "37201"),
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
        f.db()->BulkUpsertUlsStations(
            {MakeStation("K4NAS", "NASHVILLE, NAN", "37201"), MakeStation("K4NAT", "NASHVILLE, NAT", "37201"),
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
        f.db()->ReplaceIsedStations({ann, MakeStation("VE3XYZ", "Zed, Zoe"), MakeStation("VE3XZZ", "Zulu, Zak")}, 1);
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

    QL_TEST(CanadianCallSignsNearTheNetsPostalCodeShowTheirDistance)
    {
        Fixture f;
        LoadZipData(f.db());
        std::vector<ZipCentroid> fsas = {{"K1A", 45.40, -75.70}, {"K2P", 45.41, -75.69}, {"M5V", 43.64, -79.40}};
        f.db()->BulkUpsertZipCentroids(fsas);
        Station near_a = MakeStation("VE3AAA", "Able, Ann");
        near_a.zip = "K1A 0B1";
        Station near_b = MakeStation("VE3BBB", "Baker, Bob");
        near_b.zip = "K2P 1A1";
        Station far_c = MakeStation("VE3CCC", "Cole, Cy");
        far_c.zip = "M5V 2T6";
        f.db()->ReplaceIsedStations({near_a, near_b, far_c, MakeStation("VE3DDD", "Dunn, Di")}, 1);
        f.StartNet("Skywarn");
        f.state.active_net_zip = "K1A 0B1";
        ClearModalFields(&f.state);
        f.state.modal_station.callsign = "ve3";
        RefreshCallsignSuggestions(&f.state);

        // The two in Ottawa first, with distances; the rest as before.
        REQUIRE(f.state.modal_callsign_suggestions.size() == 4);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("VE3AAA"));
        CHECK_EQ(f.state.modal_callsign_suggestion_sources[0], std::string("(ISED ~0 mi)"));
        CHECK_EQ(f.state.modal_callsign_suggestions[1].callsign, std::string("VE3BBB"));
        CHECK_EQ(f.state.modal_callsign_suggestion_sources[1].find("(ISED ~"), std::size_t{0});
        CHECK_EQ(f.state.modal_callsign_suggestion_sources[2], std::string("(ISED)"));
        CHECK_EQ(f.state.modal_callsign_suggestion_sources[3], std::string("(ISED)"));

        // Picking one fills its grid from the FSA.
        f.state.selected_suggestion_index = 0;
        ApplySelectedCallsignSuggestion(&f.state);
        CHECK_EQ(f.state.modal_station.grid_square, std::string("FN25"));
    }

    QL_TEST(APostalCodeIsAcceptedAndKeptTidyInSettings)
    {
        Fixture f;
        f.state.settings_path = f.dir().File("settings.txt");
        OpenSettingsForm(&f.state);
        f.state.settings_form.location = "k1a0b1";
        REQUIRE(SaveSettingsForm(&f.state));
        CHECK_EQ(f.state.settings.location, std::string("K1A 0B1"));
        CHECK_EQ(LoadSettings(f.state.settings_path).location, std::string("K1A 0B1"));
        CHECK(SettingsAreComplete(f.state.settings));

        f.state.settings_form.location = "K1A 0B";
        CHECK(!SaveSettingsForm(&f.state));
        CHECK(!CheckNetZip(&f.state, "K1A 0B"));
        CHECK(CheckNetZip(&f.state, "K1A 0B1"));
        CHECK(CheckNetZip(&f.state, "37415"));
        CHECK(CheckNetZip(&f.state, ""));
    }

    QL_TEST(PartialMatchingIsSetPerNetToUsOrCanadianData)
    {
        Fixture f;
        LoadZipData(f.db());
        f.db()->ReplaceIsedStations({MakeStation("VE3EVA", "Able, Eva")}, 1);
        f.db()->BulkUpsertUlsStations(
            {MakeStation("KQ4EVW", "NEAR, NED", "37402"), MakeStation("EV4AA", "EARLY, EVE", "37402")}, 0, 2, 1);
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

    QL_TEST(ANetsModeIsPickedFromTheList)
    {
        Fixture f;
        // FM unless another is picked.
        ResetCreateNetForm(&f.state);
        CHECK_EQ(f.state.new_net_mode_index, 0);
        f.state.new_net_name = "Fusion Net";
        f.state.new_net_mode_index = NetModeIndex("Fusion");
        CreateNetSubmitHandler create(&f.state);
        create();
        REQUIRE(f.state.nets.size() == 1);
        CHECK_EQ(f.state.nets[0].mode, std::string("Fusion"));
        CHECK_EQ(f.state.new_net_mode_index, 0);

        // A net with no mode (an old free-text one that wasn't recognized)
        // opens on FM, saying so, and saves whatever is picked.
        Net net = f.state.nets[0];
        net.mode = "";
        f.db()->UpdateNet(net);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net.id));
        CHECK(f.state.edit_net_mode_was_blank);
        CHECK_EQ(f.state.edit_net_mode_index, 0);
        f.state.edit_net_mode_index = NetModeIndex("CW");
        CHECK(SaveEditNetForm(&f.state));
        CHECK_EQ(f.db()->GetNetById(net.id)->mode, std::string("CW"));
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net.id));
        CHECK(!f.state.edit_net_mode_was_blank);
        CHECK_EQ(f.state.edit_net_mode_index, NetModeIndex("CW"));

        // Ad hoc nets too.
        f.state.operator_callsign = "W4KWK";
        f.state.new_net_name = "Tailgate";
        f.state.new_net_mode_index = NetModeIndex("SSB");
        StartAdHocNet(&f.state);
        CHECK_EQ(f.db()->GetNetById(f.state.start_net.id)->mode, std::string("SSB"));
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
        f.db()->BulkUpsertUlsStations(
            {MakeStation("K4AAC", "CHATTANOOGA", "37402"), MakeStation("K4AAE", "NASHVILLE", "37201")}, 0, 2, 1);
        f.StartNet("Skywarn");
        f.state.active_net_zip = "37201";
        f.state.modal_station.callsign = "K4AA";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(f.state.modal_callsign_suggestions.size() == 1);
        CHECK_EQ(f.state.modal_callsign_suggestions[0].callsign, std::string("K4AAE"));
        CHECK(f.state.modal_callsign_suggestion_labels[0].find("(ULS, ~0 mi)") != std::string::npos);

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
        f.db()->BulkUpsertUlsStations(
            {MakeStation("K4AAC", "CHATTANOOGA", "37402"), MakeStation("K4AAE", "NASHVILLE", "37201")}, 0, 2, 1);
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
            net_id, MakeStation("K4LOG", "A Name That Is Far Too Long To Fit", "37415", "Chattanooga"), "", 1);
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
        std::snprintf(expected, sizeof(expected), "%-3d %-10.10s %-20.20s %-10.10s %-14.14s %-6.6s %s", 2,
                      cells[2].c_str(), cells[3].c_str(), cells[4].c_str(), cells[6].c_str(), cells[7].c_str(),
                      cells[9].c_str());
        CHECK_EQ(f.state.active_display_rows[1], std::string(expected));
        CHECK_EQ(cells[9], std::string("on time"));
        std::snprintf(expected, sizeof(expected), "  %-3.3s %-10.10s %-20.20s %-10.10s %-14.14s %-6.6s %s", "#",
                      "Callsign", "Name", "Member ID", "County", "Role", "Remarks");
        CHECK_EQ(CheckInListHeader(80), std::string(expected));

        // History sessions.
        std::snprintf(expected, sizeof(expected), "  %-11.11s %-9.9s %-9.9s %-12.12s %-13.13s %-9.9s %s", "Date",
                      "Start", "End", "Net Control", "Alternate NC", "Logger", "Status");
        CHECK_EQ(NetInstanceListHeader(80, false), std::string(expected));
        std::snprintf(expected, sizeof(expected), "  %-11.11s %-9.9s %-9.9s %-23.23s %-12.12s %s", "Date", "Start",
                      "End", "Net", "Net Control", "Status");
        CHECK_EQ(NetInstanceListHeader(80, true), std::string(expected));

        // Saved stations (since 1.8.0, a wider Name and City, State at 80
        // too, using the width) and autocomplete matches.
        std::snprintf(expected, sizeof(expected), "  %-10.10s %-24.24s %-10.10s %s", "Callsign", "Name", "Member ID",
                      "City, State");
        CHECK_EQ(SavedStationListHeader(80), std::string(expected));
        std::snprintf(expected, sizeof(expected), "  %-10.10s %-20.20s %s", "Callsign", "Name", "Source");
        CHECK_EQ(MatchListHeader(80), std::string(expected));
        f.state.modal_station.callsign = "K4AA";
        RefreshCallsignSuggestions(&f.state);
        REQUIRE(!f.state.modal_callsign_suggestion_labels.empty());
        std::snprintf(expected, sizeof(expected), "%-10.10s %-20.20s %s", "K4AAA", "ANN AMATEUR", "(this net)");
        CHECK_EQ(f.state.modal_callsign_suggestion_labels[0], std::string(expected));

        // The net list: the name padded to 30 columns (or 4 past the
        // longest name), its Type, the frequency (none here), then whether
        // a session is open.
        RefreshNets(&f.state);
        REQUIRE(f.state.net_names.size() == 1);
        CHECK_EQ(f.state.net_names[0],
                 "Skywarn" + std::string(30 - 7 + 2, ' ') + "Amateur" + std::string(2 + 10 + 2, ' ') + "session open");
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
        f.db()->SaveNetStation(net_id, MakeStation("K4AAA", "Ann", "37415", "Chattanooga"), "mobile", 1);
        RefreshNets(&f.state);
        OpenEditNetForm(&f.state, *f.db()->GetNetById(net_id));
        // At 80 the net list has the frequency (since 1.8.0) but not the
        // mode or recurrence.
        CHECK(f.state.net_names[0].find("146.940") != std::string::npos);
        CHECK(f.state.net_names[0].find("Tuesdays 8pm") == std::string::npos);
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
        CHECK(f.state.net_names[0].find("Tuesdays 8pm") == std::string::npos);
    }

    QL_TEST(ASessionExportOverScpIsAZipAndKeepsItsFiles)
    {
        Fixture f;
        f.StartNet("Skywarn");
        f.state.modal_station.callsign = "K4AAA";
        REQUIRE(LogStationCheckIn(&f.state));
        NetInstance instance = *f.db()->GetNetInstanceById(f.state.active_instance.id);
        std::vector<CheckIn> check_ins = f.db()->GetCheckInsForNetInstance(instance.id);
        f.state.is_console_session = false;
        f.state.ssh_username = "K4WES";
        f.state.over_mosh = true;
        f.state.console_settings_path = f.dir().File("settings.txt");
        SetTestEnvironment("SSH_CONNECTION", "");
        ExportNetLog(&f.state, "Skywarn", instance, check_ins);
        int zips = 0;
        int others = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(f.dir().File("exports")))
        {
            if (entry.is_regular_file())
            {
                (entry.path().extension() == ".zip" ? zips : others) += 1;
            }
        }
        CHECK_EQ(zips, 1);
        CHECK(others >= 2);
        CHECK(f.state.status_message.find(".zip") != std::string::npos);
        CHECK(f.state.status_message.find("scp") != std::string::npos);
        CHECK(f.state.zmodem_zip_contents.empty());
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
        CHECK(first.find("a remark longer than thirty characters, kept whole") == std::string::npos);
        // Fixed columns, whatever the data: the header and each row put a
        // column at the same position.
        std::string::size_type header_start = first.find("#  ");
        REQUIRE(header_start != std::string::npos);
        std::string header = first.substr(header_start, first.find('\n', header_start) - header_start);
        CHECK_EQ(header.find("Callsign"), std::string::size_type{4 + 2 + 8 + 2});
        CHECK_EQ(header.find("Comment"), std::string::size_type{4 + 2 + 8 + 2 + 13 + 2 + 30 + 2 + 10 + 2 + 30 + 2 + 20 +
                                                                2 + 6 + 2 + 6 + 2 + 40 + 2});
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
                // The log, .qlsession and .adi, not zipped.
                CHECK_EQ(f.state.zmodem_send_paths.size(), std::size_t{3});
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
        CHECK_EQ(ListFilesWithExtension(mine, ".adi").size(), std::size_t{1});
        // Zipped too, when ZMODEM can send it: just the .zip is sent.
        std::size_t zips = ListFilesWithExtension(mine, ".zip").size();
        CHECK_EQ(zips, std::size_t{ZmodemSendAvailable() && !NoZmodemOnThisSystem() ? 1U : 0U});
        if (zips == 1)
        {
            REQUIRE(f.state.zmodem_send_paths.size() == 1);
            CHECK(f.state.zmodem_send_paths[0].find(".zip") != std::string::npos);
            // Once ZMODEM is done with it (here, skipped), the .zip goes;
            // the files in it stay, and the message names them.
            CancelZmodemAction(&f.state);
            CHECK(ListFilesWithExtension(mine, ".zip").empty());
            CHECK_EQ(ListFilesWithExtension(mine, ".adi").size(), std::size_t{1});
            CHECK(f.state.status_message.find(".qlsession") != std::string::npos);
            CHECK(f.state.status_message.find(".zip") == std::string::npos);
            CHECK(f.state.zmodem_zip_contents.empty());
            zips = 0;
        }
        f.state.show_zmodem_confirm_modal = false;
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
        // Wes's log, .qlsession, .adi (and .zip), and both users' received
        // files.
        CHECK_EQ(RemoveOldSshUserFiles(f.state.db_path, -60), static_cast<int>(5 + zips));
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
            REQUIRE(WriteNetSliceFile(ImportsDir(f.state.db_path) + "/their.qlnet", GatherNetSlice(&other, net_id),
                                      &error));
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

    QL_TEST(ANetFileNamedLikeOneHereCanOnlyBeMerged)
    {
        Fixture f;
        {
            Database other(f.dir().File("other.db"));
            std::int64_t net_id = AddTestNet(&other, "TAG SKYWARN");
            std::string error;
            REQUIRE(
                WriteNetSliceFile(ImportsDir(f.state.db_path) + "/tag.qlnet", GatherNetSlice(&other, net_id), &error));
        }
        AddTestNet(f.db(), "TAG Skywarn");
        RefreshNets(&f.state);
        RefreshImportNetFiles(&f.state);
        REQUIRE(f.state.import_net_files.size() == 1);

        // Never as a new net ("TAG SKYWARN" is the same name): only merged,
        // into the net with that name.
        ImportSelectedNetSlice(&f.state);
        CHECK(f.state.merge_stage == MergeStage::kChooseNet);
        CHECK(f.state.merge_name_taken);
        REQUIRE(f.state.merge_candidates.size() == 1);
        CHECK_EQ(f.state.merge_candidates[0].name, std::string("TAG Skywarn"));
        ImportSelectedNetSliceAnyway(&f.state);
        CHECK_EQ(f.db()->GetAllNets().size(), std::size_t{1});
    }

    QL_TEST(ImportingANetFileLikeANetAlreadyHereAsksFirst)
    {
        Fixture f;
        {
            Database other(f.dir().File("other.db"));
            std::int64_t net_id = AddTestNet(&other, "Hamilton County ARES");
            std::string error;
            REQUIRE(
                WriteNetSliceFile(ImportsDir(f.state.db_path) + "/ares.qlnet", GatherNetSlice(&other, net_id), &error));
        }
        AddTestNet(f.db(), "Hamilton Co. ARES Net");
        AddTestNet(f.db(), "TAG Skywarn");
        RefreshNets(&f.state);
        RefreshImportNetFiles(&f.state);
        REQUIRE(f.state.import_net_files.size() == 1);

        // Asked first, offering only the net that looks like it; Esc imports
        // nothing.
        ImportSelectedNetSlice(&f.state);
        CHECK(f.state.merge_stage == MergeStage::kChooseNet);
        CHECK(f.state.show_merge_modal);
        CHECK(!f.state.merge_name_taken);
        REQUIRE(f.state.merge_candidates.size() == 1);
        CHECK_EQ(f.state.merge_candidates[0].name, std::string("Hamilton Co. ARES Net"));
        AppKeyHandler keys(&f.state);
        f.state.page = kPageImportNet;
        CHECK(keys(ftxui::Event::Escape));
        CHECK(f.state.merge_stage == MergeStage::kNone);
        CHECK_EQ(f.state.nets.size(), std::size_t{2});

        // Imported as a new net anyway.
        ImportSelectedNetSlice(&f.state);
        CHECK(keys(ftxui::Event::F2));
        CHECK(f.state.merge_stage == MergeStage::kNone);
        CHECK_EQ(f.state.nets.size(), std::size_t{3});
        CHECK_EQ(f.state.page, kPageNetList);
    }

    QL_TEST(ANetFileCanBeMergedIntoTheNetItCameFrom)
    {
        Fixture f;
        std::int64_t net_id = f.StartNet("TAG Skywarn");
        f.Log("K4AAA");
        CloseActiveNet(&f.state);
        // The same net, logged elsewhere: its session, plus one more.
        std::string file = ImportsDir(f.state.db_path) + "/skywarn.qlnet";
        {
            std::string error;
            NetSlice slice = GatherNetSlice(f.db(), net_id);
            Database other(f.dir().File("other.db"));
            std::int64_t other_net = ApplyNetSlice(&other, slice, 0);
            std::int64_t later = AddTestInstance(&other, other_net, "2026-10-01", 1000 + 8 * 24 * 3600, "W4KWK");
            AddTestCheckIn(&other, later, "K4ZZZ", 1);
            REQUIRE(WriteNetSliceFile(file, GatherNetSlice(&other, other_net), &error));
        }
        RefreshNets(&f.state);
        RefreshImportNetFiles(&f.state);
        f.state.page = kPageImportNet;
        AppKeyHandler keys(&f.state);

        // The same name: it can only be merged.
        ImportSelectedNetSlice(&f.state);
        REQUIRE(f.state.merge_stage == MergeStage::kChooseNet);
        CHECK(f.state.merge_name_taken);
        CHECK(keys(ftxui::Event::F2));  // Import New isn't offered.
        CHECK(f.state.merge_stage == MergeStage::kChooseNet);
        CHECK_EQ(f.state.nets.size(), std::size_t{1});

        // F3: the summary. Esc goes back to choosing.
        CHECK(keys(ftxui::Event::F3));
        REQUIRE(f.state.merge_stage == MergeStage::kSummary);
        CHECK_EQ(f.state.merge_target_name, std::string("TAG Skywarn"));
        CHECK(f.state.merge_conflicts.empty());
        CHECK(keys(ftxui::Event::Escape));
        CHECK(f.state.merge_stage == MergeStage::kChooseNet);
        CHECK(keys(ftxui::Event::F3));

        // F2 merges: one session added, nothing else changed.
        CHECK(keys(ftxui::Event::F2));
        CHECK(f.state.merge_stage == MergeStage::kNone);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK_EQ(f.state.nets.size(), std::size_t{1});
        CHECK_EQ(f.db()->GetNetInstancesForNet(net_id).size(), std::size_t{2});
        CHECK(f.state.status_message.find("Merged into TAG Skywarn: 1 session") == 0);
    }

    // ---- Two operators sharing one weekly net, syncing by .qlnet --------------

    // Tuesday 2026-09-01, 8 PM Eastern, and a week.
    static constexpr std::int64_t kFirstTuesday = 1788307200;
    static constexpr std::int64_t kWeek = 7 * 24 * 60 * 60;

    // Logs week `week` (0 = 2026-09-01) of net `net_id`: 8 PM to 8:45 PM,
    // closed, with `operator_callsign` first and then `stations`.
    static void LogTuesday(Database* db, std::int64_t net_id, int week, const std::string& operator_callsign,
                           const std::vector<std::string>& stations)
    {
        static const char* const kDates[] = {"2026-09-01", "2026-09-08", "2026-09-15",
                                             "2026-09-22", "2026-09-29", "2026-10-06"};
        std::int64_t start = kFirstTuesday + week * kWeek;
        std::int64_t session = AddTestInstance(db, net_id, kDates[week], start, operator_callsign);
        AddTestCheckIn(db, session, operator_callsign, 1);
        for (std::size_t i = 0; i < stations.size(); ++i)
        {
            AddTestCheckIn(db, session, stations[i], static_cast<int>(i) + 2);
        }
        db->CloseNetInstance(session, start + 45 * 60);
    }

    // Exports `from`'s net `net_id` to `to`'s imports folder, then imports it
    // there through the Import page the way the operator would: the same
    // name, so the Import or Merge window offers only Merge (F3), then the
    // summary, then F2. Leaves `to` on the summary's result; returns the
    // plan the summary showed, and its rows for the sessions that differ in
    // `conflict_rows` if given.
    static NetMergePlan SyncNet(Database* from, std::int64_t net_id, Fixture* to, bool replace_stations = false,
                                std::vector<MergeConflictText>* conflict_rows = nullptr)
    {
        std::string error;
        std::string file = ImportsDir(to->state.db_path) + "/Tuesday_Night_Net.qlnet";
        REQUIRE(WriteNetSliceFile(file, GatherNetSlice(from, net_id), &error));
        RefreshNets(&to->state);
        RefreshImportNetFiles(&to->state);
        to->state.selected_import_file_index = 0;
        to->state.page = kPageImportNet;
        AppKeyHandler keys(&to->state);

        ImportSelectedNetSlice(&to->state);
        REQUIRE(to->state.merge_stage == MergeStage::kChooseNet);
        CHECK(to->state.merge_name_taken);  // The very same name: merge only.
        REQUIRE(to->state.merge_candidates.size() == 1);
        CHECK_EQ(to->state.merge_candidates[0].name, std::string("Tuesday Night Net"));
        CHECK(keys(ftxui::Event::F3));
        REQUIRE(to->state.merge_stage == MergeStage::kSummary);
        // Each session that differs has its row's text, made with the plan:
        // the file's date and start, and what differs.
        REQUIRE(to->state.merge_conflict_texts.size() == to->state.merge_conflicts.size());
        for (std::size_t i = 0; i < to->state.merge_conflicts.size(); ++i)
        {
            const MergeSession& session = to->state.merge_plan.sessions[to->state.merge_conflicts[i]];
            const NetInstance& file = to->state.merge_slice.instances[session.file_index];
            const MergeConflictText& text = to->state.merge_conflict_texts[i];
            CHECK_EQ(text.when, file.instance_date + "  " + FormatLocalTimeOfDay(file.started_at));
            CHECK(!text.what.empty());
        }
        if (conflict_rows != nullptr)
        {
            *conflict_rows = to->state.merge_conflict_texts;
        }
        // To replace a station's details: Down to its row (after any
        // sessions that differ), then Right.
        if (replace_stations)
        {
            int sessions = static_cast<int>(to->state.merge_conflicts.size());
            for (std::size_t i = 0; i < to->state.merge_plan.station_conflicts.size(); ++i)
            {
                while (to->state.selected_merge_conflict < sessions + static_cast<int>(i))
                {
                    CHECK(keys(ftxui::Event::ArrowDown));
                }
                CHECK(keys(ftxui::Event::ArrowRight));
            }
        }
        // A copy without the pointers into the file, which goes with the
        // merge: what the summary showed, for the test to check.
        NetMergePlan plan = to->state.merge_plan;
        for (MergeStationConflict& conflict : plan.station_conflicts)
        {
            conflict.file_station = nullptr;
        }
        CHECK(keys(ftxui::Event::F2));
        CHECK(to->state.merge_stage == MergeStage::kNone);
        std::filesystem::remove(file);
        return plan;
    }

    static int Count(const NetMergePlan& plan, MergeSessionKind kind)
    {
        int count = 0;
        for (const MergeSession& session : plan.sessions)
        {
            count += session.kind == kind ? 1 : 0;
        }
        return count;
    }

    // The net's sessions as "date:check-ins" and its saved stations as
    // "callsign:member id:remarks", sorted, for comparing two machines.
    static std::vector<std::string> Sessions(Database* db, std::int64_t net_id)
    {
        std::vector<std::string> sessions;
        for (const NetInstance& session : db->GetNetInstancesForNet(net_id))
        {
            sessions.push_back(session.instance_date + ":" +
                               std::to_string(db->GetCheckInsForNetInstance(session.id).size()));
        }
        std::sort(sessions.begin(), sessions.end());
        return sessions;
    }

    static std::vector<std::string> SavedStations(Database* db, std::int64_t net_id)
    {
        std::vector<std::string> saved;
        for (const Station& station : db->GetSavedStationsForNet(net_id))
        {
            saved.push_back(station.callsign + ":" + station.member_id + ":" +
                            db->GetSavedNetStationRemarks(net_id, station.callsign));
        }
        std::sort(saved.begin(), saved.end());
        return saved;
    }

    // K4BTH, a station both operators saved: not at all, with a member ID
    // on B's machine only, or with different member IDs on each.
    enum class SharedStation
    {
        kNone,
        kMemberIdOnBOnly,
        kDifferentMemberIds,
    };

    // Two operators share the Tuesday net from the same start (one exported
    // it, the other imported it), alternate weeks logging it, and each saves
    // stations of their own, and perhaps K4BTH (see SharedStation).
    struct AlternatingWeeks
    {
        Fixture a;
        Fixture b;
        std::int64_t a_net = 0;
        std::int64_t b_net = 0;

        explicit AlternatingWeeks(SharedStation shared)
        {
            Net net;
            net.name = "Tuesday Night Net";
            net.mode = "FM";
            net.default_frequency = "146.940";
            a_net = a.db()->CreateNet(net);
            a.db()->SaveNetStation(a_net, MakeStation("K4OLD", "Olive Old"), "regular", 1);
            LogTuesday(a.db(), a_net, 0, "W4KWK", {"K4OLD"});
            std::string error;
            b_net = ApplyNetSlice(b.db(), GatherNetSlice(a.db(), a_net), 1);

            // Weeks 1, 3, 5 on B's machine; 2, 4 on A's.
            LogTuesday(b.db(), b_net, 1, "N4BBB", {"K4OLD", "K4BEE"});
            LogTuesday(a.db(), a_net, 2, "W4KWK", {"K4OLD", "K4AAY"});
            LogTuesday(b.db(), b_net, 3, "N4BBB", {"K4BEE"});
            LogTuesday(a.db(), a_net, 4, "W4KWK", {"K4AAY", "K4OLD"});
            LogTuesday(b.db(), b_net, 5, "N4BBB", {"K4OLD"});
            // Saved stations each picked up along the way.
            a.db()->SaveNetStation(a_net, MakeStation("K4AAY", "Amy Aye"), "mobile", 2);
            a.db()->SaveNetStation(a_net, MakeStation("K4AAZ", "Al Zed"), "", 2);
            b.db()->SaveNetStation(b_net, MakeStation("K4BEE", "Bea Bee"), "base", 2);
            if (shared != SharedStation::kNone)
            {
                Station on_a = MakeStation("K4BTH", "Beth Both");
                if (shared == SharedStation::kDifferentMemberIds)
                {
                    on_a.member_id = "SP-41";
                }
                a.db()->SaveNetStation(a_net, on_a, "on A", 2);
                Station on_b = MakeStation("K4BTH", "Beth Both");
                on_b.member_id = "SP-42";
                b.db()->SaveNetStation(b_net, on_b, "on B", 2);
            }
        }
    };

    QL_TEST(OperatorsAlternatingWeeksSyncWithNothingInConflict)
    {
        AlternatingWeeks net(SharedStation::kNone);

        // B sends theirs to A: B's three weeks are new, the shared first
        // week is already there, nothing differs; B's one saved station is
        // new, and the one they share is known.
        NetMergePlan to_a = SyncNet(net.b.db(), net.b_net, &net.a);
        CHECK_EQ(to_a.sessions.size(), std::size_t{4});
        CHECK_EQ(Count(to_a, MergeSessionKind::kNew), 3);
        CHECK_EQ(Count(to_a, MergeSessionKind::kAlreadyHere), 1);
        CHECK_EQ(Count(to_a, MergeSessionKind::kDiffers), 0);
        CHECK(to_a.station_conflicts.empty());
        CHECK_EQ(to_a.new_saved_stations, 1);
        CHECK_EQ(to_a.known_saved_stations, 1);
        CHECK(net.a.state.status_message.find("3 sessions and 1 saved station added") != std::string::npos);

        // A sends everything back: A's two weeks are new to B.
        NetMergePlan to_b = SyncNet(net.a.db(), net.a_net, &net.b);
        CHECK_EQ(Count(to_b, MergeSessionKind::kNew), 2);
        CHECK_EQ(Count(to_b, MergeSessionKind::kAlreadyHere), 4);
        CHECK_EQ(Count(to_b, MergeSessionKind::kDiffers), 0);
        CHECK_EQ(to_b.new_saved_stations, 2);

        // Both machines now have the same six Tuesdays and saved stations.
        std::vector<std::string> sessions = Sessions(net.a.db(), net.a_net);
        CHECK_EQ(sessions.size(), std::size_t{6});
        CHECK(sessions == Sessions(net.b.db(), net.b_net));
        CHECK(SavedStations(net.a.db(), net.a_net) == SavedStations(net.b.db(), net.b_net));
        CHECK_EQ(SavedStations(net.a.db(), net.a_net).size(), std::size_t{4});
        // Each session's check-ins came with it.
        CHECK_EQ(sessions[1], std::string("2026-09-08:3"));

        // Syncing again finds nothing to do, either way.
        NetMergePlan again = SyncNet(net.b.db(), net.b_net, &net.a);
        CHECK_EQ(Count(again, MergeSessionKind::kNew), 0);
        CHECK_EQ(Count(again, MergeSessionKind::kDiffers), 0);
        CHECK_EQ(again.new_saved_stations, 0);
        CHECK_EQ(Sessions(net.a.db(), net.a_net).size(), std::size_t{6});
    }

    QL_TEST(OperatorsAlternatingWeeksFillInEachOthersMissingMemberId)
    {
        AlternatingWeeks net(SharedStation::kMemberIdOnBOnly);

        // K4BTH is saved on both machines: known, not new, and not a
        // session conflict (saved stations are never asked about).
        NetMergePlan to_a = SyncNet(net.b.db(), net.b_net, &net.a);
        CHECK_EQ(Count(to_a, MergeSessionKind::kNew), 3);
        CHECK_EQ(Count(to_a, MergeSessionKind::kDiffers), 0);
        CHECK_EQ(to_a.new_saved_stations, 1);
        CHECK_EQ(to_a.known_saved_stations, 2);
        // A blank on one side isn't a conflict: it's just filled in.
        CHECK(to_a.station_conflicts.empty());
        // A's copy gains the member ID it lacked, and keeps its own remarks.
        std::optional<Station> on_a = net.a.db()->FindStationByCallsign("K4BTH");
        REQUIRE(on_a.has_value());
        CHECK_EQ(on_a->member_id, std::string("SP-42"));
        CHECK_EQ(on_a->name, std::string("Beth Both"));
        CHECK_EQ(net.a.db()->GetSavedNetStationRemarks(net.a_net, "K4BTH"), std::string("on A"));

        // Back to B: B keeps its member ID (a blank on A never erases it)
        // and its own remarks.
        SyncNet(net.a.db(), net.a_net, &net.b);
        std::optional<Station> on_b = net.b.db()->FindStationByCallsign("K4BTH");
        REQUIRE(on_b.has_value());
        CHECK_EQ(on_b->member_id, std::string("SP-42"));
        CHECK_EQ(net.b.db()->GetSavedNetStationRemarks(net.b_net, "K4BTH"), std::string("on B"));

        // Everything else matches; only each machine's own remarks for
        // K4BTH differ, by design.
        CHECK(Sessions(net.a.db(), net.a_net) == Sessions(net.b.db(), net.b_net));
        std::vector<std::string> a_saved = SavedStations(net.a.db(), net.a_net);
        std::vector<std::string> b_saved = SavedStations(net.b.db(), net.b_net);
        REQUIRE(a_saved.size() == 5);
        REQUIRE(b_saved.size() == 5);
        for (std::size_t i = 0; i < a_saved.size(); ++i)
        {
            if (a_saved[i].rfind("K4BTH", 0) == 0)
            {
                CHECK_EQ(a_saved[i], std::string("K4BTH:SP-42:on A"));
                CHECK_EQ(b_saved[i], std::string("K4BTH:SP-42:on B"));
            }
            else
            {
                CHECK_EQ(a_saved[i], b_saved[i]);
            }
        }
    }

    QL_TEST(OperatorsWithDifferentMemberIdsChooseWhichToKeep)
    {
        AlternatingWeeks net(SharedStation::kDifferentMemberIds);

        // B sends to A: K4BTH's member ID is listed as differing, the
        // sessions still merge cleanly, and Keep (the default) keeps A's.
        NetMergePlan kept = SyncNet(net.b.db(), net.b_net, &net.a);
        CHECK_EQ(Count(kept, MergeSessionKind::kNew), 3);
        CHECK_EQ(Count(kept, MergeSessionKind::kDiffers), 0);
        REQUIRE(kept.station_conflicts.size() == 1);
        const MergeStationConflict& conflict = kept.station_conflicts[0];
        REQUIRE(conflict.differences.size() == 1);
        CHECK_EQ(std::string(conflict.differences[0].field), std::string("member ID"));
        CHECK_EQ(conflict.differences[0].here, std::string("SP-41"));
        CHECK_EQ(conflict.differences[0].file, std::string("SP-42"));
        CHECK(!conflict.replace);
        CHECK_EQ(net.a.db()->FindStationByCallsign("K4BTH")->member_id, std::string("SP-41"));

        // Sent again, A chooses Replace: K4BTH takes B's member ID, keeping
        // A's remarks; nothing else changes.
        NetMergePlan replaced = SyncNet(net.b.db(), net.b_net, &net.a, true);
        REQUIRE(replaced.station_conflicts.size() == 1);
        CHECK(replaced.station_conflicts[0].replace);
        CHECK_EQ(Count(replaced, MergeSessionKind::kNew), 0);
        std::optional<Station> on_a = net.a.db()->FindStationByCallsign("K4BTH");
        REQUIRE(on_a.has_value());
        CHECK_EQ(on_a->member_id, std::string("SP-42"));
        CHECK_EQ(on_a->name, std::string("Beth Both"));
        CHECK_EQ(net.a.db()->GetSavedNetStationRemarks(net.a_net, "K4BTH"), std::string("on A"));
        CHECK(net.a.state.status_message.find("1 station's details taken from the file") != std::string::npos);

        // Now they agree: syncing back to B finds no station conflict.
        NetMergePlan to_b = SyncNet(net.a.db(), net.a_net, &net.b);
        CHECK(to_b.station_conflicts.empty());
        CHECK(Sessions(net.a.db(), net.a_net) == Sessions(net.b.db(), net.b_net));
    }

    QL_TEST(ASessionLoggedDifferentlyOnEachMachineIsListedWithWhatDiffers)
    {
        AlternatingWeeks net(SharedStation::kNone);
        // The first week, which both have: B adds K4NEW to theirs.
        std::int64_t b_first = 0;
        for (const NetInstance& session : net.b.db()->GetNetInstancesForNet(net.b_net))
        {
            if (session.instance_date == "2026-09-01")
            {
                b_first = session.id;
            }
        }
        REQUIRE(b_first != 0);
        AddTestCheckIn(net.b.db(), b_first, "K4NEW", 3);

        // Its row says when it was and what differs, made with the plan.
        std::vector<MergeConflictText> rows;
        NetMergePlan to_a = SyncNet(net.b.db(), net.b_net, &net.a, false, &rows);
        CHECK_EQ(Count(to_a, MergeSessionKind::kDiffers), 1);
        REQUIRE(rows.size() == 1);
        CHECK_EQ(rows[0].when, "2026-09-01  " + FormatLocalTimeOfDay(kFirstTuesday));
        CHECK_EQ(rows[0].what, std::string("check-ins: K4NEW only in file"));
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
        std::vector<std::string> files = ListFilesWithExtension(f.dir().File("exports"), ".qlsession");
        REQUIRE(files.size() == 1);
        CHECK_EQ(files[0], std::string("Skywarn_2026-09-24.qlsession"));

        // It holds the session exactly, with its stations' details.
        std::string error;
        std::optional<NetSlice> slice = ReadSessionSliceFile(f.dir().File("exports/" + files[0]), &error);
        REQUIRE(slice.has_value());
        CHECK_EQ(slice->net.name, std::string("Skywarn"));
        REQUIRE(slice->instances.size() == 1);
        CHECK_EQ(slice->instances[0].started_at, std::int64_t{1000});
        CHECK_EQ(slice->check_ins.size(), std::size_t{2});
        bool has_ann = false;
        for (const Station& station : slice->other_stations)
        {
            has_ann = has_ann || (station.callsign == "K4AAA" && station.street_address == "1 Main St" &&
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
        REQUIRE(WriteNetSliceFile(ImportsDir(f.state.db_path) + "/" + file_name, GatherSessionSlice(&other, session),
                                  &error));
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
        CHECK(f.state.confirm_prompt_lines[0].find("\"Hamilton County ARES\"") != std::string::npos);
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
        CHECK(f.state.status_message.find("logged as \"Hamilton County ARES\"") != std::string::npos);
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
            REQUIRE(WriteNetSliceFile(ImportsDir(f.state.db_path) + "/their.qlnet", GatherNetSlice(&other, other_net),
                                      &error));
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
        CHECK_EQ(ListFilesWithExtension(f.dir().File("exports"), ".qlsession").size(), std::size_t{1});
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
            REQUIRE(WriteNetSliceFile(f.dir().File("their.qlsession"), GatherNetSlice(&other, net_id), &error));
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

        LoadSavedStationIntoForm(&f.state, 0);
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
        AddPastSession(f.db(), net_id, FormatLocalDate(static_cast<std::int64_t>(std::time(nullptr))), {"K4NOW"});
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
            found_extra = found_extra || (row.find("F8") == 0 && row.find(" *") != std::string::npos);
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
        f.db()->BulkUpsertUlsStations({MakeStation("K4AAA", "ANN", "37415", "Chattanooga")}, 0, 1, 1);
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
        CHECK_EQ(f.state.form_error, std::string("K4AAA/M is already in this session's log, as K4AAA #2."));
        CHECK(!f.Log("VE3/K4AAA"));
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(f.state.active_instance.id).size(), std::size_t{2});
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
