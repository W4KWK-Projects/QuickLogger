#include "handlers.hpp"

#include <ctime>

#include "../date_utils.hpp"
#include "../text_utils.hpp"

namespace ql
{

    void UppercaseFieldHandler::operator()() const
    {
        *field_ = ToUpperAscii(*field_);
    }

    void ShowCreateNetPageHandler::operator()() const
    {
        ResetCreateNetForm(state_);
        state_->page = kPageCreateNet;
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

    bool CreateNetKeyHandler::operator()(ftxui::Event event) const
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
        if (state_->nets.empty())
        {
            state_->form_error = "Create a recurring net first.";
            return;
        }
        ResetStartNetFlow(state_);
        state_->page = kPageSelectRole;
    }

    void ShowAdHocNetPageHandler::operator()() const
    {
        ResetCreateNetForm(state_);
        state_->page = kPageAdHocNet;
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

    bool AdHocNetKeyHandler::operator()(ftxui::Event event) const
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
        state_->page = kPageNetHistory;
    }

    void NetHistoryBackHandler::operator()() const
    {
        state_->page = kPageNetList;
    }

    bool NetHistoryKeyHandler::operator()(ftxui::Event event) const
    {
        if (event == ftxui::Event::Escape)
        {
            NetHistoryBackHandler back(state_);
            back();
            return true;
        }
        return false;
    }

    void ShowEditNetPageHandler::operator()() const
    {
        if (state_->nets.empty())
        {
            state_->form_error = "Create a recurring net first.";
            return;
        }
        OpenEditNetForm(state_, state_->nets[state_->selected_net_index]);
        state_->page = kPageEditNet;
    }

    void SaveEditNetHandler::operator()() const
    {
        SaveEditNetForm(state_);
    }

    void SaveNetStationFormHandler::operator()() const
    {
        SaveNetStationForm(state_);
    }

    void RemoveSavedNetStationHandler::operator()() const
    {
        RemoveSelectedSavedNetStation(state_);
    }

    void DeleteSelectedSavedStationHandler::operator()() const
    {
        DeleteSelectedSavedStationCompletely(state_);
    }

    void SavedStationCallsignChangeHandler::operator()() const
    {
        state_->saved_station.callsign = ToUpperAscii(state_->saved_station.callsign);
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

    void EditNetBackHandler::operator()() const
    {
        state_->form_error.clear();
        state_->page = kPageNetList;
    }

    bool EditNetKeyHandler::operator()(ftxui::Event event) const
    {
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
            RemoveSavedNetStationHandler remove_station(state_);
            remove_station();
            return true;
        }
        if (event == ftxui::Event::F5)
        {
            DeleteSelectedSavedStationHandler delete_station(state_);
            delete_station();
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

    bool NetListKeyHandler::operator()(ftxui::Event event) const
    {
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
            ShowEditNetPageHandler show_edit(state_);
            show_edit();
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

    void RoleContinueHandler::operator()() const
    {
        state_->form_error.clear();
        state_->page = kPageEnterCallsign;
    }

    void RoleBackHandler::operator()() const
    {
        state_->page = kPageNetList;
    }

    bool SelectRoleKeyHandler::operator()(ftxui::Event event) const
    {
        if (event == ftxui::Event::F2)
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
        // "Created By" reflects who is running the software (from Settings),
        // which may differ from whichever role-callsign is entered below --
        // falling back to that role-callsign if Settings hasn't been set up yet.
        instance.created_by = state_->settings.callsign.empty() ? state_->operator_callsign
                                                                : state_->settings.callsign;
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
        state_->active_check_ins.clear();
        state_->active_display_rows.clear();
        state_->form_error.clear();
        state_->page = kPageActiveNet;
    }

    void OperatorCallsignBackHandler::operator()() const
    {
        state_->form_error.clear();
        state_->page = kPageSelectRole;
    }

    bool EnterCallsignKeyHandler::operator()(ftxui::Event event) const
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

        std::optional<Station> station =
            state_->db->FindStationByCallsign(state_->modal_station.callsign);
        if (station.has_value())
        {
            state_->modal_station = *station;
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
        state_->modal_station.callsign = ToUpperAscii(state_->modal_station.callsign);
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

    void CloseNetInstanceHandler::operator()() const
    {
        state_->db->CloseNetInstance(state_->active_instance.id,
                                     static_cast<std::int64_t>(std::time(nullptr)));
        state_->active_check_ins.clear();
        state_->active_display_rows.clear();
        state_->show_new_station_modal = false;
        ClearModalFields(state_);
        ResetStartNetFlow(state_);
        state_->page = kPageNetList;
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

    void RemoveSelectedCheckInHandler::operator()() const
    {
        RemoveSelectedCheckIn(state_);
    }

    bool ActiveNetKeyHandler::operator()(ftxui::Event event) const
    {
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
        if (event == ftxui::Event::F3 && !modal_open)
        {
            EditSelectedCheckInHandler edit_selected(state_);
            edit_selected();
            return true;
        }
        if (event == ftxui::Event::F4 && !modal_open)
        {
            CloseNetInstanceHandler close_net(state_);
            close_net();
            return true;
        }
        if (event == ftxui::Event::F5 && !modal_open)
        {
            RemoveSelectedCheckInHandler remove_selected(state_);
            remove_selected();
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
                LogAndCloseHandler log_and_close(state_);
                log_and_close();
                return true;
            }
        }
        return false;
    }

    void ShowSettingsPageHandler::operator()() const
    {
        state_->settings_form = state_->settings;
        state_->form_error.clear();
        state_->page = kPageSettings;
    }

    void SaveSettingsHandler::operator()() const
    {
        if (state_->settings_form.callsign.empty())
        {
            state_->form_error = "Your callsign is required.";
            return;
        }

        SaveSettings(state_->settings_path, state_->settings_form);
        state_->settings = state_->settings_form;
        state_->form_error.clear();
        state_->page = kPageNetList;
    }

    void CancelSettingsHandler::operator()() const
    {
        state_->form_error.clear();
        state_->page = kPageNetList;
    }

    void StartUlsImportHandler::operator()() const
    {
        StartUlsImport(state_->db_path, &state_->uls_import_progress, state_->screen);
    }

    bool SettingsKeyHandler::operator()(ftxui::Event event) const
    {
        if (event == ftxui::Event::F2)
        {
            SaveSettingsHandler save(state_);
            save();
            return true;
        }
        if (event == ftxui::Event::F3)
        {
            StartUlsImportHandler start_import(state_);
            start_import();
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

    void QuitHandler::operator()() const
    {
        if (state_->uls_import_progress.running.load())
        {
            state_->form_error =
                "Cannot quit while the ULS import is running. Please wait for it to finish.";
            return;
        }
        state_->screen->ExitLoopClosure()();
    }

    bool AppKeyHandler::operator()(ftxui::Event event) const
    {
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
        return false;
    }

}  // namespace ql
