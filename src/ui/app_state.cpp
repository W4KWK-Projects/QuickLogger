#include "app_state.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <optional>
#include <utility>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

#include "../callsign_rules.hpp"
#include "../date_utils.hpp"
#include "../file_export.hpp"
#include "../geo_utils.hpp"
#include "../net_slice.hpp"
#include "../public_key.hpp"
#include "../text_utils.hpp"
#include "../zmodem_send.hpp"

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
    static constexpr int kCountyColumnWidth = 14;
    // The net-history columns below are kept tight enough that a whole row
    // (with the End column) still fits an 80-column terminal.
    static constexpr int kDateColumnWidth = 10;
    // "03:42 PM" -- the net's start time, right after its date. Blank for a
    // net logged before start times were recorded.
    static constexpr int kStartTimeColumnWidth = 8;
    // "05:10 PM" -- when the net was closed. Blank while it's still open.
    static constexpr int kEndTimeColumnWidth = 8;
    // Sized to fit the longest header label that lands in each of these
    // columns ("Alternate NC", 12 chars; the space between columns is the
    // padding) -- the row shows the bare callsign, no "NC:"/"Alt:"/"Log:"
    // prefix, since the header above it already names the column.
    static constexpr int kNetControlColumnWidth = 12;
    static constexpr int kAlternateNcColumnWidth = 12;
    static constexpr int kLoggerColumnWidth = 12;

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
        std::string start_time = FormatLocalTimeOfDay(instance.started_at);
        std::string end_time = FormatLocalTimeOfDay(instance.closed_at);
        std::snprintf(
            buffer, sizeof(buffer), "%-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %s",
            kDateColumnWidth, kDateColumnWidth, instance.instance_date.c_str(),
            kStartTimeColumnWidth, kStartTimeColumnWidth, start_time.c_str(), kEndTimeColumnWidth,
            kEndTimeColumnWidth, end_time.c_str(), kNetControlColumnWidth, kNetControlColumnWidth,
            instance.net_control_callsign.c_str(), kAlternateNcColumnWidth, kAlternateNcColumnWidth,
            instance.alternate_net_control_callsign.c_str(), kLoggerColumnWidth, kLoggerColumnWidth,
            instance.logger_callsign.c_str(), status);
        return std::string(buffer);
    }

    std::string FormatNetInstanceHeaderRow()
    {
        // Same field widths as FormatNetInstanceRow.
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %s",
                      kDateColumnWidth, kDateColumnWidth, "Date", kStartTimeColumnWidth,
                      kStartTimeColumnWidth, "Start", kEndTimeColumnWidth, kEndTimeColumnWidth,
                      "End", kNetControlColumnWidth, kNetControlColumnWidth, "Net Control",
                      kAlternateNcColumnWidth, kAlternateNcColumnWidth, "Alternate NC",
                      kLoggerColumnWidth, kLoggerColumnWidth, "Logger", "Status");
        return std::string(kMenuEntryIndicatorWidth, ' ') + buffer;
    }

    // The ad hoc history's rows: every ad hoc net's sessions in one list, so
    // the net's name takes the place of the Alternate NC and Logger columns
    // (keeping a row within 80 columns).
    static constexpr int kAdHocNetNameColumnWidth = 24;

    static std::string FormatAdHocInstanceRow(const NetInstance& instance,
                                              const std::string& net_name)
    {
        const char* status = instance.status == NetInstanceStatus::kOpen ? "OPEN" : "closed";
        char buffer[256];
        std::string start_time = FormatLocalTimeOfDay(instance.started_at);
        std::string end_time = FormatLocalTimeOfDay(instance.closed_at);
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %s",
                      kDateColumnWidth, kDateColumnWidth, instance.instance_date.c_str(),
                      kStartTimeColumnWidth, kStartTimeColumnWidth, start_time.c_str(),
                      kEndTimeColumnWidth, kEndTimeColumnWidth, end_time.c_str(),
                      kAdHocNetNameColumnWidth, kAdHocNetNameColumnWidth, net_name.c_str(),
                      kNetControlColumnWidth, kNetControlColumnWidth,
                      instance.net_control_callsign.c_str(), status);
        return std::string(buffer);
    }

    std::string FormatAdHocInstanceHeaderRow()
    {
        // Same field widths as FormatAdHocInstanceRow.
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %s",
                      kDateColumnWidth, kDateColumnWidth, "Date", kStartTimeColumnWidth,
                      kStartTimeColumnWidth, "Start", kEndTimeColumnWidth, kEndTimeColumnWidth,
                      "End", kAdHocNetNameColumnWidth, kAdHocNetNameColumnWidth, "Net",
                      kNetControlColumnWidth, kNetControlColumnWidth, "Net Control", "Status");
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

    std::string FormatSavedStationHeaderRow(bool above_menu)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s %-*.*s %s", kCallsignColumnWidth,
                      kCallsignColumnWidth, "Callsign", kNameColumnWidth, kNameColumnWidth, "Name",
                      "Member ID");
        std::string prefix =
            above_menu ? std::string(kMenuEntryIndicatorWidth, ' ') : std::string();
        return prefix + buffer;
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

    // Net-list names are padded to the longest (up to this) so the dates
    // after them line up.
    static constexpr int kMaxNetNameColumnWidth = 40;

    static std::string FormatNetListRow(const Net& net, int name_width, bool has_open_session)
    {
        std::string when;
        if (net.imported_at > 0)
        {
            when = "imported " + FormatLocalDate(net.imported_at);
        }
        else if (net.created_at > 0)
        {
            when = "created " + FormatLocalDate(net.created_at);
        }
        if (has_open_session)
        {
            when += when.empty() ? "session open" : ", session open";
        }
        if (when.empty())
        {
            return net.name;
        }
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-*.*s  %s", name_width, name_width,
                      net.name.c_str(), when.c_str());
        return std::string(buffer);
    }

    void RefreshNets(AppState* state)
    {
        state->nets.clear();
        for (const Net& net : state->db->GetAllNets())
        {
            if (!net.is_ad_hoc)
            {
                state->nets.push_back(net);
            }
        }
        std::vector<std::int64_t> open_net_ids = state->db->GetNetIdsWithOpenInstances();

        int name_width = 0;
        for (const Net& net : state->nets)
        {
            name_width = std::max(name_width, static_cast<int>(net.name.size()));
        }
        name_width = std::min(name_width, kMaxNetNameColumnWidth);

        state->net_names.clear();
        for (const Net& net : state->nets)
        {
            bool has_open_session =
                std::find(open_net_ids.begin(), open_net_ids.end(), net.id) != open_net_ids.end();
            state->net_names.push_back(FormatNetListRow(net, name_width, has_open_session));
        }

        if (state->selected_net_index >= static_cast<int>(state->nets.size()))
        {
            state->selected_net_index = 0;
        }
    }

    // "2026-09-24 03:42 PM", or just the date if no start time was recorded.
    static std::string DescribeSessionStart(const NetInstance& instance)
    {
        std::string when = instance.instance_date;
        std::string start_time = FormatLocalTimeOfDay(instance.started_at);
        if (!start_time.empty())
        {
            when += " " + start_time;
        }
        return when;
    }

    // When a closed session ended: "05:10 PM", with its date in front if
    // that's not the day it started (a net that ran past midnight). Empty
    // while it's open.
    static std::string DescribeSessionEnd(const NetInstance& instance)
    {
        if (instance.closed_at <= 0)
        {
            return "";
        }
        std::string end_date = FormatLocalDate(instance.closed_at);
        std::string end_time = FormatLocalTimeOfDay(instance.closed_at);
        return end_date == instance.instance_date ? end_time : end_date + " " + end_time;
    }

    static std::string CountCheckIns(std::size_t count)
    {
        return std::to_string(count) + (count == 1 ? " check-in" : " check-ins");
    }

    static void ShowConfirmPrompt(AppState* state, ConfirmPrompt prompt, const std::string& title,
                                  const std::vector<std::string>& lines)
    {
        state->confirm_prompt = prompt;
        state->confirm_prompt_title = title;
        state->confirm_prompt_lines = lines;
        state->show_confirm_prompt = true;
        state->form_error.clear();
        state->status_message.clear();
    }

    void CancelConfirmPrompt(AppState* state)
    {
        state->show_confirm_prompt = false;
        state->confirm_prompt = ConfirmPrompt::kNone;
    }

    // Asks whether to resume `session` of AppState::start_net, which is still
    // open, or close it and start a new one (ConfirmPrompt::kResumeNet).
    static void OfferToResume(AppState* state, const NetInstance& session)
    {
        state->resume_instance = session;
        std::size_t check_ins = state->db->GetCheckInsForNetInstance(session.id).size();
        ShowConfirmPrompt(
            state, ConfirmPrompt::kResumeNet, "Session Still Open",
            {state->start_net.name + " has a session that's still open: started " +
                 DescribeSessionStart(session) + ", " + CountCheckIns(check_ins) + ".",
             "Resume it to keep logging -- if someone else is logging it right now, you'll "
             "both be adding to the same log. Or close it and start a new session."});
    }

    void StartSelectedNet(AppState* state)
    {
        if (state->nets.empty())
        {
            state->form_error = "Create a recurring net first.";
            return;
        }
        state->start_net = state->nets[state->selected_net_index];

        // Newest first, so this is the most recent session left open.
        std::vector<NetInstance> sessions = state->db->GetNetInstancesForNet(state->start_net.id);
        for (const NetInstance& session : sessions)
        {
            if (session.status == NetInstanceStatus::kOpen)
            {
                OfferToResume(state, session);
                return;
            }
        }

        ResetStartNetFlow(state);
        state->page = kPageSelectRole;
    }

    // "Tailgate Net  started 2026-09-24 06:10 PM, 3 check-ins".
    static std::string FormatOpenAdHocSession(const Net& net, const NetInstance& session,
                                              std::size_t check_ins)
    {
        return net.name + "  started " + DescribeSessionStart(session) + ", " +
               CountCheckIns(check_ins);
    }

    void RefreshOpenAdHocSessions(AppState* state)
    {
        state->open_ad_hoc_sessions.clear();
        state->open_ad_hoc_labels.clear();
        for (const NetInstance& session : state->db->GetAdHocNetInstances())
        {
            if (session.status != NetInstanceStatus::kOpen)
            {
                continue;
            }
            std::optional<Net> net = state->db->GetNetById(session.net_id);
            std::size_t check_ins = state->db->GetCheckInsForNetInstance(session.id).size();
            state->open_ad_hoc_sessions.push_back(session);
            state->open_ad_hoc_labels.push_back(
                FormatOpenAdHocSession(net.has_value() ? *net : Net(), session, check_ins));
        }
        if (state->selected_open_ad_hoc_index >=
            static_cast<int>(state->open_ad_hoc_sessions.size()))
        {
            state->selected_open_ad_hoc_index = 0;
        }
    }

    void StartAdHocNet(AppState* state)
    {
        if (state->new_net_name.empty())
        {
            state->form_error = "Net name is required.";
            return;
        }
        if (!CheckNetZip(state, state->new_net_location))
        {
            return;
        }

        Net net;
        net.name = state->new_net_name;
        net.mode = state->new_net_mode;
        net.default_frequency = state->new_net_frequency;
        net.default_location = state->new_net_location;
        net.created_at = static_cast<std::int64_t>(std::time(nullptr));
        net.is_ad_hoc = true;
        net.id = state->db->CreateNet(net);
        state->start_net = net;

        ResetCreateNetForm(state);
        ResetStartNetFlow(state);
        state->page = kPageSelectRole;
    }

    void ResumeOpenNet(AppState* state)
    {
        CancelConfirmPrompt(state);
        std::optional<NetInstance> session =
            state->db->GetNetInstanceById(state->resume_instance.id);
        if (!session.has_value() || session->status != NetInstanceStatus::kOpen)
        {
            RefreshNets(state);
            state->form_error = "That session has been closed or deleted in the meantime.";
            return;
        }

        state->active_instance = *session;
        std::optional<Net> net = state->db->GetNetById(session->net_id);
        state->active_net_name = net.has_value() ? net->name : "";
        state->active_net_zip = net.has_value() ? net->default_location : "";
        state->active_net_is_ad_hoc = net.has_value() && net->is_ad_hoc;
        // The header shows who started it, in which role.
        state->selected_role_index = session->operator_role;
        if (session->operator_role == kRoleAlternateNetControl)
        {
            state->operator_callsign = session->alternate_net_control_callsign;
        }
        else if (session->operator_role == kRoleLogger)
        {
            state->operator_callsign = session->logger_callsign;
        }
        else
        {
            state->operator_callsign = session->net_control_callsign;
        }
        state->show_new_station_modal = false;
        state->show_edit_checkin_modal = false;
        state->selected_check_in_index = 0;
        ClearModalFields(state);
        RefreshActiveCheckIns(state);
        state->status_message = "Resumed the " + DescribeSessionStart(*session) + " session.";
        state->page = kPageActiveNet;
    }

    void CloseOpenNetAndStartNew(AppState* state)
    {
        CancelConfirmPrompt(state);
        // Nobody closed this session when it ended, so "now" could be days
        // later. Its last check-in is the best record of when it ended (or
        // its start, if nothing was logged).
        std::int64_t ended_at = state->resume_instance.started_at;
        for (const CheckIn& check_in :
             state->db->GetCheckInsForNetInstance(state->resume_instance.id))
        {
            ended_at = std::max(ended_at, check_in.checked_in_at);
        }
        if (ended_at <= 0)
        {
            ended_at = static_cast<std::int64_t>(std::time(nullptr));
        }
        state->db->CloseNetInstance(state->resume_instance.id, ended_at);
        RefreshNets(state);
        ResetStartNetFlow(state);
        state->page = kPageSelectRole;
    }

    // Where the active net's history is: ad hoc nets aren't on the net list.
    static const char* HistoryKeyDescription(const AppState* state)
    {
        return state->active_net_is_ad_hoc ? "F6 on the Ad Hoc Net page" : "F6 on the net list";
    }

    void RequestCloseActiveNet(AppState* state)
    {
        std::string name = state->active_net_name.empty() ? "this net" : state->active_net_name;
        ShowConfirmPrompt(
            state, ConfirmPrompt::kCloseNet, "Close Net",
            {"Close " + name + " (" + CountCheckIns(state->active_check_ins.size()) + ")?",
             std::string("It moves to History (") + HistoryKeyDescription(state) +
                 "), where you can still view and export it. A closed session can't be "
                 "reopened for logging."});
    }

    // Leaves the active net for the net list, closing any dialog on it.
    static void LeaveActiveNet(AppState* state)
    {
        state->watched_instance_id = 0;
        state->active_check_ins.clear();
        state->active_display_rows.clear();
        state->show_new_station_modal = false;
        state->show_edit_checkin_modal = false;
        ClearModalFields(state);
        ResetStartNetFlow(state);
        RefreshNets(state);
        state->page = kPageNetList;
    }

    void CloseActiveNet(AppState* state)
    {
        CancelConfirmPrompt(state);
        std::string closed_name = state->active_net_name;
        bool closed_here = state->db->CloseNetInstance(
            state->active_instance.id, static_cast<std::int64_t>(std::time(nullptr)));
        if (!closed_here)
        {
            EnsureActiveSessionOpen(state, "");
            return;
        }
        std::size_t check_ins =
            state->db->GetCheckInsForNetInstance(state->active_instance.id).size();
        std::string history = HistoryKeyDescription(state);
        LeaveActiveNet(state);
        state->form_error.clear();
        state->status_message = "Closed " + closed_name + " (" + CountCheckIns(check_ins) +
                                "). It's in History (" + history + ").";
    }

    bool EnsureActiveSessionOpen(AppState* state, const std::string& unlogged_callsign)
    {
        std::optional<NetInstance> session =
            state->db->GetNetInstanceById(state->active_instance.id);
        if (session.has_value() && session->status == NetInstanceStatus::kOpen)
        {
            return true;
        }

        std::string name = state->active_net_name.empty() ? "This net" : state->active_net_name;
        std::string message;
        if (!session.has_value())
        {
            message = name +
                      "'s session was deleted by someone else, so nothing more can be "
                      "logged to it.";
        }
        else
        {
            std::string ended = DescribeSessionEnd(*session);
            message = name + " was closed by someone else" + (ended.empty() ? "" : " at " + ended) +
                      ", so nothing more can be logged to it.";
        }
        if (!unlogged_callsign.empty())
        {
            message += " " + unlogged_callsign + " was not logged.";
        }
        message += state->active_net_is_ad_hoc
                       ? " Start a new ad hoc net (F5) to keep logging."
                       : " Start the net again (F3) to begin a new session.";

        LeaveActiveNet(state);
        state->status_message.clear();
        state->form_error = message;
        return false;
    }

    void OpenSettingsForm(AppState* state)
    {
        state->settings_form = state->settings;
        state->settings_time_format_index = state->settings.use_24_hour_clock ? 1 : 0;
    }

    bool SaveSettingsForm(AppState* state)
    {
        if (state->settings_form.callsign.empty())
        {
            state->form_error = "Your callsign is required.";
            return false;
        }
        if (!CheckCallsign(state, state->settings_form.callsign))
        {
            return false;
        }
        if (state->settings_form.location.size() != 5)
        {
            state->form_error = "Your ZIP code is required and must be 5 digits.";
            return false;
        }

        state->settings_form.use_24_hour_clock = state->settings_time_format_index == 1;
        SaveSettings(state->settings_path, state->settings_form);
        state->settings = state->settings_form;
        SetUse24HourClock(state->settings.use_24_hour_clock);
        state->form_error.clear();
        return true;
    }

    bool CheckCallsign(AppState* state, const std::string& callsign)
    {
        if (!IsValidCallsign(callsign))
        {
            state->form_error = callsign + " isn't a valid US or Canadian call sign.";
            return false;
        }
        return true;
    }

    bool CheckNetZip(AppState* state, const std::string& zip)
    {
        if (!zip.empty() && !IsFiveDigitZip(zip))
        {
            state->form_error = "ZIP code must be 5 digits, or left blank.";
            return false;
        }
        return true;
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
                                 const std::string& member_id, const std::string& county)
    {
        std::string role = RoleAbbreviation(check_in.designated_role);
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-*d %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %s",
                      kSequenceColumnWidth, check_in.sequence_number, kCallsignColumnWidth,
                      kCallsignColumnWidth, check_in.callsign.c_str(), kNameColumnWidth,
                      kNameColumnWidth, name.c_str(), kMemberIdColumnWidth, kMemberIdColumnWidth,
                      member_id.c_str(), kCountyColumnWidth, kCountyColumnWidth, county.c_str(),
                      kRoleColumnWidth, kRoleColumnWidth, role.c_str(), check_in.remarks.c_str());
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
                      kMemberIdColumnWidth, kMemberIdColumnWidth, "Member ID", kCountyColumnWidth,
                      kCountyColumnWidth, "County", kRoleColumnWidth, kRoleColumnWidth, "Role",
                      "Remarks");
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
            std::string county = station.has_value() ? station->county : "";
            rows.push_back(FormatCheckInRow(check_in, name, member_id, county));
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

        std::int64_t newest_id = 0;
        for (const CheckIn& check_in : state->active_check_ins)
        {
            newest_id = std::max(newest_id, check_in.id);
        }
        state->shown_check_in_count = static_cast<std::int64_t>(state->active_check_ins.size());
        state->shown_newest_check_in_id = newest_id;
        state->watched_instance_id = state->active_instance.id;
    }

    void RefreshActiveCheckInsFromOthers(AppState* state, std::int64_t instance_id)
    {
        if (state->page != kPageActiveNet || state->active_instance.id != instance_id ||
            state->row_pick_action != RowPickAction::kNone ||
            state->show_row_delete_confirm_modal || state->show_edit_checkin_modal)
        {
            return;
        }
        RefreshActiveCheckIns(state);
        if (state->screen != nullptr)
        {
            state->screen->PostEvent(ftxui::Event::Custom);
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
        BackfillCountyFromZip(state, &operator_station);

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        // SaveNetStation (rather than a bare RecordManualCheckInStation) so
        // logging a check-in also associates the station with this net --
        // otherwise it only ever showed up in future autocomplete for this
        // net (SearchNetStationsByCallsignSubstring also matches on real
        // check-in history), never in the edit-net page's saved-station
        // list or its export, which both query net_saved_stations only.
        std::string existing_remarks = state->db->GetSavedNetStationRemarks(
            state->active_instance.net_id, operator_station.callsign);
        state->db->SaveNetStation(state->active_instance.net_id, operator_station, existing_remarks,
                                  now);

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
        state->modal_station.callsign = NormalizeCallsign(state->modal_station.callsign);
        if (state->modal_station.callsign.empty())
        {
            state->form_error = "Callsign is required.";
            return false;
        }
        if (!CheckCallsign(state, state->modal_station.callsign))
        {
            return false;
        }
        if (!EnsureActiveSessionOpen(state, state->modal_station.callsign))
        {
            return false;
        }

        BackfillCountyFromZip(state, &state->modal_station);
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        // SaveNetStation (rather than a bare RecordManualCheckInStation) so
        // logging a check-in also associates the station with this net, the
        // same reasoning as LogOperatorCheckIn above -- and this check-in's
        // own remarks become the saved station's new default remarks,
        // prefilled next time it's picked via autocomplete for this net.
        state->db->SaveNetStation(state->active_instance.net_id, state->modal_station,
                                  state->modal_remarks, now);

        CheckIn check_in;
        check_in.net_instance_id = state->active_instance.id;
        check_in.callsign = state->modal_station.callsign;
        check_in.signal_report = state->modal_signal_report;
        check_in.remarks = state->modal_remarks;
        check_in.comment = state->modal_comment;
        check_in.checked_in_at = now;
        check_in.designated_role = RoleFromRoleChoiceIndex(state, state->modal_role_choice_index);
        // Numbered one past the highest so far (not the count: after a
        // delete the numbers have a gap), worked out by the database so two
        // people logging this same session (see ResumeOpenNet) can't collide.
        check_in.id = state->db->AddCheckInAtNextSequence(check_in);

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
        // The operator's own check-in holds the role they started the net
        // in, which isn't among the choices the form offers (see
        // RoleChoiceLabels) -- so the form reads "No additional role" for it,
        // and saving that would strip the operator's role from the net.
        if (old_role != kRoleNone && old_role == state->active_instance.operator_role)
        {
            new_role = old_role;
        }

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

        if (state->history_ad_hoc)
        {
            std::unordered_map<std::int64_t, std::string> names;
            for (const Net& net : state->db->GetAllNets())
            {
                names[net.id] = net.name;
            }
            state->history_instances = state->db->GetAdHocNetInstances();
            for (const NetInstance& instance : state->history_instances)
            {
                state->history_instance_labels.push_back(
                    FormatAdHocInstanceRow(instance, names[instance.net_id]));
            }
        }
        else
        {
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
        state->history_check_ins.clear();
        state->selected_history_check_in_index = 0;

        if (state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            return;
        }

        const NetInstance& selected = state->history_instances[state->selected_history_index];
        state->history_check_ins = state->db->GetCheckInsForNetInstance(selected.id);
        state->history_check_in_labels = FormatCheckInRows(state->db, state->history_check_ins);
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
            state->form_error =
                "That session is still open. Resume it (F3 on the net list), close it with F4, "
                "then delete it.";
            return;
        }

        state->db->DeleteNetInstance(selected.id);
        state->form_error.clear();
        RefreshNetHistory(state);
    }

    // Writes `lines` to `path`, then -- only if that succeeded -- also tries
    // to hand the same file to a remote, SSH'd-in user via ZMODEM (see
    // zmodem_send.hpp), since a plain local write alone leaves them with no
    // way to get the file off the machine QuickLogger runs on. Sets
    // AppState::status_message/form_error to describe whichever of the two
    // steps is the more relevant outcome to report: a failed local write is
    // still always an error; ZMODEM being unavailable or timing out is a
    // secondary note on an otherwise-successful export, not a failure of
    // the export itself, so it's folded into status_message rather than
    // form_error.
    void OfferZmodemSend(AppState* state, const std::string& path)
    {
        if (!ZmodemSendAvailable())
        {
#if defined(_WIN32)
            // No ZMODEM on Windows at all (see zmodem_send.cpp), so there's
            // nothing to install.
            state->status_message = "Saved to " + path + ".";
#else
            state->status_message =
                "Saved to " + path + " (install 'sz'/lrzsz for ZMODEM download).";
#endif
            return;
        }

        // Don't send yet -- the transfer hijacks the real terminal for its
        // raw protocol bytes and can't show anything meaningful while it's
        // in flight, so the operator needs a chance to get their client's
        // receive dialog ready (or back out) *before* that happens, not be
        // dropped into it with no warning. ConfirmZmodemAction/
        // CancelZmodemAction (wired to the modal's F2/Enter and Esc) do the
        // actual send.
        state->status_message = "Saved to " + path + ".";
        state->zmodem_action = ZmodemAction::kSend;
        state->zmodem_confirm_path = path;
        state->show_zmodem_confirm_modal = true;
    }

    static void ExportLinesToFile(AppState* state, const std::string& path,
                                  const std::vector<std::string>& lines)
    {
        std::string error;
        if (!WriteExportFile(path, lines, &error))
        {
            state->status_message.clear();
            state->form_error = error;
            return;
        }

        state->form_error.clear();
        OfferZmodemSend(state, path);
    }

    void ConfirmZmodemAction(AppState* state)
    {
        state->show_zmodem_confirm_modal = false;
        std::string error;

        if (state->zmodem_action == ZmodemAction::kSend)
        {
            if (SendFileViaZmodem(state->screen, state->zmodem_confirm_path, &error))
            {
                state->status_message =
                    "Saved to " + state->zmodem_confirm_path + " and sent via ZMODEM.";
            }
            else
            {
                state->status_message =
                    "Saved to " + state->zmodem_confirm_path + " (" + error + ")";
            }
            return;
        }

        if (ReceiveFileViaZmodem(state->screen, ImportsDir(state->db_path), &error))
        {
            RefreshImportNetFiles(state);
            state->form_error.clear();
            state->status_message = "Received a file. Highlight it below and press F2 to import.";
        }
        else
        {
            state->status_message.clear();
            state->form_error = error;
        }
    }

    void CancelZmodemAction(AppState* state)
    {
        state->show_zmodem_confirm_modal = false;
        if (state->zmodem_action == ZmodemAction::kSend)
        {
            state->status_message = "Saved to " + state->zmodem_confirm_path + " (ZMODEM skipped).";
        }
        else
        {
            state->status_message = "ZMODEM receive skipped.";
        }
    }

    void RequestDeleteNet(AppState* state)
    {
        state->show_delete_net_confirm_modal = true;
    }

    void ConfirmDeleteNet(AppState* state)
    {
        state->show_delete_net_confirm_modal = false;
        std::string deleted_name = state->edit_net_name;
        state->db->DeleteNetCompletely(state->edit_net_id);
        RefreshNets(state);
        state->form_error.clear();
        state->status_message = "Deleted \"" + deleted_name + "\" and all of its history.";
        state->page = kPageNetList;
    }

    void CancelDeleteNet(AppState* state)
    {
        state->show_delete_net_confirm_modal = false;
    }

    // ---- Picking a row by number -------------------------------------------

    PickList RowPickListFor(RowPickAction action)
    {
        switch (action)
        {
            case RowPickAction::kEditNet:
                return PickList::kNets;
            case RowPickAction::kEditCheckIn:
            case RowPickAction::kDeleteCheckIn:
                return PickList::kActiveCheckIns;
            case RowPickAction::kEditSavedStation:
            case RowPickAction::kRemoveSavedStation:
                return PickList::kSavedStations;
            case RowPickAction::kDeleteNetInstance:
                return PickList::kNetInstances;
            case RowPickAction::kDeleteHistoryCheckIn:
                return PickList::kHistoryCheckIns;
            case RowPickAction::kRemoveUser:
                return PickList::kUsers;
            case RowPickAction::kResumeAdHocSession:
                return PickList::kOpenAdHocSessions;
            case RowPickAction::kNone:
                break;
        }
        return PickList::kNone;
    }

    static std::size_t PickListSize(const AppState* state, PickList list)
    {
        switch (list)
        {
            case PickList::kNets:
                return state->nets.size();
            case PickList::kActiveCheckIns:
                return state->active_check_ins.size();
            case PickList::kSavedStations:
                return state->edit_net_saved_stations.size();
            case PickList::kNetInstances:
                return state->history_instances.size();
            case PickList::kHistoryCheckIns:
                return state->history_check_ins.size();
            case PickList::kUsers:
                return state->manage_users.size();
            case PickList::kOpenAdHocSessions:
                return state->open_ad_hoc_sessions.size();
            case PickList::kNone:
                break;
        }
        return 0;
    }

    // The variable holding the list's highlighted row -- set to the picked
    // row, so the existing "act on the selected row" functions do the work.
    static int* PickListSelection(AppState* state, PickList list)
    {
        switch (list)
        {
            case PickList::kNets:
                return &state->selected_net_index;
            case PickList::kActiveCheckIns:
                return &state->selected_check_in_index;
            case PickList::kSavedStations:
                return &state->selected_saved_station_index;
            case PickList::kNetInstances:
                return &state->selected_history_index;
            case PickList::kHistoryCheckIns:
                return &state->selected_history_check_in_index;
            case PickList::kUsers:
                return &state->selected_user_index;
            case PickList::kOpenAdHocSessions:
                return &state->selected_open_ad_hoc_index;
            case PickList::kNone:
                break;
        }
        return nullptr;
    }

    // The nouns used in prompts and messages.
    static const char* PickListNoun(PickList list)
    {
        switch (list)
        {
            case PickList::kNets:
                return "net";
            case PickList::kActiveCheckIns:
                return "check-in";
            case PickList::kSavedStations:
                return "station";
            case PickList::kNetInstances:
                return "net session";
            case PickList::kHistoryCheckIns:
                return "check-in";
            case PickList::kUsers:
                return "user";
            case PickList::kOpenAdHocSessions:
                return "open session";
            case PickList::kNone:
                break;
        }
        return "row";
    }

    static const char* RowPickVerb(RowPickAction action)
    {
        switch (action)
        {
            case RowPickAction::kEditNet:
            case RowPickAction::kEditCheckIn:
            case RowPickAction::kEditSavedStation:
                return "Edit";
            case RowPickAction::kRemoveSavedStation:
            case RowPickAction::kRemoveUser:
                return "Remove";
            case RowPickAction::kResumeAdHocSession:
                return "Resume";
            case RowPickAction::kDeleteCheckIn:
            case RowPickAction::kDeleteNetInstance:
            case RowPickAction::kDeleteHistoryCheckIn:
            case RowPickAction::kNone:
                break;
        }
        return "Delete";
    }

    void StartRowPick(AppState* state, RowPickAction action)
    {
        PickList list = RowPickListFor(action);
        std::size_t count = PickListSize(state, list);
        if (count == 0)
        {
            state->status_message.clear();
            std::string verb = RowPickVerb(action);
            verb[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(verb[0])));
            state->form_error =
                std::string("There's no ") + PickListNoun(list) + " to " + verb + ".";
            return;
        }

        // Check-ins already show their own number in the # column, so
        // that's the number to type; every other list is numbered 1, 2, 3...
        state->row_pick_numbers.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            int number = static_cast<int>(i) + 1;
            if (list == PickList::kActiveCheckIns)
            {
                number = state->active_check_ins[i].sequence_number;
            }
            else if (list == PickList::kHistoryCheckIns)
            {
                number = state->history_check_ins[i].sequence_number;
            }
            state->row_pick_numbers.push_back(number);
        }
        state->row_pick_action = action;
        state->row_pick_digits.clear();
        state->form_error.clear();
        state->status_message.clear();
    }

    void CancelRowPick(AppState* state)
    {
        state->row_pick_action = RowPickAction::kNone;
        state->row_pick_digits.clear();
        state->row_pick_numbers.clear();
    }

    // The history page's check-in pane follows the highlighted session.
    static void SyncPickListSideEffects(AppState* state)
    {
        if (RowPickListFor(state->row_pick_action) == PickList::kNetInstances)
        {
            RefreshHistoryCheckIns(state);
        }
    }

    // Moves the list's highlight to the row numbered as typed, if there is one.
    static void HighlightTypedRow(AppState* state)
    {
        int* selection = PickListSelection(state, RowPickListFor(state->row_pick_action));
        if (selection == nullptr || state->row_pick_digits.empty())
        {
            return;
        }
        int typed = 0;
        for (char digit : state->row_pick_digits)
        {
            typed = typed * 10 + (digit - '0');
        }
        for (std::size_t i = 0; i < state->row_pick_numbers.size(); ++i)
        {
            if (state->row_pick_numbers[i] == typed)
            {
                *selection = static_cast<int>(i);
                SyncPickListSideEffects(state);
                return;
            }
        }
    }

    void MoveRowPickHighlight(AppState* state, int delta)
    {
        PickList list = RowPickListFor(state->row_pick_action);
        int* selection = PickListSelection(state, list);
        int count = static_cast<int>(PickListSize(state, list));
        if (selection == nullptr || count == 0)
        {
            return;
        }
        int moved = *selection + delta;
        *selection = moved < 0 ? 0 : (moved >= count ? count - 1 : moved);
        state->row_pick_digits.clear();
        SyncPickListSideEffects(state);
    }

    std::string RowPickVerbFor(RowPickAction action)
    {
        return RowPickVerb(action);
    }

    void TypeRowPickDigit(AppState* state, char digit)
    {
        if (state->row_pick_digits.size() < 4)
        {
            state->row_pick_digits += digit;
            state->form_error.clear();
            HighlightTypedRow(state);
        }
    }

    void EraseRowPickDigit(AppState* state)
    {
        if (!state->row_pick_digits.empty())
        {
            state->row_pick_digits.pop_back();
            HighlightTypedRow(state);
        }
    }

    std::string RowPickPrompt(const AppState* state)
    {
        PickList list = RowPickListFor(state->row_pick_action);
        std::string noun = PickListNoun(list);
        return std::string(RowPickVerb(state->row_pick_action)) + " which " + noun +
               "? Type its number, then Enter (Esc cancels).";
    }

    // Opens the confirmation for deleting row `index` of the action's list,
    // or explains why it can't be deleted.
    static void OpenRowDeleteConfirm(AppState* state, RowPickAction action, int index)
    {
        state->row_delete_lines.clear();
        switch (action)
        {
            case RowPickAction::kDeleteCheckIn:
            {
                const CheckIn& check_in = state->active_check_ins[index];
                std::optional<Station> station =
                    state->db->FindStationByCallsign(check_in.callsign);
                std::string who = check_in.callsign;
                if (station.has_value() && !station->name.empty())
                {
                    who += " (" + station->name + ")";
                }
                state->row_delete_title = "Delete Check-In";
                state->row_delete_lines.emplace_back("Delete check-in #" +
                                                     std::to_string(check_in.sequence_number) +
                                                     ": " + who + "?");
                state->row_delete_lines.emplace_back("It's removed from this net's log.");
                break;
            }
            case RowPickAction::kRemoveSavedStation:
            {
                const Station& station = state->edit_net_saved_stations[index];
                state->row_delete_title = "Remove Saved Station";
                state->row_delete_lines.emplace_back(
                    "Remove " + station.callsign +
                    (station.name.empty() ? "" : " (" + station.name + ")") +
                    " from this net's saved stations?");
                if (state->db->IsStationUsedOutsideNet(station.callsign, state->edit_net_id))
                {
                    state->row_delete_lines.emplace_back(
                        "It's saved to another net or has checked in, so its details stay and "
                        "it still comes up in autocomplete.");
                }
                else
                {
                    state->row_delete_lines.emplace_back(
                        "It isn't saved to any other net and has never checked in, so its "
                        "details (member ID, address...) are deleted too.");
                }
                break;
            }
            case RowPickAction::kDeleteHistoryCheckIn:
            {
                const CheckIn& check_in = state->history_check_ins[index];
                std::optional<Station> station =
                    state->db->FindStationByCallsign(check_in.callsign);
                std::string who = check_in.callsign;
                if (station.has_value() && !station->name.empty())
                {
                    who += " (" + station->name + ")";
                }
                std::string when;
                if (state->selected_history_index <
                    static_cast<int>(state->history_instances.size()))
                {
                    when = " from the " +
                           state->history_instances[state->selected_history_index].instance_date +
                           " session";
                }
                state->row_delete_title = "Delete Check-In";
                state->row_delete_lines.push_back("Delete check-in #" +
                                                  std::to_string(check_in.sequence_number) + ": " +
                                                  who + when + "?");
                state->row_delete_lines.emplace_back("It's removed from that net's log.");
                break;
            }
            case RowPickAction::kDeleteNetInstance:
            {
                const NetInstance& instance = state->history_instances[index];
                if (instance.status == NetInstanceStatus::kOpen)
                {
                    state->form_error =
                        std::string("That session is still open. Resume it (F3 on the ") +
                        (state->history_ad_hoc ? "Ad Hoc Net page" : "net list") +
                        "), close it with F4, then delete it.";
                    return;
                }
                std::size_t check_ins = state->db->GetCheckInsForNetInstance(instance.id).size();
                std::string ended = DescribeSessionEnd(instance);
                state->row_delete_title = "Delete Net Session";
                state->row_delete_lines.emplace_back(
                    "Delete the " + DescribeSessionStart(instance) + " session (" +
                    (ended.empty() ? "" : "ended " + ended + ", ") + CountCheckIns(check_ins) +
                    ")?");
                state->row_delete_lines.emplace_back("Its whole log is deleted.");
                break;
            }
            case RowPickAction::kRemoveUser:
            {
                state->row_delete_title = "Remove SSH User";
                state->row_delete_lines.emplace_back("Remove " +
                                                     state->manage_users[index].username + "?");
                state->row_delete_lines.emplace_back("They won't be able to log in over SSH.");
                break;
            }
            case RowPickAction::kNone:
            case RowPickAction::kEditNet:
            case RowPickAction::kEditCheckIn:
            case RowPickAction::kEditSavedStation:
            case RowPickAction::kResumeAdHocSession:
                return;
        }
        state->row_delete_lines.emplace_back("This can't be undone.");
        state->row_delete_action = action;
        state->row_delete_index = index;
        state->show_row_delete_confirm_modal = true;
    }

    void FinishRowPick(AppState* state)
    {
        RowPickAction action = state->row_pick_action;
        PickList list = RowPickListFor(action);
        int* selection = PickListSelection(state, list);
        if (selection == nullptr)
        {
            CancelRowPick(state);
            return;
        }

        int index = *selection;
        if (!state->row_pick_digits.empty())
        {
            // Only digits can be typed in pick mode (see HandleRowPickKey),
            // at most a few of them.
            int typed = 0;
            for (char digit : state->row_pick_digits)
            {
                typed = typed * 10 + (digit - '0');
            }
            index = -1;
            for (std::size_t i = 0; i < state->row_pick_numbers.size(); ++i)
            {
                if (state->row_pick_numbers[i] == typed)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index < 0)
            {
                state->form_error = "There's no " + std::string(PickListNoun(list)) + " #" +
                                    state->row_pick_digits + ".";
                state->row_pick_digits.clear();
                return;
            }
        }
        if (index < 0 || index >= static_cast<int>(PickListSize(state, list)))
        {
            CancelRowPick(state);
            return;
        }

        CancelRowPick(state);
        state->form_error.clear();
        *selection = index;
        switch (action)
        {
            case RowPickAction::kEditNet:
                OpenEditNetForm(state, state->nets[index]);
                state->page = kPageEditNet;
                if (state->edit_net_name_input)
                {
                    state->edit_net_name_input->TakeFocus();
                }
                return;
            case RowPickAction::kEditCheckIn:
                OpenEditCheckInForm(state, state->active_check_ins[index]);
                return;
            case RowPickAction::kResumeAdHocSession:
            {
                const NetInstance& session = state->open_ad_hoc_sessions[index];
                std::optional<Net> net = state->db->GetNetById(session.net_id);
                if (!net.has_value())
                {
                    RefreshOpenAdHocSessions(state);
                    state->form_error = "That net has been deleted in the meantime.";
                    return;
                }
                state->start_net = *net;
                OfferToResume(state, session);
                return;
            }
            case RowPickAction::kEditSavedStation:
                LoadSavedStationIntoForm(state, state->edit_net_saved_stations[index]);
                return;
            default:
                if (list == PickList::kNetInstances)
                {
                    RefreshHistoryCheckIns(state);
                }
                OpenRowDeleteConfirm(state, action, index);
                return;
        }
    }

    void ConfirmRowDelete(AppState* state)
    {
        RowPickAction action = state->row_delete_action;
        int index = state->row_delete_index;
        state->show_row_delete_confirm_modal = false;
        state->row_delete_action = RowPickAction::kNone;
        state->row_delete_index = -1;

        int* selection = PickListSelection(state, RowPickListFor(action));
        if (selection == nullptr || index < 0 ||
            index >= static_cast<int>(PickListSize(state, RowPickListFor(action))))
        {
            return;
        }
        *selection = index;
        switch (action)
        {
            case RowPickAction::kDeleteCheckIn:
                RemoveSelectedCheckIn(state);
                state->status_message = "Check-in deleted.";
                break;
            case RowPickAction::kRemoveSavedStation:
                RemoveSelectedSavedNetStation(state);
                break;
            case RowPickAction::kDeleteHistoryCheckIn:
                DeleteSelectedHistoryCheckIn(state);
                state->status_message = "Check-in deleted.";
                break;
            case RowPickAction::kDeleteNetInstance:
                DeleteSelectedNetInstance(state);
                if (state->form_error.empty())
                {
                    state->status_message = "Net session deleted.";
                }
                break;
            case RowPickAction::kRemoveUser:
                RemoveSelectedUser(state);
                break;
            default:
                break;
        }
        // Keep the highlight on a row that still exists.
        std::size_t remaining = PickListSize(state, RowPickListFor(action));
        if (*selection >= static_cast<int>(remaining))
        {
            *selection = remaining == 0 ? 0 : static_cast<int>(remaining) - 1;
        }
    }

    void CancelRowDelete(AppState* state)
    {
        state->show_row_delete_confirm_modal = false;
        state->row_delete_action = RowPickAction::kNone;
        state->row_delete_index = -1;
    }

    static std::string FormatUserLabel(const User& user)
    {
        std::string last_login = "never logged in";
        if (user.last_login_at > 0)
        {
            last_login = "last login " + FormatLocalDateTime(user.last_login_at);
        }
        return user.username + "  (" + last_login + ")";
    }

    void RefreshUsers(AppState* state)
    {
        state->manage_users = state->db->ListUsers();
        state->manage_users_labels.clear();
        for (const User& user : state->manage_users)
        {
            state->manage_users_labels.push_back(FormatUserLabel(user));
        }
        if (state->selected_user_index >= static_cast<int>(state->manage_users.size()))
        {
            state->selected_user_index = 0;
        }
    }

    void AddUserFromForm(AppState* state)
    {
        if (state->new_user_username.empty() || state->new_user_public_key.empty())
        {
            state->status_message.clear();
            state->form_error = "Username and public key are both required.";
            return;
        }
        std::string public_key;
        std::string key_error;
        if (!ValidatePublicKey(state->new_user_public_key, &public_key, &key_error))
        {
            state->status_message.clear();
            state->form_error = key_error;
            return;
        }

        User user;
        user.username = state->new_user_username;
        user.public_key = public_key;
        user.created_at = static_cast<std::int64_t>(std::time(nullptr));
        state->db->CreateUser(user);

        state->new_user_username.clear();
        state->new_user_public_key.clear();
        RefreshUsers(state);
        state->form_error.clear();
        state->status_message = "Added \"" + user.username + "\".";
    }

    void RemoveSelectedUser(AppState* state)
    {
        if (state->manage_users.empty())
        {
            return;
        }
        std::string username = state->manage_users[state->selected_user_index].username;
        state->db->DeleteUser(username);
        RefreshUsers(state);
        state->form_error.clear();
        state->status_message = "Removed \"" + username + "\".";
    }

    void ExportNetLog(AppState* state, const std::string& net_name, const NetInstance& instance,
                      const std::vector<CheckIn>& check_ins)
    {
        std::vector<std::string> lines;
        lines.push_back("Net: " + net_name);
        lines.push_back("Date: " + instance.instance_date);
        if (instance.started_at > 0)
        {
            lines.push_back("Start Time: " + FormatLocalTimeOfDay(instance.started_at));
        }
        if (instance.closed_at > 0)
        {
            lines.push_back("End Time: " + DescribeSessionEnd(instance));
        }
        lines.push_back("Net Control: " + instance.net_control_callsign);
        lines.push_back("Alternate NC: " + instance.alternate_net_control_callsign);
        lines.push_back("Logger: " + instance.logger_callsign);
        lines.push_back(std::string("Status: ") +
                        (instance.status == NetInstanceStatus::kOpen ? "OPEN" : "closed"));
        lines.emplace_back("");
        lines.push_back(FormatCheckInHeaderRow(/*above_menu=*/false));
        std::vector<std::string> rows = FormatCheckInRows(state->db, check_ins);
        lines.insert(lines.end(), rows.begin(), rows.end());

        std::string path = ExportsDir(state->db_path) + "/" + SanitizeFilenameComponent(net_name) +
                           "_" + SanitizeFilenameComponent(instance.instance_date) + "_log.txt";
        ExportLinesToFile(state, path, lines);
    }

    void ExportSavedStations(AppState* state, const std::string& net_name,
                             const std::vector<Station>& saved_stations)
    {
        std::vector<std::string> lines;
        lines.push_back("Net: " + net_name);
        lines.emplace_back("Saved Stations:");
        lines.emplace_back("");
        lines.push_back(FormatSavedStationHeaderRow(/*above_menu=*/false));
        for (const Station& station : saved_stations)
        {
            lines.push_back(FormatSavedStationRow(station));
        }

        std::string path = ExportsDir(state->db_path) + "/" + SanitizeFilenameComponent(net_name) +
                           "_saved_stations.txt";
        ExportLinesToFile(state, path, lines);
    }

    void ExportNetSlice(AppState* state, const Net& net)
    {
        NetSlice slice = GatherNetSlice(state->db, net.id);
        std::string path =
            ExportsDir(state->db_path) + "/" + SanitizeFilenameComponent(net.name) + ".qlnet";

        std::string error;
        if (!WriteNetSliceFile(path, slice, &error))
        {
            state->status_message.clear();
            state->form_error = error;
            return;
        }

        state->form_error.clear();
        OfferZmodemSend(state, path);
    }

    void RefreshImportNetFiles(AppState* state)
    {
        state->import_net_files = ListFilesWithExtension(ImportsDir(state->db_path), ".qlnet");
        if (state->selected_import_file_index >= static_cast<int>(state->import_net_files.size()))
        {
            state->selected_import_file_index = 0;
        }
    }

    void ImportSelectedNetSlice(AppState* state)
    {
        if (state->import_net_files.empty())
        {
            state->status_message.clear();
            state->form_error = "No net-export files found in imports/.";
            return;
        }

        std::string path = ImportsDir(state->db_path) + "/" +
                           state->import_net_files[state->selected_import_file_index];
        std::string error;
        std::optional<NetSlice> slice = ReadNetSliceFile(path, &error);
        if (!slice.has_value())
        {
            state->status_message.clear();
            state->form_error = error;
            return;
        }

        try
        {
            ApplyNetSlice(state->db, *slice, static_cast<std::int64_t>(std::time(nullptr)));
        }
        catch (const std::exception& e)
        {
            RefreshNets(state);
            state->status_message.clear();
            state->form_error = std::string("Import failed: ") + e.what();
            return;
        }
        RefreshNets(state);
        state->form_error.clear();
        state->status_message = "Imported \"" + slice->net.name +
                                "\". It's listed with today's date as \"imported\", so you can "
                                "tell it apart from a net of the same name.";
        state->page = kPageNetList;
    }

    void StartZmodemReceive(AppState* state)
    {
        state->zmodem_action = ZmodemAction::kReceive;
        state->show_zmodem_confirm_modal = true;
    }

    static void AppendNearbyUlsSuggestions(AppState* state, const std::string& typed,
                                           const std::string& net_zip, std::size_t max_suggestions,
                                           std::vector<Station>* suggestions,
                                           std::vector<std::string>* labels);

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

        // Tier 2: callers known to other nets (see
        // SearchStationsByCallsignSubstring).
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

        // Tier 3: licensed stations near the net, from the FCC data.
        AppendNearbyUlsSuggestions(state, state->modal_station.callsign, state->active_net_zip,
                                   kMaxSuggestions, &state->modal_callsign_suggestions,
                                   &state->modal_callsign_suggestion_labels);
    }

    void ApplySelectedCallsignSuggestion(AppState* state)
    {
        if (state->modal_callsign_suggestions.empty())
        {
            return;
        }

        state->modal_station = state->modal_callsign_suggestions[state->selected_suggestion_index];
        // Fill County in now, so it shows in the form as soon as the station
        // is picked rather than only once it's saved.
        BackfillCountyFromZip(state, &state->modal_station);

        std::string default_remarks = state->db->GetSavedNetStationRemarks(
            state->active_instance.net_id, state->modal_station.callsign);
        if (!default_remarks.empty())
        {
            state->modal_remarks = std::move(default_remarks);
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

        CloseSavedStationForm(state);
        state->status_message.clear();

        RefreshEditNetSavedStations(state);
        RefreshNearbyZips(state, state->edit_net_location);
    }

    bool SaveEditNetForm(AppState* state)
    {
        if (state->edit_net_name.empty())
        {
            state->form_error = "Net name is required.";
            return false;
        }
        if (!CheckNetZip(state, state->edit_net_location))
        {
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
        state->status_message = "Saved " + net.name + ".";
        state->page = kPageNetList;
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
        state->saved_station.callsign = NormalizeCallsign(state->saved_station.callsign);
        if (state->saved_station.callsign.empty())
        {
            state->form_error = "Callsign is required.";
            return false;
        }
        if (!CheckCallsign(state, state->saved_station.callsign))
        {
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

        BackfillCountyFromZip(state, &state->saved_station);
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

        state->status_message = "Saved " + state->saved_station.callsign + ".";
        state->saved_station = Station();
        state->saved_station_remarks.clear();
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->form_error.clear();
        RefreshEditNetSavedStations(state);

        // Re-focus the callsign field for the next entry -- adding several
        // stations in a row is the common case here, and without this the
        // operator would have to Tab all the way back down from wherever
        // focus landed after the save.
        if (state->saved_station_callsign_input)
        {
            state->saved_station_callsign_input->TakeFocus();
        }
        return true;
    }

    void LoadSavedStationIntoForm(AppState* state, const Station& saved)
    {
        CloseSavedStationForm(state);
        state->saved_station = saved;
        state->saved_station_remarks =
            state->db->GetSavedNetStationRemarks(state->edit_net_id, saved.callsign);
        state->show_saved_station_modal = true;
        if (state->saved_station_callsign_input)
        {
            state->saved_station_callsign_input->TakeFocus();
        }
    }

    void OpenNewSavedStationForm(AppState* state)
    {
        CloseSavedStationForm(state);
        state->status_message.clear();
        state->show_saved_station_modal = true;
        if (state->saved_station_callsign_input)
        {
            state->saved_station_callsign_input->TakeFocus();
        }
    }

    void CloseSavedStationForm(AppState* state)
    {
        state->saved_station = Station();
        state->saved_station_remarks.clear();
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->form_error.clear();
        state->show_saved_station_modal = false;
    }

    void RemoveSelectedSavedNetStation(AppState* state)
    {
        if (state->edit_net_saved_stations.empty())
        {
            state->form_error = "No saved stations to remove.";
            return;
        }

        std::string callsign =
            state->edit_net_saved_stations[state->selected_saved_station_index].callsign;
        state->db->RemoveSavedNetStation(state->edit_net_id, callsign);
        state->form_error.clear();
        state->status_message = state->db->FindStationByCallsign(callsign).has_value()
                                    ? "Removed " + callsign + " from this net's saved stations."
                                    : "Removed " + callsign +
                                          "; it wasn't used anywhere else, so its details were "
                                          "deleted too.";
        RefreshEditNetSavedStations(state);
    }

    void DeleteSelectedHistoryCheckIn(AppState* state)
    {
        if (state->history_check_ins.empty() ||
            state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            state->form_error = "No check-in to delete.";
            return;
        }

        const CheckIn& check_in = state->history_check_ins[state->selected_history_check_in_index];
        if (check_in.designated_role != kRoleNone)
        {
            state->db->SetNetInstanceRoleCallsign(
                state->history_instances[state->selected_history_index].id,
                check_in.designated_role, "");
        }
        state->db->DeleteCheckIn(check_in.id);
        state->form_error.clear();
        RefreshNetHistory(state);
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

    // Same rationale and lazy/self-correcting behavior as
    // EnsureZipCentroidsCached above, for AppState::zip_county_by_zip and
    // zip_place_county_by_key (loaded together; the second is legitimately
    // small, so emptiness of the first is what says "not loaded yet").
    static void EnsureZipCountiesCached(AppState* state)
    {
        if (!state->zip_county_by_zip.empty())
        {
            return;
        }
        std::vector<ZipCounty> zip_counties = state->db->GetAllZipCounties();
        for (const ZipCounty& zip_county : zip_counties)
        {
            state->zip_county_by_zip[zip_county.zip] = zip_county.county;
        }
        std::vector<ZipPlaceCounty> zip_places = state->db->GetAllZipPlaceCounties();
        for (const ZipPlaceCounty& zip_place : zip_places)
        {
            state->zip_place_county_by_key[zip_place.zip + "|" + zip_place.place] =
                zip_place.county;
        }
    }

    void RefreshNearbyZips(AppState* state, const std::string& net_zip)
    {
        EnsureZipCentroidsCached(state);
        std::unordered_map<std::string, ZipCentroid>::const_iterator origin_it =
            state->zip_centroids_by_zip.end();
        if (IsFiveDigitZip(net_zip))
        {
            origin_it = state->zip_centroids_by_zip.find(net_zip);
        }
        if (origin_it == state->zip_centroids_by_zip.end())
        {
            origin_it = state->zip_centroids_by_zip.find(state->settings.location);
        }
        std::string origin = origin_it == state->zip_centroids_by_zip.end() ? "" : origin_it->first;

        if (state->nearby_zips_origin == origin && !state->nearby_zips.empty())
        {
            return;
        }
        state->nearby_zips.clear();
        state->nearby_zip3_prefixes.clear();
        state->nearby_zips_origin = origin;
        if (origin_it == state->zip_centroids_by_zip.end())
        {
            return;
        }

        state->nearby_zips =
            NearbyZips(origin_it->second.lat, origin_it->second.lon, state->zip_centroids_cache);
        state->nearby_zip3_prefixes = NearbyZip3Prefixes(
            origin_it->second.lat, origin_it->second.lon, state->zip_centroids_cache);
    }

    // How long AppState::nearby_uls_callsigns is trusted before it's
    // reloaded, so a long session picks up the weekly station data refresh.
    static constexpr std::int64_t kNearbyUlsReloadSeconds = 3600;

    // Autocomplete's last tier, shared by the New Station modal and the
    // saved-station form: ULS-imported stations matching `typed` whose ZIP is
    // within geo_utils::kNearbyRadiusMiles of the net's ZIP (`net_zip`) or,
    // failing that, the operator's own, nearest first (see RefreshNearbyZips
    // and Database::SearchNearbyUlsStations). Matched in memory against
    // AppState::nearby_uls_callsigns, so only the matches are read from the
    // database. Appended after whatever `suggestions` already holds (the
    // this-net and other-nets tiers), skipping callsigns already there,
    // until `max_suggestions` is reached. Nothing is added if neither ZIP is
    // recognized.
    static void AppendNearbyUlsSuggestions(AppState* state, const std::string& typed,
                                           const std::string& net_zip, std::size_t max_suggestions,
                                           std::vector<Station>* suggestions,
                                           std::vector<std::string>* labels)
    {
        if (suggestions->size() >= max_suggestions)
        {
            return;
        }
        RefreshNearbyZips(state, net_zip);
        if (state->nearby_zips.empty())
        {
            return;
        }

        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (state->nearby_uls_origin != state->nearby_zips_origin ||
            now - state->nearby_uls_loaded_at > kNearbyUlsReloadSeconds)
        {
            // An empty substring matches every nearby station; no limit.
            std::vector<NearbyUlsStation> all = state->db->SearchNearbyUlsStations(
                "", state->nearby_zips, state->nearby_zip3_prefixes, -1);
            state->nearby_uls_callsigns.clear();
            state->nearby_uls_callsigns.reserve(all.size());
            for (const NearbyUlsStation& station : all)
            {
                state->nearby_uls_callsigns.push_back({station.station.callsign, station.miles});
            }
            state->nearby_uls_origin = state->nearby_zips_origin;
            state->nearby_uls_loaded_at = now;
        }

        std::string upper = ToUpperAscii(typed);
        for (const NearbyUlsCallsign& candidate : state->nearby_uls_callsigns)
        {
            if (suggestions->size() >= max_suggestions)
            {
                break;
            }
            if (candidate.callsign.find(upper) == std::string::npos)
            {
                continue;
            }
            bool already_known = false;
            for (const Station& existing : *suggestions)
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
            // Gone if the station data was refreshed since the list loaded.
            std::optional<Station> station =
                state->db->FindUlsStationByCallsign(candidate.callsign);
            if (!station.has_value())
            {
                continue;
            }
            suggestions->push_back(*station);
            labels->push_back(FormatUlsSuggestion(*station, candidate.miles));
        }
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

        // Tier 3: nearby ULS-imported stations.
        AppendNearbyUlsSuggestions(state, state->saved_station.callsign, state->edit_net_location,
                                   kMaxSuggestions, &state->saved_station_suggestions,
                                   &state->saved_station_suggestion_labels);
    }

    void ApplySelectedSavedStationSuggestion(AppState* state)
    {
        if (state->saved_station_suggestions.empty())
        {
            return;
        }

        state->saved_station =
            state->saved_station_suggestions[state->selected_saved_station_suggestion_index];
        // Fill County in now, so it shows in the form as soon as the station
        // is picked rather than only once it's saved. (ULS records never
        // carry a county, so a ULS suggestion always needs this.)
        BackfillCountyFromZip(state, &state->saved_station);
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
    }

    void BackfillCountyFromZip(AppState* state, Station* station)
    {
        if (!station->county.empty() || station->zip.empty())
        {
            return;
        }

        // A handful of already-persisted stations predate uls_import.cpp's
        // NormalizeZip5 fix and still carry a 9-digit ZIP+4 (e.g.
        // "374152623") rather than a plain 5-digit ZIP -- the tables are
        // keyed on the latter, so truncate defensively here too rather than
        // only at import time.
        std::string zip5 = station->zip.size() > 5 ? station->zip.substr(0, 5) : station->zip;

        EnsureZipCountiesCached(state);
        if (!station->city.empty())
        {
            std::unordered_map<std::string, std::string>::const_iterator place_it =
                state->zip_place_county_by_key.find(zip5 + "|" + NormalizePlaceName(station->city));
            if (place_it != state->zip_place_county_by_key.end())
            {
                station->county = place_it->second;
                return;
            }
        }
        std::unordered_map<std::string, std::string>::const_iterator it =
            state->zip_county_by_zip.find(zip5);
        if (it != state->zip_county_by_zip.end())
        {
            station->county = it->second;
        }
    }

}  // namespace ql
