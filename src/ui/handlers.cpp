#include "handlers.hpp"

#include <cctype>
#include <cstddef>
#include <ctime>
#include <exception>
#include <optional>
#include <utility>

#include <ftxui/screen/terminal.hpp>

#include "../date_utils.hpp"
#include "../mode_rules.hpp"
#include "../text_utils.hpp"
#include "../uls_import.hpp"
#include "chrome.hpp"
#include "mouse.hpp"

namespace ql
{

    void UppercaseFieldHandler::operator()() const
    {
        *field_ = NormalizeCallsign(*field_);
    }

    // Keeps only the digits in `*field`, at most `max_digits` of them.
    static void KeepDigits(std::string* field, std::size_t max_digits)
    {
        std::string digits_only;
        for (char c : *field)
        {
            if (std::isdigit(static_cast<unsigned char>(c)))
            {
                digits_only.push_back(c);
            }
        }
        if (digits_only.size() > max_digits)
        {
            digits_only.resize(max_digits);
        }
        *field = digits_only;
    }

    void ZipCodeFieldHandler::operator()() const
    {
        // Starting with a letter, it's a Canadian postal code: letters and
        // digits, upper-cased, at most six.
        if (!field_->empty() && std::isalpha(static_cast<unsigned char>((*field_)[0])))
        {
            std::string kept;
            for (char c : *field_)
            {
                if (std::isalnum(static_cast<unsigned char>(c)))
                {
                    kept.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                }
            }
            if (kept.size() > 6)
            {
                kept.resize(6);
            }
            *field_ = kept;
            return;
        }
        KeepDigits(field_, 5);
    }

    void DigitsFieldHandler::operator()() const
    {
        KeepDigits(field_, max_digits_);
    }

    void FrequencyFieldHandler::operator()() const
    {
        std::string kept;
        bool seen_point = false;
        for (char c : *field_)
        {
            if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && !seen_point))
            {
                seen_point = seen_point || c == '.';
                kept.push_back(c);
            }
        }
        if (kept.size() > 14)
        {
            kept.resize(14);
        }
        *field_ = kept;
    }

    void OffsetFieldHandler::operator()() const
    {
        std::string kept;
        bool seen_point = false;
        for (char c : *field_)
        {
            bool sign = (c == '+' || c == '-') && kept.empty();
            bool point = c == '.' && !seen_point;
            if (std::isdigit(static_cast<unsigned char>(c)) || sign || point)
            {
                seen_point = seen_point || point;
                kept.push_back(c);
            }
        }
        if (kept.size() > 12)
        {
            kept.resize(12);
        }
        *field_ = kept;
    }

    void ShowCreateNetPageHandler::operator()() const
    {
        if (RefuseViewOnly(state_, "create nets"))
        {
            return;
        }
        ResetCreateNetForm(state_);
        state_->page = kPageCreateNet;
        if (state_->new_net_name_input)
        {
            state_->new_net_name_input->TakeFocus();
        }
    }

    void CreateNetSubmitHandler::operator()() const
    {
        if (state_->new_net_name.empty())
        {
            state_->form_error = "Net name is required.";
            return;
        }
        std::string taken = ExistingNetNamed(state_, state_->new_net_name);
        if (!taken.empty())
        {
            state_->form_error = "You already have a net named \"" + taken + "\".";
            return;
        }
        Net net;
        if (!ReadNewNetRadio(state_, &net) || !CheckNetZip(state_, state_->new_net_location))
        {
            return;
        }
        net.name = state_->new_net_name;
        net.default_location = NormalizeZipOrPostalCode(state_->new_net_location);
        net.recurrence_description = state_->new_net_recurrence;
        net.comments = state_->new_net_comments;
        net.created_at = static_cast<std::int64_t>(std::time(nullptr));
        state_->db->CreateNet(net);

        RefreshNets(state_);
        ResetCreateNetForm(state_);
        state_->page = kPageNetList;
    }

    void CreateNetCancelHandler::operator()() const
    {
        ResetCreateNetForm(state_);
        state_->page = kPageNetList;
    }

    bool CreateNetKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (event == ftxui::Event::F2)
        {
            CreateNetSubmitHandler submit(state_);
            submit();
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            CreateNetCancelHandler cancel(state_);
            cancel();
            return true;
        }
        return false;
    }

    void StartSelectedNetHandler::operator()() const
    {
        StartSelectedNet(state_);
    }

    void ShowAdHocNetPageHandler::operator()() const
    {
        ResetCreateNetForm(state_);
        state_->status_message.clear();
        RefreshOpenAdHocSessions(state_);
        state_->page = kPageAdHocNet;
        if (state_->ad_hoc_net_name_input)
        {
            state_->ad_hoc_net_name_input->TakeFocus();
        }
    }

    void AdHocNetSubmitHandler::operator()() const
    {
        StartAdHocNet(state_);
    }

    bool AdHocNetKeyHandler::operator()(const ftxui::Event& event) const
    {
        // A view-only user can't start one; F3 views an open one.
        if (state_->view_only_user && event == ftxui::Event::F2)
        {
            return true;
        }
        if (event == ftxui::Event::F2)
        {
            AdHocNetSubmitHandler submit(state_);
            submit();
            return true;
        }
        if (event == ftxui::Event::F3)
        {
            StartRowPick(
                state_, state_->view_only_user ? RowPickAction::kViewAdHocSession : RowPickAction::kResumeAdHocSession);
            return true;
        }
        if (event == ftxui::Event::F6)
        {
            // Opens on the newest session, not the row another net's
            // History was left on.
            state_->history_ad_hoc = true;
            state_->selected_history_index = 0;
            state_->form_error.clear();
            state_->status_message.clear();
            RefreshNetHistory(state_);
            state_->page = kPageNetHistory;
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            CreateNetCancelHandler cancel(state_);
            cancel();
            return true;
        }
        return false;
    }

    void ViewNetHistoryHandler::operator()() const
    {
        if (state_->nets.empty())
        {
            state_->form_error = "Create a recurring net first.";
            return;
        }
        // Opens on the newest session, not the row another net's History was
        // left on.
        state_->history_ad_hoc = false;
        state_->selected_history_index = 0;
        RefreshNetHistory(state_);
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageNetHistory;
    }

    void HistoryInstanceChangedHandler::operator()() const
    {
        ShowHistoryCheckIns(state_);
    }

    void NetHistoryBackHandler::operator()() const
    {
        if (state_->history_ad_hoc)
        {
            ShowAdHocNetPageHandler show_adhoc(state_);
            show_adhoc();
            return;
        }
        state_->page = kPageNetList;
    }

    void ExportNetHistoryLogHandler::operator()() const
    {
        if (state_->selected_history_index >= static_cast<int>(state_->history_instances.size()))
        {
            state_->status_message.clear();
            state_->form_error = "No net instance to export.";
            return;
        }

        const NetInstance& instance = state_->history_instances[state_->selected_history_index];
        std::vector<CheckIn> check_ins = state_->db->GetCheckInsForNetInstance(instance.id);
        std::optional<Net> net = state_->db->GetNetById(instance.net_id);
        ExportNetLog(state_, net.has_value() ? net->name : "", instance, check_ins);
    }

    bool NetHistoryKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (state_->show_zmodem_confirm_modal)
        {
            // After F7 Export of a closed session, with an upstream set.
            if (event == ftxui::Event::F3 && ExportOffersPush(state_))
            {
                PushExport(state_);
                return true;
            }
            if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
            {
                ConfirmZmodemActionHandler confirm(state_);
                confirm();
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CancelZmodemActionHandler cancel(state_);
                cancel();
                return true;
            }
            return true;
        }

        if (event == ftxui::Event::Escape)
        {
            NetHistoryBackHandler back(state_);
            back();
            return true;
        }
        if (event == ftxui::Event::F8)
        {
            OpenNetStatistics(state_);
            return true;
        }
        if (event == ftxui::Event::F9)
        {
            OpenStationSearch(state_);
            return true;
        }
        if (event == ftxui::Event::F7)
        {
            ExportNetHistoryLogHandler export_log(state_);
            export_log();
            return true;
        }
        if (event == ftxui::Event::F12)
        {
            OpenHistorySessionNotes(state_);
            return true;
        }
        // Nothing in History can be deleted or imported by a view-only
        // user.
        if (state_->view_only_user &&
            (event == ftxui::Event::F4 || event == ftxui::Event::F5 || event == ftxui::Event::F6))
        {
            return true;
        }
        if (event == ftxui::Event::F6)
        {
            OpenSessionImport(state_);
            return true;
        }
        // F5 deletes a check-in, as on the active net.
        if (event == ftxui::Event::F5)
        {
            StartRowPick(state_, RowPickAction::kDeleteHistoryCheckIn);
            return true;
        }
        if (event == ftxui::Event::F4)
        {
            StartRowPick(state_, RowPickAction::kDeleteNetInstance);
            return true;
        }
        return false;
    }

    void SaveEditNetHandler::operator()() const
    {
        SaveEditNetForm(state_);
    }

    // True if nothing at all has been typed into the saved-station form.
    static bool SavedStationFormIsBlank(const AppState* state)
    {
        const Station& station = state->saved_station;
        return station.callsign.empty() && station.name.empty() && station.member_id.empty() &&
               station.street_address.empty() && station.city.empty() && station.county.empty() &&
               station.state.empty() && station.zip.empty() && station.grid_square.empty() &&
               state->saved_station_remarks.empty();
    }

    void SaveNetStationFormHandler::operator()() const
    {
        if (state_->saved_station.callsign.empty())
        {
            if (close_after_ && SavedStationFormIsBlank(state_))
            {
                CloseSavedStationForm(state_);
                return;
            }
            // Only say something if other fields were filled in and just the
            // callsign left out; either way, the cursor goes to it.
            state_->form_error =
                SavedStationFormIsBlank(state_) ? std::string() : std::string("Enter a callsign to save this station.");
            if (state_->saved_station_callsign_input)
            {
                state_->saved_station_callsign_input->TakeFocus();
            }
            return;
        }
        if (SaveNetStationForm(state_) && close_after_)
        {
            CloseSavedStationForm(state_);
        }
    }

    void ExportSavedStationsHandler::operator()() const
    {
        ExportSavedStations(state_, state_->edit_net_name);
    }

    void InfoQueryChangeHandler::operator()() const
    {
        RefreshStationSearch(state_);
    }

    void SavedStationCallsignChangeHandler::operator()() const
    {
        state_->saved_station.callsign = NormalizeCallsign(state_->saved_station.callsign);
        RefreshSavedStationSuggestions(state_);
    }

    void SavedStationCallsignEnterHandler::operator()() const
    {
        ApplySelectedSavedStationSuggestion(state_);
    }

    void LoadSavedStationHandler::operator()() const
    {
        if (state_->edit_net_saved_stations.empty())
        {
            return;
        }
        LoadSavedStationIntoForm(state_, static_cast<std::size_t>(state_->selected_saved_station_index));
    }

    void AddNewSavedStationHandler::operator()() const
    {
        OpenNewSavedStationForm(state_);
    }

    void EditNetBackHandler::operator()() const
    {
        state_->form_error.clear();
        state_->page = kPageNetList;
    }

    // Up/Down while typing in a callsign field that has autocomplete matches
    // showing: moves the highlighted match (the one Enter picks) without
    // leaving the field, so the operator can keep typing or choose. Returns
    // whether it handled the event. Does nothing -- leaving Up/Down to move
    // between fields as usual -- when the field isn't focused or there are
    // no matches.
    static bool MoveSuggestionHighlight(const ftxui::Event& event, const ftxui::Component& callsign_input,
                                        std::size_t suggestion_count, int* selected_index)
    {
        if (suggestion_count == 0 || !callsign_input || !callsign_input->Focused())
        {
            return false;
        }
        int last = static_cast<int>(suggestion_count) - 1;
        if (event == ftxui::Event::ArrowDown)
        {
            *selected_index = *selected_index < last ? *selected_index + 1 : last;
            return true;
        }
        if (event == ftxui::Event::ArrowUp)
        {
            *selected_index = *selected_index > 0 ? *selected_index - 1 : 0;
            return true;
        }
        return false;
    }

    bool EditNetKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (state_->show_zmodem_confirm_modal)
        {
            if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
            {
                ConfirmZmodemActionHandler confirm(state_);
                confirm();
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CancelZmodemActionHandler cancel(state_);
                cancel();
                return true;
            }
            return true;
        }
        if (state_->show_delete_net_confirm_modal)
        {
            if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
            {
                ConfirmDeleteNetHandler confirm(state_);
                confirm();
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CancelDeleteNetHandler cancel(state_);
                cancel();
                return true;
            }
            return true;
        }

        if (state_->show_saved_station_modal)
        {
            bool leaving_callsign = event == ftxui::Event::Tab || event == ftxui::Event::TabReverse ||
                                    event == ftxui::Event::F2 || event == ftxui::Event::F3;
            if (leaving_callsign && state_->saved_station_callsign_input &&
                state_->saved_station_callsign_input->Focused())
            {
                // The match marked ">", as in the New Check-In window; with
                // no matches, what's known about the callsign typed.
                ApplySelectedSavedStationSuggestion(state_);
            }
            if (MoveSuggestionHighlight(event, state_->saved_station_callsign_input,
                                        state_->saved_station_suggestions.size(),
                                        &state_->selected_saved_station_suggestion_index))
            {
                return true;
            }
            if (event == ftxui::Event::F2 || event == ftxui::Event::F3)
            {
                SaveNetStationFormHandler save_station(state_, event == ftxui::Event::F3);
                save_station();
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CloseSavedStationForm(state_);
                return true;
            }
            // Every other key goes to the window's fields; none of the
            // page's own keys apply while it's open.
            return false;
        }

        if (event == ftxui::Event::F2)
        {
            SaveEditNetHandler save(state_);
            save();
            return true;
        }
        if (event == ftxui::Event::F4)
        {
            StartRowPick(state_, RowPickAction::kRemoveSavedStation);
            return true;
        }
        // F3 edits, as on the active net.
        if (event == ftxui::Event::F3)
        {
            StartRowPick(state_, RowPickAction::kEditSavedStation);
            return true;
        }
        if (event == ftxui::Event::F6)
        {
            AddNewSavedStationHandler add_station(state_);
            add_station();
            return true;
        }
        if (event == ftxui::Event::F7)
        {
            ExportSavedStationsHandler export_stations(state_);
            export_stations();
            return true;
        }
        if (event == ftxui::Event::F5)
        {
            OpenQuietStations(state_);
            return true;
        }
        if (event == ftxui::Event::F8)
        {
            RequestDeleteNetHandler request_delete(state_);
            request_delete();
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            EditNetBackHandler back(state_);
            back();
            return true;
        }
        return false;
    }

    void ExportNetSliceHandler::operator()() const
    {
        if (state_->selected_net_index >= static_cast<int>(state_->nets.size()))
        {
            state_->status_message.clear();
            state_->form_error = "No net selected to export.";
            return;
        }
        ExportNetSlice(state_, state_->nets[state_->selected_net_index]);
    }

    void ShowImportNetPageHandler::operator()() const
    {
        if (RefuseViewOnly(state_, "import nets"))
        {
            return;
        }
        state_->import_session = false;
        RefreshImportNetFiles(state_);
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageImportNet;
    }

    bool NetListKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (state_->show_zmodem_confirm_modal)
        {
            // After an export, with an upstream set: push what was exported.
            if (event == ftxui::Event::F3 && ExportOffersPush(state_))
            {
                PushExport(state_);
                return true;
            }
            if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
            {
                ConfirmZmodemActionHandler confirm(state_);
                confirm();
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CancelZmodemActionHandler cancel(state_);
                cancel();
                return true;
            }
            return true;
        }

        // A view-only user has no New, Edit or Import (see
        // AppState::view_only_user); F3 only ever views.
        if (state_->view_only_user &&
            (event == ftxui::Event::F2 || event == ftxui::Event::F7 || event == ftxui::Event::F9))
        {
            return true;
        }
        if (event == ftxui::Event::F2)
        {
            ShowCreateNetPageHandler show_create(state_);
            show_create();
            return true;
        }
        if (event == ftxui::Event::F3)
        {
            StartSelectedNetHandler start_net(state_);
            start_net();
            return true;
        }
        if (event == ftxui::Event::F4)
        {
            ShowSettingsPageHandler show_settings(state_);
            show_settings();
            return true;
        }
        if (event == ftxui::Event::F5)
        {
            ShowAdHocNetPageHandler show_adhoc(state_);
            show_adhoc();
            return true;
        }
        if (event == ftxui::Event::F6)
        {
            ViewNetHistoryHandler view_history(state_);
            view_history();
            return true;
        }
        if (event == ftxui::Event::F7)
        {
            StartRowPick(state_, RowPickAction::kEditNet);
            return true;
        }
        if (event == ftxui::Event::F8)
        {
            ExportNetSliceHandler export_net(state_);
            export_net();
            return true;
        }
        if (event == ftxui::Event::F9)
        {
            ShowImportNetPageHandler show_import(state_);
            show_import();
            return true;
        }
        if (event == ftxui::Event::F10)
        {
            QuitHandler quit(state_);
            quit();
            return true;
        }
        return false;
    }

    void ImportSelectedNetSliceHandler::operator()() const
    {
        if (state_->import_session)
        {
            ImportSelectedSession(state_);
            return;
        }
        ImportSelectedNetSlice(state_);
    }

    void StartZmodemReceiveHandler::operator()() const
    {
        StartZmodemReceive(state_);
    }

    void ImportNetBackHandler::operator()() const
    {
        LeaveImportPage(state_);
    }

    bool ImportNetKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (state_->show_zmodem_confirm_modal)
        {
            if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
            {
                ConfirmZmodemActionHandler confirm(state_);
                confirm();
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CancelZmodemActionHandler cancel(state_);
                cancel();
                return true;
            }
            return true;
        }

        if (event == ftxui::Event::F2)
        {
            ImportSelectedNetSliceHandler import_slice(state_);
            import_slice();
            return true;
        }
        if (event == ftxui::Event::F3)
        {
            StartZmodemReceiveHandler start_receive(state_);
            start_receive();
            return true;
        }
        if (event == ftxui::Event::F4 && CanPullUpstream(state_))
        {
            OpenPullWindow(state_);
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            ImportNetBackHandler back(state_);
            back();
            return true;
        }
        return false;
    }

    void RoleContinueHandler::operator()() const
    {
        state_->form_error.clear();
        if (state_->selected_role_index == kRoleViewer || state_->view_only_user)
        {
            ViewStartNet(state_);
            return;
        }
        state_->page = kPageEnterCallsign;
    }

    void RoleBackHandler::operator()() const
    {
        state_->page = kPageNetList;
    }

    bool SelectRoleKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
        {
            RoleContinueHandler continue_handler(state_);
            continue_handler();
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            RoleBackHandler back(state_);
            back();
            return true;
        }
        return false;
    }

    void OperatorCallsignSubmitHandler::operator()() const
    {
        if (state_->operator_callsign.empty())
        {
            state_->form_error = "Enter a callsign to continue.";
            return;
        }
        if (!CheckNetCallsign(state_, state_->operator_callsign, state_->start_net.service))
        {
            return;
        }

        const Net& net = state_->start_net;

        NetInstance instance;
        instance.net_id = net.id;
        instance.instance_date = CurrentDateIso8601();
        instance.started_at = static_cast<std::int64_t>(std::time(nullptr));
        // "Created By" reflects who is running the software (from Settings),
        // which may differ from whichever role-callsign is entered below --
        // falling back to that role-callsign if Settings hasn't been set up yet.
        const std::string& own = OwnCallsign(state_, net.service);
        instance.created_by = own.empty() ? state_->operator_callsign : own;
        instance.operator_role = state_->selected_role_index;
        if (state_->selected_role_index == kRoleNetControl)
        {
            instance.net_control_callsign = state_->operator_callsign;
        }
        else if (state_->selected_role_index == kRoleAlternateNetControl)
        {
            instance.alternate_net_control_callsign = state_->operator_callsign;
        }
        else
        {
            instance.logger_callsign = state_->operator_callsign;
        }
        instance.id = state_->db->CreateNetInstance(instance);

        state_->active_instance = instance;
        state_->active_net_name = net.name;
        state_->active_net_zip = net.default_location;
        state_->active_net_partial_match_canada = net.partial_match_canada;
        state_->active_net_service = net.service;
        state_->active_net_radio = DescribeNetRadio(net);
        state_->active_net_is_ad_hoc = net.is_ad_hoc;
        state_->viewing_only = false;
        LogOperatorCheckIn(state_);
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageActiveNet;
    }

    void OperatorCallsignBackHandler::operator()() const
    {
        state_->form_error.clear();
        state_->page = kPageSelectRole;
    }

    bool EnterCallsignKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (event == ftxui::Event::F2)
        {
            OperatorCallsignSubmitHandler submit(state_);
            submit();
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            OperatorCallsignBackHandler back(state_);
            back();
            return true;
        }
        return false;
    }

    void OpenNewStationModalHandler::operator()() const
    {
        if (!EnsureActiveSessionOpen(state_, ""))
        {
            return;
        }
        ClearModalFields(state_);
        state_->show_new_station_modal = true;
        if (state_->modal_callsign_input)
        {
            state_->modal_callsign_input->TakeFocus();
        }
    }

    void CallsignLookupHandler::operator()() const
    {
        // If matches are showing, Enter on the callsign field takes the one
        // marked ">" (see MarkTypedCallsignMatch). Only fall back to a bare
        // exact-match lookup when there's nothing to pick from.
        if (!state_->modal_callsign_suggestions.empty())
        {
            ApplySelectedCallsignSuggestion(state_);
            return;
        }

        // Known to some net first; failing that, the FCC or ISED data (any
        // distance -- the full callsign was typed, so there's no guessing).
        std::optional<Station> station = state_->db->FindStationByCallsign(state_->modal_station.callsign);
        if (!station.has_value())
        {
            station = FindLicensee(state_->db, state_->modal_station.callsign, state_->active_net_service);
        }
        if (station.has_value())
        {
            state_->modal_station = std::move(*station);
            BackfillCountyFromZip(state_, &state_->modal_station);
            BackfillGridFromZip(state_, &state_->modal_station);
        }

        std::string default_remarks = state_->db->GetSavedNetStationRemarks(
            state_->active_instance.net_id, state_->modal_station.callsign,
            SavedEntryName(state_->active_net_service, state_->modal_station.name));
        if (!default_remarks.empty())
        {
            state_->modal_remarks = default_remarks;
        }
    }

    void CallsignSuggestHandler::operator()() const
    {
        state_->modal_station.callsign = NormalizeCallsign(state_->modal_station.callsign);
        RefreshCallsignSuggestions(state_);
    }

    void LogAndContinueHandler::operator()() const
    {
        if (!LogStationCheckIn(state_))
        {
            return;
        }
        ClearModalFields(state_);
        if (state_->modal_callsign_input)
        {
            state_->modal_callsign_input->TakeFocus();
        }
    }

    void LogAndCloseHandler::operator()() const
    {
        if (!state_->modal_station.callsign.empty() && !LogStationCheckIn(state_))
        {
            return;
        }
        ClearModalFields(state_);
        state_->show_new_station_modal = false;
    }

    void CancelNewStationHandler::operator()() const
    {
        ClearModalFields(state_);
        state_->show_new_station_modal = false;
    }

    void CloseNetInstanceHandler::operator()() const
    {
        CloseActiveNet(state_);
    }

    void EditSelectedCheckInHandler::operator()() const
    {
        if (state_->viewing_only)
        {
            return;
        }
        if (state_->active_check_ins.empty())
        {
            state_->form_error = "No check-ins to edit yet.";
            return;
        }
        OpenEditCheckInForm(state_, state_->active_check_ins[state_->selected_check_in_index]);
    }

    void SaveEditCheckInHandler::operator()() const
    {
        if (SaveEditCheckInForm(state_))
        {
            state_->show_edit_checkin_modal = false;
        }
    }

    void CancelEditCheckInHandler::operator()() const
    {
        state_->form_error.clear();
        state_->show_edit_checkin_modal = false;
    }

    void ExportActiveNetLogHandler::operator()() const
    {
        ExportNetLog(state_, state_->active_net_name, state_->active_instance, state_->active_check_ins);
    }

    bool ActiveNetKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (state_->show_zmodem_confirm_modal)
        {
            if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
            {
                ConfirmZmodemActionHandler confirm(state_);
                confirm();
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CancelZmodemActionHandler cancel(state_);
                cancel();
                return true;
            }
            return true;
        }

        // Watching as a Viewer: only what doesn't change anything.
        if (state_->viewing_only)
        {
            if (event == ftxui::Event::Escape)
            {
                StopViewing(state_);
                return true;
            }
            if (event == ftxui::Event::F7)
            {
                ExportActiveNetLogHandler export_log(state_);
                export_log();
                return true;
            }
            if (event == ftxui::Event::F6)
            {
                StartRowPick(state_, RowPickAction::kViewStationHistory);
                return true;
            }
            if (event == ftxui::Event::F8)
            {
                OpenRegulars(state_);
                return true;
            }
            if (event == ftxui::Event::F9)
            {
                StartRowPick(state_, RowPickAction::kViewStationCard);
                return true;
            }
            if (event == ftxui::Event::F10)
            {
                OpenSessionSummary(state_);
                return true;
            }
            if (event == ftxui::Event::F12)
            {
                OpenActiveSessionNotes(state_);
                return true;
            }
            return event == ftxui::Event::F2 || event == ftxui::Event::F3 || event == ftxui::Event::F4 ||
                   event == ftxui::Event::F5 || event == ftxui::Event::Return;
        }

        bool leaving_callsign = event == ftxui::Event::Tab || event == ftxui::Event::TabReverse ||
                                event == ftxui::Event::F2 || event == ftxui::Event::F3 || event == ftxui::Event::F4 ||
                                event == ftxui::Event::F5 || event == ftxui::Event::F6;
        if (state_->show_new_station_modal && leaving_callsign && state_->modal_callsign_input &&
            state_->modal_callsign_input->Focused())
        {
            // However the callsign field is left -- to another field or
            // straight to logging -- the match marked ">" is the one taken,
            // just as the screen shows it. With no matches, what's known
            // about the callsign typed (see FillCheckInFromKnownStation).
            if (!state_->modal_callsign_suggestions.empty())
            {
                ApplySelectedCallsignSuggestion(state_);
            }
            else
            {
                FillCheckInFromKnownStation(state_);
            }
        }

        if (state_->show_new_station_modal &&
            MoveSuggestionHighlight(event, state_->modal_callsign_input, state_->modal_callsign_suggestions.size(),
                                    &state_->selected_suggestion_index))
        {
            return true;
        }

        bool modal_open = state_->show_new_station_modal || state_->show_edit_checkin_modal;

        if (event == ftxui::Event::F2)
        {
            if (state_->show_edit_checkin_modal)
            {
                SaveEditCheckInHandler save_edit(state_);
                save_edit();
            }
            else if (state_->show_new_station_modal)
            {
                LogAndContinueHandler log_and_continue(state_);
                log_and_continue();
            }
            else
            {
                OpenNewStationModalHandler open_modal(state_);
                open_modal();
            }
            return true;
        }
        // F4/F5/F6 in a check-in window: straight to Remarks, Comment or
        // the role choice, past the station's details.
        if (modal_open && (event == ftxui::Event::F4 || event == ftxui::Event::F5 || event == ftxui::Event::F6))
        {
            bool is_new = state_->show_new_station_modal;
            // Remarks and Comment open with the cursor at the end of what's
            // there, ready to add to it.
            if (event == ftxui::Event::F4)
            {
                int* cursor = is_new ? &state_->modal_remarks_cursor : &state_->edit_checkin_remarks_cursor;
                *cursor = static_cast<int>((is_new ? state_->modal_remarks : state_->edit_checkin_remarks).size());
            }
            else if (event == ftxui::Event::F5)
            {
                int* cursor = is_new ? &state_->modal_comment_cursor : &state_->edit_checkin_comment_cursor;
                *cursor = static_cast<int>((is_new ? state_->modal_comment : state_->edit_checkin_comment).size());
            }
            ftxui::Component target = event == ftxui::Event::F4
                                          ? (is_new ? state_->modal_remarks_input : state_->edit_checkin_remarks_input)
                                      : event == ftxui::Event::F5
                                          ? (is_new ? state_->modal_comment_input : state_->edit_checkin_comment_input)
                                          : (is_new ? state_->modal_role_input : state_->edit_checkin_role_input);
            if (target)
            {
                target->TakeFocus();
            }
            return true;
        }
        if (event == ftxui::Event::F3 && state_->show_new_station_modal)
        {
            LogAndCloseHandler log_and_close(state_);
            log_and_close();
            return true;
        }
        if (event == ftxui::Event::F3 && !modal_open)
        {
            StartRowPick(state_, RowPickAction::kEditCheckIn);
            return true;
        }
        if (event == ftxui::Event::F4 && !modal_open)
        {
            RequestCloseActiveNet(state_);
            return true;
        }
        if (event == ftxui::Event::F5 && !modal_open)
        {
            StartRowPick(state_, RowPickAction::kDeleteCheckIn);
            return true;
        }
        if (event == ftxui::Event::F7 && !modal_open)
        {
            ExportActiveNetLogHandler export_log(state_);
            export_log();
            return true;
        }
        // The seldom-used keys (see InfoWindow).
        if (event == ftxui::Event::F6 && !modal_open)
        {
            StartRowPick(state_, RowPickAction::kViewStationHistory);
            return true;
        }
        if (event == ftxui::Event::F8 && !modal_open)
        {
            OpenRegulars(state_);
            return true;
        }
        if (event == ftxui::Event::F9 && !modal_open)
        {
            StartRowPick(state_, RowPickAction::kViewStationCard);
            return true;
        }
        if (event == ftxui::Event::F10 && !modal_open)
        {
            OpenSessionSummary(state_);
            return true;
        }
        if (event == ftxui::Event::F12 && !modal_open)
        {
            OpenActiveSessionNotes(state_);
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            if (state_->show_edit_checkin_modal)
            {
                CancelEditCheckInHandler cancel_edit(state_);
                cancel_edit();
                return true;
            }
            if (state_->show_new_station_modal)
            {
                CancelNewStationHandler cancel_new_station(state_);
                cancel_new_station();
                return true;
            }
        }
        return false;
    }

    void ShowSettingsPageHandler::operator()() const
    {
        OpenSettingsForm(state_);
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageSettings;
    }

    void SaveSettingsHandler::operator()() const
    {
        if (SaveSettingsForm(state_))
        {
            state_->page = kPageNetList;
        }
    }

    void CancelSettingsHandler::operator()() const
    {
        // Settings aren't optional on first launch: refuse to leave until a
        // callsign and postal code are on file, same requirement F2/Save
        // enforces above, so Esc can't be used to bypass it.
        if (!SettingsAreComplete(state_->settings))
        {
            state_->form_error = "Enter your callsign and postal code first.";
            return;
        }
        state_->form_error.clear();
        state_->page = kPageNetList;
    }

    void RequestStationDataRefreshHandler::operator()() const
    {
        if (!state_->is_console_session)
        {
            return;
        }
        state_->db->RequestImportRun(kDataRefreshJob, static_cast<std::int64_t>(std::time(nullptr)));
        state_->form_error.clear();
        state_->status_message = "Station data refresh starts in a few seconds.";
    }

    void ShowManageUsersPageHandler::operator()() const
    {
        if (!CanManageUsers(state_))
        {
            return;
        }
        RefreshUsers(state_);
        state_->new_user_username.clear();
        state_->new_user_public_key.clear();
        state_->new_user_access_index = 0;
        CloseUserKeys(state_);
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageManageUsers;
    }

    void AddUserHandler::operator()() const
    {
        AddUserFromForm(state_);
    }

    void ShowUserKeysHandler::operator()() const
    {
        OpenUserKeys(state_, state_->selected_user_index);
    }

    void AddUserKeyHandler::operator()() const
    {
        AddKeyToShownUser(state_);
    }

    void ManageUsersBackHandler::operator()() const
    {
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageSettings;
    }

    bool ManageUsersKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (state_->show_user_keys_modal)
        {
            if (event == ftxui::Event::F2)
            {
                SaveEditedUser(state_);
                return true;
            }
            if (event == ftxui::Event::F3)
            {
                StartRowPick(state_, RowPickAction::kRemoveUserKey);
                return true;
            }
            if (event == ftxui::Event::F4)
            {
                AddUserKeyHandler add_key(state_);
                add_key();
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CloseUserKeys(state_);
                return true;
            }
            // Everything else goes to the window's fields and key list.
            return false;
        }
        if (event == ftxui::Event::F2)
        {
            AddUserHandler add_user(state_);
            add_user();
            return true;
        }
        if (event == ftxui::Event::F3)
        {
            StartRowPick(state_, RowPickAction::kRemoveUser);
            return true;
        }
        if (event == ftxui::Event::F4)
        {
            StartRowPick(state_, RowPickAction::kEditUser);
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            ManageUsersBackHandler back(state_);
            back();
            return true;
        }
        return false;
    }

    bool SettingsKeyHandler::operator()(const ftxui::Event& event) const
    {
        // The Upstream Server window takes its own keys; typing goes to its
        // fields, and every other F-key is swallowed.
        if (state_->show_upstream_window)
        {
            if (event == ftxui::Event::F2)
            {
                SaveUpstreamWindow(state_);
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CloseUpstreamWindow(state_);
                return true;
            }
            return event == ftxui::Event::F1 || event == ftxui::Event::F3 || event == ftxui::Event::F4 ||
                   event == ftxui::Event::F5 || event == ftxui::Event::F6 || event == ftxui::Event::F7 ||
                   event == ftxui::Event::F8 || event == ftxui::Event::F9 || event == ftxui::Event::F10 ||
                   event == ftxui::Event::F11 || event == ftxui::Event::F12;
        }
        if (state_->show_my_keys_window)
        {
            if (event == ftxui::Event::F2)
            {
                SaveMyKeyComment(state_);
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                CloseMyKeys(state_);
                return true;
            }
            return event == ftxui::Event::F1 || event == ftxui::Event::F3 || event == ftxui::Event::F4 ||
                   event == ftxui::Event::F5 || event == ftxui::Event::F6 || event == ftxui::Event::F7 ||
                   event == ftxui::Event::F8 || event == ftxui::Event::F9 || event == ftxui::Event::F10 ||
                   event == ftxui::Event::F11 || event == ftxui::Event::F12;
        }
        if (event == ftxui::Event::F4 && CanEditOwnKeys(state_))
        {
            OpenMyKeys(state_);
            return true;
        }
        if (event == ftxui::Event::F5 && state_->is_console_session)
        {
            OpenUpstreamWindow(state_);
            return true;
        }
        if (event == ftxui::Event::F2)
        {
            SaveSettingsHandler save(state_);
            save();
            return true;
        }
        if (event == ftxui::Event::F3 && state_->is_console_session)
        {
            RequestStationDataRefreshHandler request_refresh(state_);
            request_refresh();
            return true;
        }
        if (event == ftxui::Event::F4 && CanManageUsers(state_))
        {
            ShowManageUsersPageHandler show_manage_users(state_);
            show_manage_users();
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            CancelSettingsHandler cancel(state_);
            cancel();
            return true;
        }
        return false;
    }

    void MyKeySelectionHandler::operator()() const
    {
        LoadMyKeyComment(state_);
    }

    void ConfirmZmodemActionHandler::operator()() const
    {
        ConfirmZmodemAction(state_);
    }

    void CancelZmodemActionHandler::operator()() const
    {
        CancelZmodemAction(state_);
    }

    void RequestDeleteNetHandler::operator()() const
    {
        RequestDeleteNet(state_);
    }

    void ConfirmDeleteNetHandler::operator()() const
    {
        ConfirmDeleteNet(state_);
    }

    void CancelDeleteNetHandler::operator()() const
    {
        CancelDeleteNet(state_);
    }

    void QuitHandler::operator()() const
    {
        state_->screen->ExitLoopClosure()();
    }

    // Every key while a list is in pick mode (see RowPickAction): digits
    // build the number, Backspace erases, Enter picks, Esc cancels, Up/Down
    // move the highlight (Enter with nothing typed picks the highlighted
    // row). Everything else is swallowed, so a digit can't land in a text
    // field that happens to have focus.
    static bool HandleRowPickKey(AppState* state, const ftxui::Event& event)
    {
        if (event == ftxui::Event::Return)
        {
            FinishRowPick(state);
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            CancelRowPick(state);
            return true;
        }
        if (event == ftxui::Event::Backspace)
        {
            EraseRowPickDigit(state);
            return true;
        }
        if (event.is_character() && event.character().size() == 1 &&
            std::isdigit(static_cast<unsigned char>(event.character()[0])) != 0)
        {
            TypeRowPickDigit(state, event.character()[0]);
            return true;
        }
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)
        {
            MoveRowPickHighlight(state, event == ftxui::Event::ArrowUp ? -1 : 1);
            return true;
        }
        return event != ftxui::Event::Custom;
    }

    // Every key while a ConfirmPrompt is showing; anything but its own keys
    // is swallowed, so nothing happens behind it.
    static bool HandleConfirmPromptKey(AppState* state, const ftxui::Event& event)
    {
        bool yes = event == ftxui::Event::F2 || event == ftxui::Event::Return;
        if (state->confirm_prompt == ConfirmPrompt::kSessionClosed)
        {
            if (event == ftxui::Event::Return)
            {
                LeaveClosedSession(state);
            }
        }
        else if (state->confirm_prompt == ConfirmPrompt::kPushToNet)
        {
            if (yes)
            {
                ConfirmPushToNet(state);
            }
            else if (event == ftxui::Event::Escape)
            {
                DeclinePushToNet(state);
            }
        }
        else if (event == ftxui::Event::Escape)
        {
            CancelConfirmPrompt(state);
        }
        else if (state->confirm_prompt == ConfirmPrompt::kResumeNet)
        {
            if (yes)
            {
                ResumeOpenNet(state);
            }
            else if (event == ftxui::Event::F3)
            {
                CloseOpenNetAndStartNew(state);
            }
            else if (event == ftxui::Event::F4)
            {
                ViewOpenNet(state);
            }
        }
        else if (state->confirm_prompt == ConfirmPrompt::kCloseNet && yes)
        {
            CloseActiveNet(state);
        }
        else if (state->confirm_prompt == ConfirmPrompt::kCloseNet && event == ftxui::Event::F3 &&
                 CanPushUpstream(state))
        {
            CloseActiveNetAndPush(state);
        }
        else if (state->confirm_prompt == ConfirmPrompt::kImportOtherNet && yes)
        {
            ImportSelectedSessionAnyway(state);
        }
        return event != ftxui::Event::Custom;
    }

    // Keys while an InfoWindow is open: it takes them all, except that in
    // Find a Station typing goes to its callsign field.
    static bool HandleInfoWindowKey(AppState* state, const ftxui::Event& event)
    {
        if (event == ftxui::Event::Escape)
        {
            CloseInfoWindow(state);
            return true;
        }
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)
        {
            MoveInfoSelection(state, event == ftxui::Event::ArrowUp ? -1 : 1);
            return true;
        }
        if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
        {
            MoveInfoSelection(state, event == ftxui::Event::PageUp ? -10 : 10);
            return true;
        }
        if (event == ftxui::Event::Return)
        {
            if (state->info_window == InfoWindow::kRegulars)
            {
                CheckInSelectedRegular(state);
            }
            return true;
        }
        if (state->info_window == InfoWindow::kStationSearch && event != ftxui::Event::Custom &&
            (event.is_character() || event == ftxui::Event::Backspace || event == ftxui::Event::Delete ||
             event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight || event == ftxui::Event::Home ||
             event == ftxui::Event::End))
        {
            return false;  // To the callsign field.
        }
        return event != ftxui::Event::Custom;
    }

    // True while any dialog is open over the page (not counting an
    // InfoWindow, pick mode or a confirmation, handled before this matters).
    static bool AnyPageDialogOpen(const AppState* state)
    {
        return state->show_new_station_modal || state->show_edit_checkin_modal || state->show_saved_station_modal ||
               state->show_zmodem_confirm_modal || state->show_delete_net_confirm_modal ||
               state->show_session_notes_modal || state->show_merge_modal || state->show_upstream_window ||
               state->show_pull_modal;
    }

    // Keys in the Import or Merge window (see MergeStage); it takes them
    // all.
    static bool HandleNetMergeKey(AppState* state, const ftxui::Event& event)
    {
        if (event == ftxui::Event::Escape)
        {
            BackOutOfNetMerge(state);
        }
        else if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)
        {
            MoveMergeHighlight(state, event == ftxui::Event::ArrowUp ? -1 : 1);
        }
        else if (state->merge_stage == MergeStage::kChooseNet)
        {
            if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
            {
                ImportSelectedNetSliceAnyway(state);
            }
            else if (event == ftxui::Event::F3)
            {
                ChooseMergeTarget(state);
            }
        }
        else if (event == ftxui::Event::F2)
        {
            ConfirmNetMerge(state);
        }
        else if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight ||
                 event == ftxui::Event::Return || event == ftxui::Event::Character(' '))
        {
            ToggleMergeReplace(state);
        }
        return event != ftxui::Event::Custom;
    }

    // Keys in the Pull window (see PullStage); it takes them all.
    static bool HandlePullKey(AppState* state, const ftxui::Event& event)
    {
        if (event == ftxui::Event::Escape)
        {
            ClosePullWindow(state);
        }
        else if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)
        {
            MovePullHighlight(state, event == ftxui::Event::ArrowUp ? -1 : 1);
        }
        else if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
        {
            ChoosePullNet(state);
        }
        return event != ftxui::Event::Custom;
    }

    // Keys while the Session Notes window is open: F2 saves, Esc cancels,
    // and every other F-key is swallowed so nothing happens behind it;
    // everything else goes to the notes (see NotesEditor).
    static bool HandleSessionNotesKey(AppState* state, const ftxui::Event& event)
    {
        if (event == ftxui::Event::F2)
        {
            SaveSessionNotes(state);
            return true;
        }
        if (event == ftxui::Event::Escape)
        {
            CloseSessionNotes(state);
            return true;
        }
        return event == ftxui::Event::F1 || event == ftxui::Event::F3 || event == ftxui::Event::F4 ||
               event == ftxui::Event::F5 || event == ftxui::Event::F6 || event == ftxui::Event::F7 ||
               event == ftxui::Event::F8 || event == ftxui::Event::F9 || event == ftxui::Event::F10 ||
               event == ftxui::Event::F11 || event == ftxui::Event::F12 || event == ftxui::Event::Tab ||
               event == ftxui::Event::TabReverse;
    }

    bool AppKeyHandler::operator()(const ftxui::Event& event) const
    {
        // A click on the top bar's update notice, on any page.
        if (event == OpenUpdatePageEvent())
        {
            OpenUpdatePage(state_);
            return true;
        }
        if (state_->info_window != InfoWindow::kNone)
        {
            return HandleInfoWindowKey(state_, event);
        }
        // A delete confirmation, or a list in pick mode, takes every key
        // first, whatever page it's on.
        if (state_->show_row_delete_confirm_modal)
        {
            if (event == ftxui::Event::F2 || event == ftxui::Event::Return)
            {
                ConfirmRowDelete(state_);
            }
            else if (event == ftxui::Event::Escape)
            {
                CancelRowDelete(state_);
            }
            return event != ftxui::Event::Custom;
        }
        if (state_->row_pick_action != RowPickAction::kNone)
        {
            return HandleRowPickKey(state_, event);
        }
        if (state_->show_confirm_prompt)
        {
            return HandleConfirmPromptKey(state_, event);
        }
        if (state_->show_session_notes_modal)
        {
            return HandleSessionNotesKey(state_, event);
        }
        if (state_->pull_stage != PullStage::kNone)
        {
            return HandlePullKey(state_, event);
        }
        if (state_->merge_stage != MergeStage::kNone)
        {
            return HandleNetMergeKey(state_, event);
        }
        if (event == ftxui::Event::F1 && !AnyPageDialogOpen(state_))
        {
            OpenHelp(state_);
            return true;
        }

        if (state_->page == kPageNetList)
        {
            NetListKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageCreateNet)
        {
            CreateNetKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageSelectRole)
        {
            SelectRoleKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageEnterCallsign)
        {
            EnterCallsignKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageActiveNet)
        {
            ActiveNetKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageSettings)
        {
            SettingsKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageAdHocNet)
        {
            AdHocNetKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageNetHistory)
        {
            NetHistoryKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageEditNet)
        {
            EditNetKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageImportNet)
        {
            ImportNetKeyHandler handler(state_);
            return handler(event);
        }
        if (state_->page == kPageManageUsers)
        {
            ManageUsersKeyHandler handler(state_);
            return handler(event);
        }
        return false;
    }

    SafeAppEventDispatcher::SafeAppEventDispatcher(ftxui::Component child, AppState* state) : state_(state)
    {
        Add(std::move(child));
    }

    bool SafeAppEventDispatcher::OnEvent(ftxui::Event event)
    {
        // A click on a key in a key bar presses that key.
        ftxui::Event clicked_key;
        if (event.is_mouse() && ClickedKey(event.mouse(), &clicked_key))
        {
            return Dispatch(clicked_key);
        }
        // In a list numbered for picking, a click highlights a row and a
        // double-click picks it, as Enter would.
        int clicked_row = -1;
        bool double_click = false;
        if (event.is_mouse() && state_->row_pick_action != RowPickAction::kNone &&
            ClickedRow(event.mouse(), &clicked_row, &double_click))
        {
            HighlightRowPickRow(state_, clicked_row);
            return double_click ? Dispatch(ftxui::Event::Return) : true;
        }
        std::vector<ftxui::Event> events = escape_splitter_.Feed(event);
        if (events.empty())
        {
            return true;  // Held: part of a key still arriving.
        }
        bool handled = false;
        for (const ftxui::Event& one : events)
        {
            handled = Dispatch(one) || handled;
        }
        return handled;
    }

    ftxui::Element SafeAppEventDispatcher::Render()
    {
        StopMouseMovementReports();
        BeginClickTargets();
        ftxui::Dimensions terminal = ftxui::Terminal::Size();
        SetFrameTerminalSize(terminal);
        UpdateListWidths(state_, terminal.dimx);
        state_->screen_height = terminal.dimy;
        // Not while a prompt or window is up over it, which may be about the
        // highlighted net.
        state_->showing_net_list =
            state_->page == kPageNetList && !state_->show_confirm_prompt && state_->info_window == InfoWindow::kNone &&
            state_->row_pick_action == RowPickAction::kNone && !state_->show_zmodem_confirm_modal;
        return ComponentBase::Render();
    }

    bool SafeAppEventDispatcher::Dispatch(const ftxui::Event& event)
    {
        // A status message has been seen by the time the operator presses
        // another key, so it goes then; a handler that sets a new one below
        // still shows it. Not on a redraw request, mouse movement or cursor
        // report, none of which the operator did, nor on the key answering
        // the ZMODEM/Show Folder pop-up, which opens with the export's
        // "Saved to" message behind it.
        if (event != ftxui::Event::Custom && !event.is_mouse() && !event.is_cursor_reporting() &&
            !state_->show_zmodem_confirm_modal)
        {
            state_->status_message.clear();
        }
        try
        {
            AppKeyHandler key_handler(state_);
            if (key_handler(event))
            {
                return true;
            }
            return ComponentBase::OnEvent(event);
        }
        catch (const std::exception& e)
        {
            state_->form_error = std::string("Action not completed: ") + e.what();
            return true;
        }
    }

}  // namespace ql
