#include "app_state.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <ctime>

#include "../geo_utils.hpp"

namespace ql
{

    static bool CallsignsEqual(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            unsigned char char_a = static_cast<unsigned char>(a[i]);
            unsigned char char_b = static_cast<unsigned char>(b[i]);
            if (std::toupper(char_a) != std::toupper(char_b))
            {
                return false;
            }
        }
        return true;
    }

    // Column widths shared between every "list of stations/matches" row
    // formatter below and its corresponding header-row builder, so a header
    // can never drift out of alignment with the data under it (an earlier
    // version of the net-history row baked literal "NC:"/"Alt:"/"Log:"
    // prefixes into the row instead of relying on the header alone to name
    // the column -- those prefixes ate into the column width unevenly and
    // never actually lined up with "Net Control"/"Alternate NC"/"Logger"
    // above them; removed rather than width-compensated for, since the
    // header already says what the column is).
    // Every field using these widths is built with printf's *runtime* width
    // and precision (the two `*`s in e.g. "%-*.*s", each consuming one int
    // argument before the string) rather than baking a number into the
    // format string literally -- that way a formatter and its header can't
    // silently drift apart the way a hand-typed number could. The precision
    // half also truncates a value longer than the column instead of
    // overflowing it and pushing every later column on that row out of
    // alignment -- plain "%-10s" only pads short values, it never shortens
    // long ones.
    static constexpr int kCallsignColumnWidth = 10;
    static constexpr int kNameColumnWidth = 20;
    static constexpr int kMemberIdColumnWidth = 10;
    static constexpr int kSignalReportColumnWidth = 6;
    static constexpr int kDateColumnWidth = 12;
    // Sized to fit the longest header label that lands in each of these
    // columns ("Alternate NC", 12 chars) plus a little padding -- the row
    // shows the bare callsign, no "NC:"/"Alt:"/"Log:" prefix, since the
    // header above it already names the column.
    static constexpr int kNetControlColumnWidth = 14;
    static constexpr int kAlternateNcColumnWidth = 14;
    static constexpr int kLoggerColumnWidth = 14;

    // Every header below sits above an ftxui::Menu, not a plain text list.
    // Menu's default entry renderer always prepends a 2-character indicator
    // ("> " for the focused row, "  " for every other one -- see
    // DefaultOptionTransform in FTXUI's menu.cpp) to every row it renders, so
    // without this same gutter a header's labels land two columns left of the
    // data they're supposed to name, no matter how well the widths above line
    // up on their own.
    static constexpr int kMenuEntryIndicatorWidth = 2;

    static std::string FormatNetInstanceRow(const NetInstance& instance)
    {
        const char* status = instance.status == NetInstanceStatus::kOpen ? "OPEN" : "closed";
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %-*.*s %-*.*s %s", kDateColumnWidth,
                      kDateColumnWidth, instance.instance_date.c_str(), kNetControlColumnWidth,
                      kNetControlColumnWidth, instance.net_control_callsign.c_str(),
                      kAlternateNcColumnWidth, kAlternateNcColumnWidth,
                      instance.alternate_net_control_callsign.c_str(), kLoggerColumnWidth,
                      kLoggerColumnWidth, instance.logger_callsign.c_str(), status);
        return std::string(buffer);
    }

    std::string FormatNetInstanceHeaderRow()
    {
        // Same field widths as FormatNetInstanceRow.
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %-*.*s %-*.*s %s", kDateColumnWidth,
                      kDateColumnWidth, "Date", kNetControlColumnWidth, kNetControlColumnWidth,
                      "Net Control", kAlternateNcColumnWidth, kAlternateNcColumnWidth,
                      "Alternate NC", kLoggerColumnWidth, kLoggerColumnWidth, "Logger", "Status");
        return std::string(kMenuEntryIndicatorWidth, ' ') + buffer;
    }

    static std::string FormatCallsignSuggestion(const Station& station, bool is_this_net)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %s", kCallsignColumnWidth,
                      kCallsignColumnWidth, station.callsign.c_str(), kNameColumnWidth,
                      kNameColumnWidth, station.name.c_str(),
                      is_this_net ? "(this net)" : "(other net)");
        return std::string(buffer);
    }

    std::string FormatCallsignSuggestionHeaderRow()
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %s", kCallsignColumnWidth,
                      kCallsignColumnWidth, "Callsign", kNameColumnWidth, kNameColumnWidth, "Name",
                      "Source");
        return std::string(kMenuEntryIndicatorWidth, ' ') + buffer;
    }

    static std::string FormatSavedStationRow(const Station& station)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %s", kCallsignColumnWidth,
                      kCallsignColumnWidth, station.callsign.c_str(), kNameColumnWidth,
                      kNameColumnWidth, station.name.c_str(), station.member_id.c_str());
        return std::string(buffer);
    }

    std::string FormatSavedStationHeaderRow()
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %s", kCallsignColumnWidth,
                      kCallsignColumnWidth, "Callsign", kNameColumnWidth, kNameColumnWidth, "Name",
                      "Member ID");
        return std::string(kMenuEntryIndicatorWidth, ' ') + buffer;
    }

    // `distance_miles` < 0 means "unknown" (the station's own ZIP has no
    // centroid on file) -- shown as "(ULS, nearby)" rather than a fabricated
    // number, since it only passed the coarser ZIP3-prefix pre-filter.
    static std::string FormatUlsSuggestion(const Station& station, double distance_miles)
    {
        char distance_text[32];
        if (distance_miles < 0.0)
        {
            std::snprintf(distance_text, sizeof(distance_text), "nearby");
        }
        else
        {
            std::snprintf(distance_text, sizeof(distance_text), "~%d mi",
                          static_cast<int>(distance_miles));
        }
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s (ULS, %s)", kCallsignColumnWidth,
                      kCallsignColumnWidth, station.callsign.c_str(), kNameColumnWidth,
                      kNameColumnWidth, station.name.c_str(), distance_text);
        return std::string(buffer);
    }

    // Appends stations from `candidates` onto `suggestions` that aren't
    // already present (by callsign), so tier 2 never duplicates a tier 1 hit.
    static void AppendNewSuggestions(std::vector<Station>* suggestions,
                                     const std::vector<Station>& candidates)
    {
        for (const Station& candidate : candidates)
        {
            bool already_included = false;
            for (const Station& existing : *suggestions)
            {
                if (existing.callsign == candidate.callsign)
                {
                    already_included = true;
                    break;
                }
            }
            if (!already_included)
            {
                suggestions->push_back(candidate);
            }
        }
    }

    void RefreshNets(AppState* state)
    {
        state->nets = state->db->GetAllNets();

        state->net_names.clear();
        for (const Net& net : state->nets)
        {
            state->net_names.push_back(net.name);
        }

        if (state->selected_net_index >= static_cast<int>(state->nets.size()))
        {
            state->selected_net_index = 0;
        }
    }

    void ResetCreateNetForm(AppState* state)
    {
        state->new_net_name.clear();
        state->new_net_mode.clear();
        state->new_net_frequency.clear();
        state->new_net_location.clear();
        state->new_net_recurrence.clear();
        state->form_error.clear();
    }

    void ResetStartNetFlow(AppState* state)
    {
        state->selected_role_index = kRoleNetControl;
        // Prefill with the operator's own callsign from Settings; they can
        // still edit it if a different person is filling this particular role.
        state->operator_callsign = state->settings.callsign;
        state->form_error.clear();
    }

    // Sequence numbers are numeric, not a string column, so unlike the other
    // widths above they can't be truncated the same way a long name could --
    // realistic check-in counts never approach 3 digits, so this is purely
    // for lining "#" up with the row's %d field.
    static constexpr int kSequenceColumnWidth = 3;
    // Fits the longest abbreviation ("AltNC", 5 chars) plus one for padding.
    static constexpr int kRoleColumnWidth = 6;

    // Short tag for CheckIn::designated_role, shown in its own column so a
    // station holding Alternate Net Control or Logger is visible straight
    // from the check-in list, not just by opening it to edit. Blank (not
    // "None") for kRoleNone, so an undesignated check-in's row doesn't look
    // busier than one that's just never been looked at.
    static std::string RoleAbbreviation(int role)
    {
        switch (role)
        {
            case kRoleNetControl:
                return "NC";
            case kRoleAlternateNetControl:
                return "AltNC";
            case kRoleLogger:
                return "Log";
            default:
                return "";
        }
    }

    std::string FormatCheckInRow(const CheckIn& check_in, const std::string& name,
                                 const std::string& member_id)
    {
        std::string role = RoleAbbreviation(check_in.designated_role);
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-*d %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %s",
                      kSequenceColumnWidth, check_in.sequence_number, kCallsignColumnWidth,
                      kCallsignColumnWidth, check_in.callsign.c_str(), kNameColumnWidth,
                      kNameColumnWidth, name.c_str(), kMemberIdColumnWidth, kMemberIdColumnWidth,
                      member_id.c_str(), kSignalReportColumnWidth, kSignalReportColumnWidth,
                      check_in.signal_report.c_str(), kRoleColumnWidth, kRoleColumnWidth,
                      role.c_str(), check_in.remarks.c_str());
        return std::string(buffer);
    }

    std::string FormatCheckInHeaderRow(bool above_menu)
    {
        // Same field widths as FormatCheckInRow (with "#" standing in for
        // the sequence number).
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %s",
                      kSequenceColumnWidth, kSequenceColumnWidth, "#", kCallsignColumnWidth,
                      kCallsignColumnWidth, "Callsign", kNameColumnWidth, kNameColumnWidth, "Name",
                      kMemberIdColumnWidth, kMemberIdColumnWidth, "Member ID",
                      kSignalReportColumnWidth, kSignalReportColumnWidth, "Sig", kRoleColumnWidth,
                      kRoleColumnWidth, "Role", "Remarks");
        std::string prefix =
            above_menu ? std::string(kMenuEntryIndicatorWidth, ' ') : std::string();
        return prefix + buffer;
    }

    std::vector<std::string> FormatCheckInRows(Database* db, const std::vector<CheckIn>& check_ins)
    {
        std::vector<std::string> rows;
        for (const CheckIn& check_in : check_ins)
        {
            std::optional<Station> station = db->FindStationByCallsign(check_in.callsign);
            std::string name = station.has_value() ? station->name : "";
            std::string member_id = station.has_value() ? station->member_id : "";
            rows.push_back(FormatCheckInRow(check_in, name, member_id));
        }
        return rows;
    }

    void RefreshActiveCheckIns(AppState* state)
    {
        state->active_check_ins = state->db->GetCheckInsForNetInstance(state->active_instance.id);
        state->active_display_rows = FormatCheckInRows(state->db, state->active_check_ins);

        if (state->selected_check_in_index >= static_cast<int>(state->active_check_ins.size()))
        {
            state->selected_check_in_index = 0;
        }
    }

    void LogOperatorCheckIn(AppState* state)
    {
        Station operator_station;
        operator_station.callsign = state->operator_callsign;

        std::optional<Station> known = state->db->FindStationByCallsign(state->operator_callsign);
        if (known.has_value())
        {
            operator_station = *known;
        }
        else
        {
            std::optional<Station> uls =
                state->db->FindUlsStationByCallsign(state->operator_callsign);
            if (uls.has_value())
            {
                operator_station = *uls;
            }
        }
        operator_station.callsign = state->operator_callsign;

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        state->db->RecordManualCheckInStation(operator_station, now);

        CheckIn check_in;
        check_in.net_instance_id = state->active_instance.id;
        check_in.callsign = state->operator_callsign;
        check_in.sequence_number = 1;
        check_in.checked_in_at = now;
        check_in.designated_role = state->selected_role_index;
        state->db->AddCheckIn(check_in);

        RefreshActiveCheckIns(state);
    }

    void RemoveSelectedCheckIn(AppState* state)
    {
        if (state->active_check_ins.empty())
        {
            state->form_error = "No check-ins to remove.";
            return;
        }

        const CheckIn& selected = state->active_check_ins[state->selected_check_in_index];
        if (selected.designated_role != kRoleNone)
        {
            state->db->SetNetInstanceRoleCallsign(state->active_instance.id,
                                                  selected.designated_role, "");
            std::optional<NetInstance> refreshed =
                state->db->GetNetInstanceById(state->active_instance.id);
            if (refreshed.has_value())
            {
                state->active_instance = *refreshed;
            }
        }
        state->db->DeleteCheckIn(selected.id);
        state->form_error.clear();
        RefreshActiveCheckIns(state);
    }

    // The two roles besides `operator_role`, in canonical order -- always
    // exactly two, since the operator claims exactly one of the three.
    static std::vector<int> AssignableCheckInRoles(int operator_role)
    {
        std::vector<int> roles;
        if (operator_role != kRoleNetControl)
        {
            roles.push_back(kRoleNetControl);
        }
        if (operator_role != kRoleAlternateNetControl)
        {
            roles.push_back(kRoleAlternateNetControl);
        }
        if (operator_role != kRoleLogger)
        {
            roles.push_back(kRoleLogger);
        }
        return roles;
    }

    std::vector<std::string> RoleChoiceLabels(const AppState* state)
    {
        std::vector<std::string> labels{"No additional role"};
        for (int role : AssignableCheckInRoles(state->active_instance.operator_role))
        {
            labels.push_back(state->role_labels[role]);
        }
        return labels;
    }

    int RoleChoiceIndexFromRole(const AppState* state, int role)
    {
        if (role == kRoleNone)
        {
            return 0;
        }
        std::vector<int> roles = AssignableCheckInRoles(state->active_instance.operator_role);
        for (std::size_t i = 0; i < roles.size(); ++i)
        {
            if (roles[i] == role)
            {
                return static_cast<int>(i) + 1;
            }
        }
        return 0;
    }

    int RoleFromRoleChoiceIndex(const AppState* state, int index)
    {
        if (index <= 0)
        {
            return kRoleNone;
        }
        std::vector<int> roles = AssignableCheckInRoles(state->active_instance.operator_role);
        std::size_t choice = static_cast<std::size_t>(index - 1);
        return choice < roles.size() ? roles[choice] : kRoleNone;
    }

    void ApplyCheckInRoleDesignation(AppState* state, std::int64_t check_in_id, int old_role,
                                     int new_role, const std::string& callsign)
    {
        if (old_role == new_role)
        {
            return;
        }
        if (new_role != kRoleNone && new_role == state->active_instance.operator_role)
        {
            return;
        }

        if (old_role != kRoleNone)
        {
            state->db->SetNetInstanceRoleCallsign(state->active_instance.id, old_role, "");
        }
        if (new_role != kRoleNone)
        {
            // Only one check-in per instance can hold a given role -- handing
            // it to this one silently takes it away from whichever check-in
            // held it before.
            state->db->ClearCheckInRoleForInstance(state->active_instance.id, new_role,
                                                   check_in_id);
            state->db->SetNetInstanceRoleCallsign(state->active_instance.id, new_role, callsign);
        }

        std::optional<NetInstance> refreshed =
            state->db->GetNetInstanceById(state->active_instance.id);
        if (refreshed.has_value())
        {
            state->active_instance = *refreshed;
        }
    }

    void ClearModalFields(AppState* state)
    {
        state->modal_station = Station();
        state->modal_signal_report.clear();
        state->modal_remarks.clear();
        state->modal_comment.clear();
        state->modal_callsign_suggestions.clear();
        state->modal_callsign_suggestion_labels.clear();
        state->selected_suggestion_index = 0;
        state->modal_role_choice_labels = RoleChoiceLabels(state);
        state->modal_role_choice_index = 0;
        state->form_error.clear();
    }

    bool LogStationCheckIn(AppState* state)
    {
        if (state->modal_station.callsign.empty())
        {
            state->form_error = "Callsign is required.";
            return false;
        }

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        state->db->RecordManualCheckInStation(state->modal_station, now);

        CheckIn check_in;
        check_in.net_instance_id = state->active_instance.id;
        check_in.callsign = state->modal_station.callsign;
        check_in.sequence_number = static_cast<int>(state->active_check_ins.size()) + 1;
        check_in.signal_report = state->modal_signal_report;
        check_in.remarks = state->modal_remarks;
        check_in.comment = state->modal_comment;
        check_in.checked_in_at = now;
        check_in.designated_role = RoleFromRoleChoiceIndex(state, state->modal_role_choice_index);
        check_in.id = state->db->AddCheckIn(check_in);

        if (check_in.designated_role != kRoleNone)
        {
            ApplyCheckInRoleDesignation(state, check_in.id, kRoleNone, check_in.designated_role,
                                        check_in.callsign);
        }

        RefreshActiveCheckIns(state);
        state->form_error.clear();
        return true;
    }

    void OpenEditCheckInForm(AppState* state, const CheckIn& check_in)
    {
        state->edit_checkin_original = check_in;

        std::optional<Station> station = state->db->FindStationByCallsign(check_in.callsign);
        state->edit_checkin_station = station.has_value() ? *station : Station();
        state->edit_checkin_station.callsign = check_in.callsign;
        state->edit_checkin_signal_report = check_in.signal_report;
        state->edit_checkin_remarks = check_in.remarks;
        state->edit_checkin_comment = check_in.comment;
        state->edit_checkin_role_choice_labels = RoleChoiceLabels(state);
        state->edit_checkin_role_choice_index =
            RoleChoiceIndexFromRole(state, check_in.designated_role);

        state->form_error.clear();
        state->show_edit_checkin_modal = true;
    }

    void SaveEditCheckInForm(AppState* state)
    {
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        state->edit_checkin_station.callsign = state->edit_checkin_original.callsign;
        state->db->UpdateStationFields(state->edit_checkin_station, now);

        int old_role = state->edit_checkin_original.designated_role;
        int new_role = RoleFromRoleChoiceIndex(state, state->edit_checkin_role_choice_index);

        CheckIn check_in = state->edit_checkin_original;
        check_in.signal_report = state->edit_checkin_signal_report;
        check_in.remarks = state->edit_checkin_remarks;
        check_in.comment = state->edit_checkin_comment;
        check_in.designated_role = new_role;
        state->db->UpdateCheckIn(check_in);
        ApplyCheckInRoleDesignation(state, check_in.id, old_role, new_role, check_in.callsign);

        RefreshActiveCheckIns(state);
    }

    void RefreshNetHistory(AppState* state)
    {
        state->history_instances.clear();
        state->history_instance_labels.clear();

        if (state->selected_net_index >= static_cast<int>(state->nets.size()))
        {
            return;
        }

        const Net& net = state->nets[state->selected_net_index];
        state->history_instances = state->db->GetNetInstancesForNet(net.id);

        for (const NetInstance& instance : state->history_instances)
        {
            state->history_instance_labels.push_back(FormatNetInstanceRow(instance));
        }

        if (state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            state->selected_history_index = 0;
        }

        RefreshHistoryCheckIns(state);
    }

    void RefreshHistoryCheckIns(AppState* state)
    {
        state->history_check_in_labels.clear();
        state->selected_history_check_in_index = 0;

        if (state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            return;
        }

        const NetInstance& selected = state->history_instances[state->selected_history_index];
        std::vector<CheckIn> check_ins = state->db->GetCheckInsForNetInstance(selected.id);
        state->history_check_in_labels = FormatCheckInRows(state->db, check_ins);
    }

    void DeleteSelectedNetInstance(AppState* state)
    {
        if (state->history_instances.empty())
        {
            state->form_error = "No net instance to delete.";
            return;
        }

        const NetInstance& selected = state->history_instances[state->selected_history_index];
        if (selected.status == NetInstanceStatus::kOpen)
        {
            state->form_error = "Close this net (from the Active Net page) before deleting it.";
            return;
        }

        state->db->DeleteNetInstance(selected.id);
        state->form_error.clear();
        RefreshNetHistory(state);
    }

    void RefreshCallsignSuggestions(AppState* state)
    {
        state->modal_callsign_suggestions.clear();
        state->modal_callsign_suggestion_labels.clear();
        state->selected_suggestion_index = 0;

        if (state->modal_station.callsign.empty())
        {
            return;
        }

        // Tier 1: callers already known to this specific net (real check-ins
        // or SaveNetStation).
        state->modal_callsign_suggestions = state->db->SearchNetStationsByCallsignSubstring(
            state->active_instance.net_id, state->modal_station.callsign);
        std::size_t tier1_count = state->modal_callsign_suggestions.size();

        // Tier 2: callers known to other nets (today's stand-in for the
        // not-yet-built QRZ/ULS tier -- see SearchStationsByCallsignSubstring).
        std::vector<Station> other_matches =
            state->db->SearchStationsByCallsignSubstring(state->modal_station.callsign);
        AppendNewSuggestions(&state->modal_callsign_suggestions, other_matches);

        constexpr std::size_t kMaxSuggestions = 8;
        if (state->modal_callsign_suggestions.size() > kMaxSuggestions)
        {
            state->modal_callsign_suggestions.resize(kMaxSuggestions);
            tier1_count = std::min(tier1_count, kMaxSuggestions);
        }

        for (std::size_t i = 0; i < state->modal_callsign_suggestions.size(); ++i)
        {
            bool is_this_net = i < tier1_count;
            state->modal_callsign_suggestion_labels.push_back(
                FormatCallsignSuggestion(state->modal_callsign_suggestions[i], is_this_net));
        }
    }

    void ApplySelectedCallsignSuggestion(AppState* state)
    {
        if (state->modal_callsign_suggestions.empty())
        {
            return;
        }

        Station selected = state->modal_callsign_suggestions[state->selected_suggestion_index];
        state->modal_station = selected;

        std::string default_remarks =
            state->db->GetSavedNetStationRemarks(state->active_instance.net_id, selected.callsign);
        if (!default_remarks.empty())
        {
            state->modal_remarks = default_remarks;
        }

        state->modal_callsign_suggestions.clear();
        state->modal_callsign_suggestion_labels.clear();
    }

    void OpenEditNetForm(AppState* state, const Net& net)
    {
        state->edit_net_id = net.id;
        state->edit_net_name = net.name;
        state->edit_net_mode = net.mode;
        state->edit_net_frequency = net.default_frequency;
        state->edit_net_location = net.default_location;
        state->edit_net_recurrence = net.recurrence_description;

        state->saved_station = Station();
        state->saved_station_remarks.clear();
        state->form_error.clear();
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();

        RefreshEditNetSavedStations(state);
        RefreshNearbyZip3Prefixes(state);
    }

    bool SaveEditNetForm(AppState* state)
    {
        if (state->edit_net_name.empty())
        {
            state->form_error = "Net name is required.";
            return false;
        }

        Net net;
        net.id = state->edit_net_id;
        net.name = state->edit_net_name;
        net.mode = state->edit_net_mode;
        net.default_frequency = state->edit_net_frequency;
        net.default_location = state->edit_net_location;
        net.recurrence_description = state->edit_net_recurrence;
        state->db->UpdateNet(net);

        RefreshNets(state);
        state->form_error.clear();
        return true;
    }

    void RefreshEditNetSavedStations(AppState* state)
    {
        state->edit_net_saved_stations = state->db->GetSavedStationsForNet(state->edit_net_id);

        state->edit_net_saved_station_labels.clear();
        for (const Station& station : state->edit_net_saved_stations)
        {
            state->edit_net_saved_station_labels.push_back(FormatSavedStationRow(station));
        }

        if (state->selected_saved_station_index >=
            static_cast<int>(state->edit_net_saved_stations.size()))
        {
            state->selected_saved_station_index = 0;
        }
    }

    bool SaveNetStationForm(AppState* state)
    {
        if (state->saved_station.callsign.empty())
        {
            state->form_error = "Callsign is required.";
            return false;
        }

        bool already_saved = false;
        for (const Station& existing : state->edit_net_saved_stations)
        {
            if (CallsignsEqual(existing.callsign, state->saved_station.callsign))
            {
                already_saved = true;
                break;
            }
        }

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (already_saved)
        {
            state->db->UpdateSavedNetStation(state->edit_net_id, state->saved_station,
                                             state->saved_station_remarks, now);
        }
        else
        {
            state->db->SaveNetStation(state->edit_net_id, state->saved_station,
                                      state->saved_station_remarks, now);
        }

        state->saved_station = Station();
        state->saved_station_remarks.clear();
        state->form_error.clear();
        RefreshEditNetSavedStations(state);
        return true;
    }

    void LoadSavedStationIntoForm(AppState* state, const Station& saved)
    {
        state->saved_station = saved;
        state->saved_station_remarks =
            state->db->GetSavedNetStationRemarks(state->edit_net_id, saved.callsign);
        state->form_error.clear();
    }

    void RemoveSelectedSavedNetStation(AppState* state)
    {
        if (state->edit_net_saved_stations.empty())
        {
            state->form_error = "No saved stations to remove.";
            return;
        }

        const Station& selected =
            state->edit_net_saved_stations[state->selected_saved_station_index];
        state->db->RemoveSavedNetStation(state->edit_net_id, selected.callsign);
        state->form_error.clear();
        RefreshEditNetSavedStations(state);
    }

    void DeleteSelectedSavedStationCompletely(AppState* state)
    {
        if (state->edit_net_saved_stations.empty())
        {
            state->form_error = "No saved stations to delete.";
            return;
        }

        const Station& selected =
            state->edit_net_saved_stations[state->selected_saved_station_index];
        if (!state->db->DeleteStationCompletely(selected.callsign))
        {
            state->form_error = selected.callsign +
                                " has check-in history and can't be fully deleted; use Remove "
                                "to un-save it from this net instead.";
            return;
        }

        state->form_error.clear();
        RefreshEditNetSavedStations(state);
    }

    // Loads AppState::zip_centroids_cache/_by_zip from the database exactly
    // once per process run (ZIP centroids are effectively static once the
    // one-time ULS/geocode import has populated them -- see uls_import.hpp),
    // so proximity lookups never re-query the database after the first call.
    // If the table is still empty (import not finished yet), leaves the
    // cache empty too and simply retries next call -- cheap, since an empty
    // table scan is trivial, and self-correcting once data arrives.
    static void EnsureZipCentroidsCached(AppState* state)
    {
        if (!state->zip_centroids_cache.empty())
        {
            return;
        }
        state->zip_centroids_cache = state->db->GetAllZipCentroids();
        state->zip_centroids_by_zip.clear();
        for (const ZipCentroid& centroid : state->zip_centroids_cache)
        {
            state->zip_centroids_by_zip[centroid.zip] = centroid;
        }
    }

    void RefreshNearbyZip3Prefixes(AppState* state)
    {
        state->saved_station_nearby_zip3_prefixes.clear();

        EnsureZipCentroidsCached(state);
        std::unordered_map<std::string, ZipCentroid>::const_iterator origin_it =
            state->zip_centroids_by_zip.find(state->settings.location);
        if (origin_it == state->zip_centroids_by_zip.end())
        {
            return;
        }

        state->saved_station_nearby_zip3_prefixes = NearbyZip3Prefixes(
            origin_it->second.lat, origin_it->second.lon, state->zip_centroids_cache);
    }

    void RefreshSavedStationSuggestions(AppState* state)
    {
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->selected_saved_station_suggestion_index = 0;

        if (state->saved_station.callsign.empty())
        {
            return;
        }

        constexpr std::size_t kMaxSuggestions = 8;

        // Tier 1: callers already known to this specific net (real check-ins
        // or previously saved) -- same query as the New Station modal's tier 1.
        state->saved_station_suggestions = state->db->SearchNetStationsByCallsignSubstring(
            state->edit_net_id, state->saved_station.callsign);
        std::size_t tier1_count = state->saved_station_suggestions.size();

        // Tier 2: callers known to other nets.
        std::vector<Station> other_matches =
            state->db->SearchStationsByCallsignSubstring(state->saved_station.callsign);
        AppendNewSuggestions(&state->saved_station_suggestions, other_matches);

        if (state->saved_station_suggestions.size() > kMaxSuggestions)
        {
            state->saved_station_suggestions.resize(kMaxSuggestions);
            tier1_count = std::min(tier1_count, kMaxSuggestions);
        }

        for (std::size_t i = 0; i < state->saved_station_suggestions.size(); ++i)
        {
            bool is_this_net = i < tier1_count;
            state->saved_station_suggestion_labels.push_back(
                FormatCallsignSuggestion(state->saved_station_suggestions[i], is_this_net));
        }

        // Tier 3: nearby ULS-imported stations (only meaningful if the
        // operator has a resolvable location set), appended after the
        // known-station tiers and never duplicating a callsign already found.
        if (state->saved_station_suggestions.size() < kMaxSuggestions &&
            !state->saved_station_nearby_zip3_prefixes.empty())
        {
            std::vector<Station> uls_candidates =
                state->db->SearchUlsStationsByCallsignAndZip3Prefixes(
                    state->saved_station.callsign, state->saved_station_nearby_zip3_prefixes);
            EnsureZipCentroidsCached(state);
            std::unordered_map<std::string, ZipCentroid>::const_iterator origin_it =
                state->zip_centroids_by_zip.find(state->settings.location);
            bool has_origin = origin_it != state->zip_centroids_by_zip.end();

            for (const Station& candidate : uls_candidates)
            {
                if (state->saved_station_suggestions.size() >= kMaxSuggestions)
                {
                    break;
                }

                bool already_known = false;
                for (const Station& existing : state->saved_station_suggestions)
                {
                    if (existing.callsign == candidate.callsign)
                    {
                        already_known = true;
                        break;
                    }
                }
                if (already_known)
                {
                    continue;
                }

                double distance_miles = -1.0;
                if (has_origin)
                {
                    std::unordered_map<std::string, ZipCentroid>::const_iterator candidate_it =
                        state->zip_centroids_by_zip.find(candidate.zip);
                    if (candidate_it != state->zip_centroids_by_zip.end())
                    {
                        distance_miles =
                            DistanceMiles(origin_it->second.lat, origin_it->second.lon,
                                          candidate_it->second.lat, candidate_it->second.lon);
                        if (distance_miles > kNearbyRadiusMiles)
                        {
                            // The ZIP3 prefix matched but this specific ZIP's
                            // exact centroid is outside the real radius.
                            continue;
                        }
                    }
                }

                state->saved_station_suggestions.push_back(candidate);
                state->saved_station_suggestion_labels.push_back(
                    FormatUlsSuggestion(candidate, distance_miles));
            }
        }
    }

    void ApplySelectedSavedStationSuggestion(AppState* state)
    {
        if (state->saved_station_suggestions.empty())
        {
            return;
        }

        state->saved_station =
            state->saved_station_suggestions[state->selected_saved_station_suggestion_index];
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
    }

}  // namespace ql
