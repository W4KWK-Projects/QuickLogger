#include "handlers.hpp"

#include <cctype>
#include <cstddef>
#include <ctime>
#include <exception>
#include <utility>

#include "../date_utils.hpp"
#include "../text_utils.hpp"
#include "../uls_import.hpp"

namespace ql
{

    void UppercaseFieldHandler::operator()() const
    {
        *field_ = NormalizeCallsign(*field_);
    }

    void ZipCodeFieldHandler::operator()() const
    {
        std::string digits_only;
        for (char c : *field_)
        {
            if (std::isdigit(static_cast<unsigned char>(c)))
            {
                digits_only.push_back(c);
            }
        }
        if (digits_only.size() > 5)
        {
            digits_only.resize(5);
        }
        *field_ = digits_only;
    }

    void ShowCreateNetPageHandler::operator()() const
    {
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

        Net net;
        net.name = state_->new_net_name;
        net.mode = state_->new_net_mode;
        net.default_frequency = state_->new_net_frequency;
        net.default_location = state_->new_net_location;
        net.recurrence_description = state_->new_net_recurrence;
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
        state_->page = kPageAdHocNet;
        if (state_->ad_hoc_net_name_input)
        {
            state_->ad_hoc_net_name_input->TakeFocus();
        }
    }

    void AdHocNetSubmitHandler::operator()() const
    {
        if (state_->new_net_name.empty())
        {
            state_->form_error = "Net name is required.";
            return;
        }

        Net net;
        net.name = state_->new_net_name;
        net.mode = state_->new_net_mode;
        net.default_frequency = state_->new_net_frequency;
        net.default_location = state_->new_net_location;
        net.created_at = static_cast<std::int64_t>(std::time(nullptr));
        std::int64_t new_net_id = state_->db->CreateNet(net);

        RefreshNets(state_);
        for (int i = 0; i < static_cast<int>(state_->nets.size()); ++i)
        {
            if (state_->nets[i].id == new_net_id)
            {
                state_->selected_net_index = i;
                break;
            }
        }

        ResetCreateNetForm(state_);
        ResetStartNetFlow(state_);
        state_->page = kPageSelectRole;
    }

    bool AdHocNetKeyHandler::operator()(const ftxui::Event& event) const
    {
        if (event == ftxui::Event::F2)
        {
            AdHocNetSubmitHandler submit(state_);
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

    void ViewNetHistoryHandler::operator()() const
    {
        if (state_->nets.empty())
        {
            state_->form_error = "Create a recurring net first.";
            return;
        }
        RefreshNetHistory(state_);
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageNetHistory;
    }

    void HistoryInstanceChangedHandler::operator()() const
    {
        RefreshHistoryCheckIns(state_);
    }

    void NetHistoryBackHandler::operator()() const
    {
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
        std::string net_name;
        if (state_->selected_net_index < static_cast<int>(state_->nets.size()))
        {
            net_name = state_->nets[state_->selected_net_index].name;
        }
        ExportNetLog(state_, net_name, instance, check_ins);
    }

    bool NetHistoryKeyHandler::operator()(const ftxui::Event& event) const
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

        if (event == ftxui::Event::Escape)
        {
            NetHistoryBackHandler back(state_);
            back();
            return true;
        }
        if (event == ftxui::Event::F7)
        {
            ExportNetHistoryLogHandler export_log(state_);
            export_log();
            return true;
        }
        if (event == ftxui::Event::F5)
        {
            StartRowPick(state_, RowPickAction::kDeleteNetInstance);
            return true;
        }
        if (event == ftxui::Event::F4)
        {
            StartRowPick(state_, RowPickAction::kDeleteHistoryCheckIn);
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
            // Nothing to save yet: rather than just complaining, take the
            // operator to the callsign field -- F3 is how people reach for
            // "add a station" here, and the field is otherwise six Tab-stops
            // down the page. Only say something if they'd already filled in
            // other fields and just left the callsign out.
            state_->form_error = SavedStationFormIsBlank(state_)
                                     ? std::string()
                                     : std::string("Enter a callsign to save this station.");
            if (state_->saved_station_callsign_input)
            {
                state_->saved_station_callsign_input->TakeFocus();
            }
            return;
        }
        SaveNetStationForm(state_);
    }

    void ExportSavedStationsHandler::operator()() const
    {
        ExportSavedStations(state_, state_->edit_net_name, state_->edit_net_saved_stations);
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

    void SelectSavedStationSuggestionHandler::operator()() const
    {
        ApplySelectedSavedStationSuggestion(state_);
    }

    void LoadSavedStationHandler::operator()() const
    {
        if (state_->edit_net_saved_stations.empty())
        {
            return;
        }
        LoadSavedStationIntoForm(
            state_, state_->edit_net_saved_stations[state_->selected_saved_station_index]);
        if (state_->saved_station_callsign_input)
        {
            state_->saved_station_callsign_input->TakeFocus();
        }
    }

    void AddNewSavedStationHandler::operator()() const
    {
        state_->saved_station = Station();
        state_->saved_station_remarks.clear();
        state_->form_error.clear();
        state_->saved_station_suggestions.clear();
        state_->saved_station_suggestion_labels.clear();
        if (state_->saved_station_callsign_input)
        {
            state_->saved_station_callsign_input->TakeFocus();
        }
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
    static bool MoveSuggestionHighlight(const ftxui::Event& event,
                                        const ftxui::Component& callsign_input,
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

        if (MoveSuggestionHighlight(event, state_->saved_station_callsign_input,
                                    state_->saved_station_suggestions.size(),
                                    &state_->selected_saved_station_suggestion_index))
        {
            return true;
        }

        if (event == ftxui::Event::F2)
        {
            SaveEditNetHandler save(state_);
            save();
            return true;
        }
        if (event == ftxui::Event::F3)
        {
            SaveNetStationFormHandler save_station(state_);
            save_station();
            return true;
        }
        if (event == ftxui::Event::F4)
        {
            StartRowPick(state_, RowPickAction::kRemoveSavedStation);
            return true;
        }
        if (event == ftxui::Event::F9)
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
        RefreshImportNetFiles(state_);
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageImportNet;
    }

    bool NetListKeyHandler::operator()(const ftxui::Event& event) const
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
        ImportSelectedNetSlice(state_);
    }

    void StartZmodemReceiveHandler::operator()() const
    {
        StartZmodemReceive(state_);
    }

    void ImportNetBackHandler::operator()() const
    {
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageNetList;
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

        const Net& net = state_->nets[state_->selected_net_index];

        NetInstance instance;
        instance.net_id = net.id;
        instance.instance_date = CurrentDateIso8601();
        instance.started_at = static_cast<std::int64_t>(std::time(nullptr));
        // "Created By" reflects who is running the software (from Settings),
        // which may differ from whichever role-callsign is entered below --
        // falling back to that role-callsign if Settings hasn't been set up yet.
        instance.created_by = state_->settings.callsign.empty() ? state_->operator_callsign
                                                                : state_->settings.callsign;
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
        ClearModalFields(state_);
        state_->show_new_station_modal = true;
        if (state_->modal_callsign_input)
        {
            state_->modal_callsign_input->TakeFocus();
        }
    }

    void CallsignLookupHandler::operator()() const
    {
        // If suggestions are showing, Enter on the callsign field accepts the
        // highlighted one -- same as pressing Enter on the suggestion menu
        // itself (SelectCallsignSuggestionHandler). Only fall back to a bare
        // exact-match lookup when there's nothing to pick from.
        if (!state_->modal_callsign_suggestions.empty())
        {
            ApplySelectedCallsignSuggestion(state_);
            return;
        }

        // Known to some net first; failing that, the FCC data (any
        // distance -- the full callsign was typed, so there's no guessing).
        std::optional<Station> station =
            state_->db->FindStationByCallsign(state_->modal_station.callsign);
        if (!station.has_value())
        {
            station = state_->db->FindUlsStationByCallsign(state_->modal_station.callsign);
        }
        if (station.has_value())
        {
            state_->modal_station = std::move(*station);
            BackfillCountyFromZip(state_, &state_->modal_station);
        }

        std::string default_remarks = state_->db->GetSavedNetStationRemarks(
            state_->active_instance.net_id, state_->modal_station.callsign);
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

    void SelectCallsignSuggestionHandler::operator()() const
    {
        ApplySelectedCallsignSuggestion(state_);
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
        if (state_->active_check_ins.empty())
        {
            state_->form_error = "No check-ins to edit yet.";
            return;
        }
        OpenEditCheckInForm(state_, state_->active_check_ins[state_->selected_check_in_index]);
    }

    void SaveEditCheckInHandler::operator()() const
    {
        SaveEditCheckInForm(state_);
        state_->show_edit_checkin_modal = false;
    }

    void CancelEditCheckInHandler::operator()() const
    {
        state_->form_error.clear();
        state_->show_edit_checkin_modal = false;
    }

    void ExportActiveNetLogHandler::operator()() const
    {
        ExportNetLog(state_, state_->active_net_name, state_->active_instance,
                     state_->active_check_ins);
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

        if (state_->show_new_station_modal &&
            MoveSuggestionHighlight(event, state_->modal_callsign_input,
                                    state_->modal_callsign_suggestions.size(),
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
        state_->settings_form = state_->settings;
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageSettings;
    }

    void SaveSettingsHandler::operator()() const
    {
        if (state_->settings_form.callsign.empty())
        {
            state_->form_error = "Your callsign is required.";
            return;
        }
        if (state_->settings_form.location.size() != 5)
        {
            state_->form_error = "Your ZIP code is required and must be 5 digits.";
            return;
        }

        SaveSettings(state_->settings_path, state_->settings_form);
        state_->settings = state_->settings_form;
        state_->form_error.clear();
        state_->page = kPageNetList;
    }

    void CancelSettingsHandler::operator()() const
    {
        // Settings aren't optional on first launch: refuse to leave until a
        // callsign and ZIP code are on file, same requirement F2/Save
        // enforces above, so Esc can't be used to bypass it.
        if (!SettingsAreComplete(state_->settings))
        {
            state_->form_error = "Please enter your callsign and ZIP code before continuing.";
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
        state_->db->RequestImportRun(kDataRefreshJob,
                                     static_cast<std::int64_t>(std::time(nullptr)));
        state_->form_error.clear();
        state_->status_message = "Station data refresh requested; it starts within a few seconds.";
    }

    void ShowManageUsersPageHandler::operator()() const
    {
        RefreshUsers(state_);
        state_->new_user_username.clear();
        state_->new_user_public_key.clear();
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageManageUsers;
    }

    void AddUserHandler::operator()() const
    {
        AddUserFromForm(state_);
    }

    void ManageUsersBackHandler::operator()() const
    {
        state_->form_error.clear();
        state_->status_message.clear();
        state_->page = kPageSettings;
    }

    bool ManageUsersKeyHandler::operator()(const ftxui::Event& event) const
    {
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
        if (event == ftxui::Event::F4 && state_->is_console_session)
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
        if (event == ftxui::Event::Escape)
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
        }
        else if (state->confirm_prompt == ConfirmPrompt::kCloseNet && yes)
        {
            CloseActiveNet(state);
        }
        return event != ftxui::Event::Custom;
    }

    bool AppKeyHandler::operator()(const ftxui::Event& event) const
    {
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

    SafeAppEventDispatcher::SafeAppEventDispatcher(ftxui::Component child, AppState* state)
        : state_(state)
    {
        Add(std::move(child));
    }

    bool SafeAppEventDispatcher::OnEvent(ftxui::Event event)
    {
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
