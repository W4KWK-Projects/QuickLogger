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
#include "list_columns.hpp"

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

    // ---- Lists laid out for the terminal's width -------------------------------
    //
    // Every list below describes its columns once (see list_columns.hpp); its
    // rows are kept as cells, and turned into the lines a Menu shows by
    // laying those out for AppState::list_width. The header line comes from
    // the same layout, so it can't drift out of line with the rows. The
    // columns shown at 80 columns, and their widths, are exactly the ones
    // these lists have always had.

    // Every header below sits above an ftxui::Menu, not a plain text list.
    // Menu's default entry renderer always prepends a 2-character indicator
    // ("> " for the focused row, "  " for every other one -- see
    // DefaultOptionTransform in FTXUI's menu.cpp) to every row it renders, so
    // without this same gutter a header's labels land two columns left of the
    // data they're supposed to name.
    static constexpr int kMenuEntryIndicatorWidth = 2;

    // The room a full-width list's rows get: the terminal less the list's
    // border and the Menu's gutter. Never less than at 80 columns.
    static int ScreenListWidth(int terminal_width)
    {
        return std::max(80, terminal_width) - 4;
    }
    static constexpr int kScreenListWidthAt80 = 76;

    // The same for the autocomplete matches, which sit inside a window.
    static int MatchListWidth(int terminal_width)
    {
        return std::max(80, terminal_width) - 26;
    }
    static constexpr int kMatchListWidthAt80 = 54;

    static std::string MenuGutter()
    {
        return std::string(kMenuEntryIndicatorWidth, ' ');
    }

    static std::vector<std::string> FormatRows(const std::vector<std::vector<std::string>>& rows,
                                               const ListLayout& layout)
    {
        std::vector<std::string> lines;
        lines.reserve(rows.size());
        for (const std::vector<std::string>& row : rows)
        {
            lines.push_back(FormatListRow(row, layout));
        }
        return lines;
    }

    // "Chattanooga, TN", or whichever half is known.
    static std::string CityAndState(const Station& station)
    {
        if (station.city.empty() || station.state.empty())
        {
            return station.city + station.state;
        }
        return station.city + ", " + station.state;
    }

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

    // -- Check-ins (the active net, History, exported logs) --

    // Heading, width, widest, when added, when widened, width in exports.
    static const std::vector<ListColumn>& CheckInColumns()
    {
        static const std::vector<ListColumn> columns = {
            {"#", 3, 3, 0, 0, 4},
            {"Time", 8, 8, 2, 0, 8},
            {"Callsign", 10, 13, 0, 99, 13},
            {"Name", 20, 30, 0, 1, 30},
            {"Member ID", 10, 10, 0, 0, 10},
            {"City, State", 16, 24, 3, 8, 30},
            {"County", 14, 14, 0, 0, 20},
            {"Role", 6, 6, 0, 0, 6},
            {"Signal", 6, 6, 4, 0, 6},
            {"Remarks", 7, 30, 0, 6, 40},
            {"Comment", 8, 40, 5, 0, 60},
        };
        return columns;
    }

    std::vector<std::vector<std::string>> CheckInCells(Database* db,
                                                       const std::vector<CheckIn>& check_ins)
    {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(check_ins.size());
        for (const CheckIn& check_in : check_ins)
        {
            std::optional<Station> found = db->FindStationByCallsign(check_in.callsign);
            Station station = found.has_value() ? *found : Station();
            rows.push_back({
                std::to_string(check_in.sequence_number),
                FormatLocalTimeOfDay(check_in.checked_in_at),
                check_in.callsign,
                station.name,
                station.member_id,
                CityAndState(station),
                station.county,
                RoleAbbreviation(check_in.designated_role),
                check_in.signal_report,
                check_in.remarks,
                check_in.comment,
            });
        }
        return rows;
    }

    static ListLayout CheckInLayout(int terminal_width)
    {
        return LayOutList(CheckInColumns(), ScreenListWidth(terminal_width), kScreenListWidthAt80,
                          1);
    }

    std::vector<std::string> FormatCheckInList(const std::vector<std::vector<std::string>>& cells,
                                               int terminal_width)
    {
        return FormatRows(cells, CheckInLayout(terminal_width));
    }

    std::string CheckInListHeader(int terminal_width)
    {
        return MenuGutter() + FormatListHeading(CheckInColumns(), CheckInLayout(terminal_width));
    }

    // -- Net sessions (History) --

    static const std::vector<ListColumn>& NetInstanceColumns(bool ad_hoc)
    {
        // A recurring net's own history; its name is in the page title.
        static const std::vector<ListColumn> recurring = {
            {"Date", 10, 10, 0, 0},        {"Start", 8, 8, 0, 0},          {"End", 8, 8, 0, 0},
            {"Net Control", 12, 12, 0, 0}, {"Alternate NC", 12, 12, 0, 0}, {"Logger", 12, 12, 0, 0},
            {"Check-ins", 9, 9, 1, 0},     {"Status", 6, 6, 0, 0},
        };
        // Every ad hoc net's sessions in one list: at 80 columns the net's
        // name takes the place of Alternate NC and Logger, which come back
        // when there's room.
        static const std::vector<ListColumn> every_ad_hoc = {
            {"Date", 10, 10, 0, 0},   {"Start", 8, 8, 0, 0},         {"End", 8, 8, 0, 0},
            {"Net", 24, 30, 0, 1},    {"Net Control", 12, 12, 0, 0}, {"Alternate NC", 12, 12, 3, 0},
            {"Logger", 12, 12, 4, 0}, {"Check-ins", 9, 9, 1, 0},     {"Status", 6, 6, 0, 0},
        };
        return ad_hoc ? every_ad_hoc : recurring;
    }

    static std::vector<std::string> NetInstanceCells(const NetInstance& instance,
                                                     const std::string& net_name,
                                                     std::int64_t check_ins, bool ad_hoc)
    {
        std::vector<std::string> cells = {
            instance.instance_date,
            FormatLocalTimeOfDay(instance.started_at),
            FormatLocalTimeOfDay(instance.closed_at),
        };
        if (ad_hoc)
        {
            cells.push_back(net_name);
        }
        cells.push_back(instance.net_control_callsign);
        cells.push_back(instance.alternate_net_control_callsign);
        cells.push_back(instance.logger_callsign);
        cells.push_back(std::to_string(check_ins));
        cells.emplace_back(instance.status == NetInstanceStatus::kOpen ? "OPEN" : "closed");
        return cells;
    }

    static ListLayout NetInstanceLayout(int terminal_width, bool ad_hoc)
    {
        return LayOutList(NetInstanceColumns(ad_hoc), ScreenListWidth(terminal_width),
                          kScreenListWidthAt80, 1);
    }

    std::string NetInstanceListHeader(int terminal_width, bool ad_hoc)
    {
        return MenuGutter() + FormatListHeading(NetInstanceColumns(ad_hoc),
                                                NetInstanceLayout(terminal_width, ad_hoc));
    }

    // -- A net's saved stations (Edit Net, exported lists) --

    static const std::vector<ListColumn>& SavedStationColumns()
    {
        static const std::vector<ListColumn> columns = {
            {"Callsign", 10, 13, 0, 99, 13},       {"Name", 20, 30, 0, 6, 30},
            {"Member ID", 10, 10, 0, 0, 10},       {"City, State", 16, 24, 2, 8, 30},
            {"County", 14, 14, 3, 0, 20},          {"Grid", 6, 8, 4, 0, 8},
            {"Default Remarks", 15, 40, 5, 0, 40},
        };
        return columns;
    }

    static std::vector<std::string> SavedStationCells(const Station& station,
                                                      const std::string& default_remarks)
    {
        return {station.callsign, station.name,        station.member_id, CityAndState(station),
                station.county,   station.grid_square, default_remarks};
    }

    static ListLayout SavedStationLayout(int terminal_width)
    {
        return LayOutList(SavedStationColumns(), ScreenListWidth(terminal_width),
                          kScreenListWidthAt80, 1);
    }

    std::string SavedStationListHeader(int terminal_width)
    {
        return MenuGutter() +
               FormatListHeading(SavedStationColumns(), SavedStationLayout(terminal_width));
    }

    // -- Autocomplete matches (New Check-In, Saved Station) --

    static const std::vector<ListColumn>& MatchColumns()
    {
        static const std::vector<ListColumn> columns = {
            {"Callsign", 10, 13, 0, 99}, {"Name", 20, 30, 0, 4},   {"City, State", 16, 24, 1, 3},
            {"County", 14, 14, 2, 0},    {"Source", 13, 13, 0, 0},
        };
        return columns;
    }

    static ListLayout MatchLayout(int terminal_width)
    {
        return LayOutList(MatchColumns(), MatchListWidth(terminal_width), kMatchListWidthAt80, 1);
    }

    std::string MatchListHeader(int terminal_width)
    {
        return MenuGutter() + FormatListHeading(MatchColumns(), MatchLayout(terminal_width));
    }

    static std::vector<std::string> FormatMatches(const std::vector<Station>& stations,
                                                  const std::vector<std::string>& sources,
                                                  int terminal_width)
    {
        ListLayout layout = MatchLayout(terminal_width);
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < stations.size() && i < sources.size(); ++i)
        {
            const Station& station = stations[i];
            lines.push_back(FormatListRow(
                {station.callsign, station.name, CityAndState(station), station.county, sources[i]},
                layout));
        }
        return lines;
    }

    // "(this net)" / "(other net)" for a match known to a net.
    static std::string KnownStationSource(bool is_this_net)
    {
        return is_this_net ? "(this net)" : "(other net)";
    }

    // `distance_miles` < 0 means "unknown" (the station's own ZIP has no
    // centroid on file) -- shown as "(ULS, nearby)" rather than a fabricated
    // number, since it only passed the coarser ZIP3-prefix pre-filter.
    static std::string UlsSource(double distance_miles)
    {
        if (distance_miles < 0.0)
        {
            return "(ULS, nearby)";
        }
        return "(ULS, ~" + std::to_string(static_cast<int>(distance_miles)) + " mi)";
    }

    // -- Recurring Nets --

    // Net-list names are padded to the longest (up to this) so the columns
    // after them line up.
    static constexpr int kMaxNetNameColumnWidth = 40;

    // The list has no header line; its extra columns (mode, frequency, when
    // it meets) read for themselves. The when-created/imported column comes
    // last, as it always has.
    static std::vector<ListColumn> NetListColumns(int name_width)
    {
        return {
            {"Net", name_width, name_width, 0, 0},
            {"Mode", 6, 8, 1, 5},
            {"Frequency", 10, 12, 2, 6},
            {"Recurrence", 20, 30, 3, 4},
            {"", 12, 12, 0, 0},
        };
    }

    static std::vector<std::string> NetListCells(const Net& net, bool has_open_session)
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
        return {net.name, net.mode, net.default_frequency, net.recurrence_description, when};
    }

    static std::vector<std::string> FormatNetList(const AppState* state)
    {
        std::vector<std::string> rows =
            FormatRows(state->net_cells,
                       LayOutList(NetListColumns(state->net_name_width),
                                  ScreenListWidth(state->list_width), kScreenListWidthAt80, 2));
        // A net with nothing after its name is shown as just its name.
        for (std::string& row : rows)
        {
            row.erase(row.find_last_not_of(' ') + 1);
        }
        return rows;
    }

    static void FormatInfoTable(AppState* state);

    // Re-lays out every list (and an open window's table) for
    // AppState::list_width.
    static void RelayOutLists(AppState* state)
    {
        state->net_names = FormatNetList(state);
        state->active_display_rows =
            FormatCheckInList(state->active_check_in_cells, state->list_width);
        state->history_check_in_labels =
            FormatCheckInList(state->history_check_in_cells, state->list_width);
        state->history_instance_labels =
            FormatRows(state->history_instance_cells,
                       NetInstanceLayout(state->list_width, state->history_ad_hoc));
        state->edit_net_saved_station_labels =
            FormatRows(state->saved_station_cells, SavedStationLayout(state->list_width));
        state->modal_callsign_suggestion_labels =
            FormatMatches(state->modal_callsign_suggestions,
                          state->modal_callsign_suggestion_sources, state->list_width);
        state->saved_station_suggestion_labels =
            FormatMatches(state->saved_station_suggestions, state->saved_station_suggestion_sources,
                          state->list_width);
        FormatInfoTable(state);
    }

    void UpdateListWidths(AppState* state, int terminal_width)
    {
        int width = std::max(80, terminal_width);
        if (width == state->list_width)
        {
            return;
        }
        state->list_width = width;
        RelayOutLists(state);
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
        state->net_name_width = std::min(name_width, kMaxNetNameColumnWidth);

        state->net_cells.clear();
        for (const Net& net : state->nets)
        {
            bool has_open_session =
                std::find(open_net_ids.begin(), open_net_ids.end(), net.id) != open_net_ids.end();
            state->net_cells.push_back(NetListCells(net, has_open_session));
        }
        state->net_names = FormatNetList(state);

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
             "both be adding to the same log. Or close it and start a new session, or just "
             "view it without changing anything."});
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

    static void LeaveActiveNet(AppState* state);

    // Joins AppState::resume_instance, to log it or (`viewing`) just watch.
    static void JoinOpenSession(AppState* state, bool viewing)
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
        state->viewing_only = viewing;
        if (viewing)
        {
            state->selected_role_index = kRoleViewer;
            state->operator_callsign = state->settings.callsign;
        }
        state->show_new_station_modal = false;
        state->show_edit_checkin_modal = false;
        state->selected_check_in_index = 0;
        ClearModalFields(state);
        RefreshActiveCheckIns(state);
        state->status_message = viewing
                                    ? "Watching the " + DescribeSessionStart(*session) + " session."
                                    : "Resumed the " + DescribeSessionStart(*session) + " session.";
        state->page = kPageActiveNet;
    }

    void ResumeOpenNet(AppState* state)
    {
        JoinOpenSession(state, false);
    }

    void ViewOpenNet(AppState* state)
    {
        JoinOpenSession(state, true);
    }

    void ViewStartNet(AppState* state)
    {
        for (const NetInstance& session : state->db->GetNetInstancesForNet(state->start_net.id))
        {
            if (session.status == NetInstanceStatus::kOpen)
            {
                state->resume_instance = session;
                ViewOpenNet(state);
                return;
            }
        }
        state->form_error = "No session of " + state->start_net.name +
                            " is open to watch. Choose another role to start one.";
    }

    void StopViewing(AppState* state)
    {
        LeaveActiveNet(state);
        state->viewing_only = false;
        state->form_error.clear();
        state->status_message.clear();
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
        state->active_check_in_cells.clear();
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
        std::size_t check_ins =
            state->db->GetCheckInsForNetInstance(state->active_instance.id).size();
        std::string history = HistoryKeyDescription(state);
        if (!closed_here)
        {
            // Someone sharing the session closed (or deleted) it first. What
            // you wanted has happened either way; say so. (Their end time is
            // the one that stands -- see CloseNetInstance.)
            std::optional<NetInstance> session =
                state->db->GetNetInstanceById(state->active_instance.id);
            LeaveActiveNet(state);
            if (session.has_value())
            {
                std::string ended = DescribeSessionEnd(*session);
                state->form_error.clear();
                state->status_message = closed_name + " was already closed by someone else" +
                                        (ended.empty() ? "" : " at " + ended) + " (" +
                                        CountCheckIns(check_ins) + "). It's in History (" +
                                        history + ").";
            }
            else
            {
                state->status_message.clear();
                state->form_error = closed_name +
                                    "'s session was deleted by someone else, so there was "
                                    "nothing left to close.";
            }
            return;
        }
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

    void RefreshActiveCheckIns(AppState* state)
    {
        state->active_check_ins = state->db->GetCheckInsForNetInstance(state->active_instance.id);
        state->active_check_in_cells = CheckInCells(state->db, state->active_check_ins);
        state->active_display_rows =
            FormatCheckInList(state->active_check_in_cells, state->list_width);

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
        state->modal_callsign_suggestion_sources.clear();
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
        // Once per session, counting W4KWK/M as W4KWK. Read fresh: someone
        // else sharing the session may have logged it.
        std::string base = BaseCallsign(state->modal_station.callsign);
        for (const CheckIn& existing :
             state->db->GetCheckInsForNetInstance(state->active_instance.id))
        {
            if (BaseCallsign(existing.callsign) == base)
            {
                state->form_error =
                    state->modal_station.callsign + " is already in this session's log, as " +
                    (existing.callsign == state->modal_station.callsign ? std::string()
                                                                        : existing.callsign + " ") +
                    "#" + std::to_string(existing.sequence_number) + ".";
                return false;
            }
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
        state->history_instance_cells.clear();
        state->history_instance_labels.clear();

        std::unordered_map<std::int64_t, std::string> names;
        if (state->history_ad_hoc)
        {
            for (const Net& net : state->db->GetAllNets())
            {
                names[net.id] = net.name;
            }
            state->history_instances = state->db->GetAdHocNetInstances();
        }
        else
        {
            if (state->selected_net_index >= static_cast<int>(state->nets.size()))
            {
                return;
            }
            state->history_instances =
                state->db->GetNetInstancesForNet(state->nets[state->selected_net_index].id);
        }
        for (const NetInstance& instance : state->history_instances)
        {
            std::int64_t check_ins = 0;
            std::int64_t newest_id = 0;
            state->db->GetCheckInSummary(instance.id, &check_ins, &newest_id);
            state->history_instance_cells.push_back(NetInstanceCells(
                instance, names[instance.net_id], check_ins, state->history_ad_hoc));
        }
        state->history_instance_labels =
            FormatRows(state->history_instance_cells,
                       NetInstanceLayout(state->list_width, state->history_ad_hoc));

        if (state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            state->selected_history_index = 0;
        }

        RefreshHistoryCheckIns(state);
    }

    void RefreshHistoryCheckIns(AppState* state)
    {
        state->history_check_in_labels.clear();
        state->history_check_in_cells.clear();
        state->history_check_ins.clear();
        state->selected_history_check_in_index = 0;

        if (state->selected_history_index >= static_cast<int>(state->history_instances.size()))
        {
            return;
        }

        const NetInstance& selected = state->history_instances[state->selected_history_index];
        state->history_check_ins = state->db->GetCheckInsForNetInstance(selected.id);
        state->history_check_in_cells = CheckInCells(state->db, state->history_check_ins);
        state->history_check_in_labels =
            FormatCheckInList(state->history_check_in_cells, state->list_width);
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
                                  std::vector<std::string> lines)
    {
        // Rows whose last columns are empty would otherwise end in padding.
        for (std::string& line : lines)
        {
            line.erase(line.find_last_not_of(' ') + 1);
        }
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
            case RowPickAction::kViewStationHistory:
            case RowPickAction::kViewStationCard:
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
            case RowPickAction::kViewStationHistory:
            case RowPickAction::kViewStationCard:
                return "View";
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
            case RowPickAction::kViewStationHistory:
            case RowPickAction::kViewStationCard:
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
            case RowPickAction::kViewStationHistory:
                OpenStationHistory(state, state->active_check_ins[index]);
                return;
            case RowPickAction::kViewStationCard:
                OpenStationCard(state, state->active_check_ins[index].callsign);
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
        // A fixed format: every column, each at its set export width,
        // whatever the terminal or the data (see ExportListLayout).
        std::vector<std::vector<std::string>> cells = CheckInCells(state->db, check_ins);
        ListLayout layout = ExportListLayout(CheckInColumns(), 1);
        lines.push_back(FormatListHeading(CheckInColumns(), layout));
        std::vector<std::string> rows = FormatRows(cells, layout);
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
        // Every column, as in ExportNetLog.
        std::vector<std::vector<std::string>> cells;
        for (const Station& station : saved_stations)
        {
            cells.push_back(SavedStationCells(station, state->db->GetSavedNetStationRemarks(
                                                           state->edit_net_id, station.callsign)));
        }
        ListLayout layout = ExportListLayout(SavedStationColumns(), 1);
        lines.push_back(FormatListHeading(SavedStationColumns(), layout));
        std::vector<std::string> rows = FormatRows(cells, layout);
        lines.insert(lines.end(), rows.begin(), rows.end());

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
                                           std::vector<std::string>* sources);

    void RefreshCallsignSuggestions(AppState* state)
    {
        state->modal_callsign_suggestions.clear();
        state->modal_callsign_suggestion_labels.clear();
        state->modal_callsign_suggestion_sources.clear();
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
            state->modal_callsign_suggestion_sources.push_back(KnownStationSource(is_this_net));
        }

        // Tier 3: licensed stations near the net, from the FCC data.
        AppendNearbyUlsSuggestions(state, state->modal_station.callsign, state->active_net_zip,
                                   kMaxSuggestions, &state->modal_callsign_suggestions,
                                   &state->modal_callsign_suggestion_sources);
        state->modal_callsign_suggestion_labels =
            FormatMatches(state->modal_callsign_suggestions,
                          state->modal_callsign_suggestion_sources, state->list_width);
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
        state->modal_callsign_suggestion_sources.clear();
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

        state->saved_station_cells.clear();
        for (const Station& station : state->edit_net_saved_stations)
        {
            state->saved_station_cells.push_back(SavedStationCells(
                station,
                state->db->GetSavedNetStationRemarks(state->edit_net_id, station.callsign)));
        }
        state->edit_net_saved_station_labels =
            FormatRows(state->saved_station_cells, SavedStationLayout(state->list_width));

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
        state->saved_station_suggestion_sources.clear();
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
        state->saved_station_suggestion_sources.clear();
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
                                           std::vector<std::string>* sources)
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
            sources->push_back(UlsSource(candidate.miles));
        }
    }

    void RefreshSavedStationSuggestions(AppState* state)
    {
        state->saved_station_suggestions.clear();
        state->saved_station_suggestion_labels.clear();
        state->saved_station_suggestion_sources.clear();
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
            state->saved_station_suggestion_sources.push_back(KnownStationSource(is_this_net));
        }

        // Tier 3: nearby ULS-imported stations.
        AppendNearbyUlsSuggestions(state, state->saved_station.callsign, state->edit_net_location,
                                   kMaxSuggestions, &state->saved_station_suggestions,
                                   &state->saved_station_suggestion_sources);
        state->saved_station_suggestion_labels =
            FormatMatches(state->saved_station_suggestions, state->saved_station_suggestion_sources,
                          state->list_width);
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
        state->saved_station_suggestion_sources.clear();
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

    // ---- The seldom-used windows ------------------------------------------------

    // The room a window's table gets: the terminal less the window's and
    // the table's borders, the marker column and a margin. Never less than
    // at 80 columns.
    static int InfoTableWidth(int terminal_width)
    {
        return std::max(80, terminal_width) - 10;
    }
    static constexpr int kInfoTableWidthAt80 = 70;

    // Lays the open window's table out for AppState::list_width (it widens
    // with the terminal, like the lists).
    static void FormatInfoTable(AppState* state)
    {
        state->info_header.clear();
        state->info_rows.clear();
        if (state->info_columns.empty())
        {
            return;
        }
        ListLayout layout = LayOutList(state->info_columns, InfoTableWidth(state->list_width),
                                       kInfoTableWidthAt80, 2);
        // Cut to the table's width: a long last column would otherwise make
        // the list scroll sideways to show it, hiding the first columns.
        std::size_t width = static_cast<std::size_t>(InfoTableWidth(state->list_width));
        state->info_header = FormatListHeading(state->info_columns, layout).substr(0, width);
        state->info_rows = FormatRows(state->info_cells, layout);
        for (std::string& row : state->info_rows)
        {
            row = row.substr(0, width);
        }
    }

    static void ShowInfoWindow(AppState* state, InfoWindow window, const std::string& title,
                               std::vector<std::string> summary, std::vector<ListColumn> columns,
                               std::vector<std::vector<std::string>> cells)
    {
        state->info_window = window;
        state->show_info_window = true;
        state->info_title = title;
        state->info_summary = std::move(summary);
        state->info_columns = std::move(columns);
        state->info_cells = std::move(cells);
        state->info_selected = 0;
        state->form_error.clear();
        state->status_message.clear();
        FormatInfoTable(state);
    }

    void CloseInfoWindow(AppState* state)
    {
        state->info_window = InfoWindow::kNone;
        state->show_info_window = false;
        state->info_summary.clear();
        state->info_header.clear();
        state->info_rows.clear();
        state->info_columns.clear();
        state->info_cells.clear();
        state->info_stations.clear();
        state->info_query.clear();
        state->info_selected = 0;
    }

    void MoveInfoSelection(AppState* state, int delta)
    {
        int last = static_cast<int>(state->info_rows.size()) - 1;
        int moved = state->info_selected + delta;
        state->info_selected = moved < 0 ? 0 : (moved > last ? std::max(last, 0) : moved);
    }

    static std::string StationName(Database* db, const std::string& callsign)
    {
        std::optional<Station> station = db->FindStationByCallsign(callsign);
        return station.has_value() ? station->name : "";
    }

    // "13 of the last 20", "1 of 1".
    static std::string OutOf(int part, int whole)
    {
        return std::to_string(part) + " of " + std::to_string(whole);
    }

    // The active net's other sessions, newest first.
    static std::vector<NetInstance> OtherSessions(AppState* state)
    {
        std::vector<NetInstance> sessions;
        for (const NetInstance& session :
             state->db->GetNetInstancesForNet(state->active_instance.net_id))
        {
            if (session.id != state->active_instance.id)
            {
                sessions.push_back(session);
            }
        }
        return sessions;
    }

    void OpenStationHistory(AppState* state, const CheckIn& check_in)
    {
        std::vector<StationCheckInRecord> records =
            state->db->GetStationCheckInsForNet(state->active_instance.net_id, check_in.callsign);
        std::vector<std::vector<std::string>> rows;
        std::vector<std::int64_t> sessions_in;
        std::string first_date;
        for (const StationCheckInRecord& record : records)
        {
            if (record.instance.id == state->active_instance.id)
            {
                continue;
            }
            sessions_in.push_back(record.instance.id);
            first_date = record.instance.instance_date;
            rows.push_back(
                {record.instance.instance_date, FormatLocalTimeOfDay(record.instance.started_at),
                 std::to_string(record.check_in.sequence_number),
                 RoleAbbreviation(record.check_in.designated_role), record.check_in.signal_report,
                 record.check_in.remarks, record.check_in.comment});
        }

        std::vector<std::string> summary;
        std::string name = StationName(state->db, check_in.callsign);
        if (!name.empty())
        {
            summary.push_back(name);
        }
        if (rows.empty())
        {
            summary.emplace_back("No other check-ins to this net.");
        }
        else
        {
            // Of the net's most recent sessions (not counting this one), how
            // many this station was in.
            std::vector<NetInstance> sessions = OtherSessions(state);
            int recent = 0;
            int recent_in = 0;
            for (std::size_t i = 0; i < sessions.size() && recent < 20; ++i)
            {
                ++recent;
                if (std::find(sessions_in.begin(), sessions_in.end(), sessions[i].id) !=
                    sessions_in.end())
                {
                    ++recent_in;
                }
            }
            summary.push_back("Checked in to " + OutOf(recent_in, recent) +
                              " of this net's most recent other sessions; " +
                              std::to_string(rows.size()) + " in all since " + first_date + ".");
        }
        ShowInfoWindow(state, InfoWindow::kStationHistory, "Station History: " + check_in.callsign,
                       summary,
                       {{"Date", 10, 10, 0, 0},
                        {"Start", 8, 8, 0, 0},
                        {"#", 3, 3, 0, 0},
                        {"Role", 5, 5, 0, 0},
                        {"Signal", 6, 6, 1, 0},
                        {"Remarks", 20, 24, 0, 3},
                        {"Comment", 12, 40, 2, 0}},
                       rows);
    }

    void OpenRegulars(AppState* state)
    {
        std::vector<NetInstance> sessions = OtherSessions(state);
        if (sessions.size() > 10)
        {
            sessions.resize(10);
        }
        std::vector<std::string> callsigns;
        std::vector<int> counts;
        std::vector<std::string> last_seen;
        for (const NetInstance& session : sessions)
        {
            std::vector<std::string> in_session;
            for (const CheckIn& check_in : state->db->GetCheckInsForNetInstance(session.id))
            {
                if (std::find(in_session.begin(), in_session.end(), check_in.callsign) !=
                    in_session.end())
                {
                    continue;  // Counted once per session.
                }
                in_session.push_back(check_in.callsign);
                std::vector<std::string>::iterator found =
                    std::find(callsigns.begin(), callsigns.end(), check_in.callsign);
                if (found == callsigns.end())
                {
                    callsigns.push_back(check_in.callsign);
                    counts.push_back(1);
                    last_seen.push_back(session.instance_date);  // Newest first.
                }
                else
                {
                    ++counts[static_cast<std::size_t>(found - callsigns.begin())];
                }
            }
        }

        // Regulars: at least half the sessions looked at, not yet here.
        std::vector<std::size_t> regulars;
        int total = static_cast<int>(sessions.size());
        for (std::size_t i = 0; i < callsigns.size(); ++i)
        {
            bool here = false;
            for (const CheckIn& check_in : state->active_check_ins)
            {
                here = here || check_in.callsign == callsigns[i];
            }
            if (!here && counts[i] * 2 >= total)
            {
                regulars.push_back(i);
            }
        }
        // Most regular first.
        for (std::size_t i = 1; i < regulars.size(); ++i)
        {
            std::size_t j = i;
            while (j > 0 && (counts[regulars[j]] > counts[regulars[j - 1]] ||
                             (counts[regulars[j]] == counts[regulars[j - 1]] &&
                              callsigns[regulars[j]] < callsigns[regulars[j - 1]])))
            {
                std::swap(regulars[j], regulars[j - 1]);
                --j;
            }
        }

        std::vector<std::vector<std::string>> rows;
        state->info_stations.clear();
        for (std::size_t index : regulars)
        {
            std::optional<Station> found = state->db->FindStationByCallsign(callsigns[index]);
            Station station = found.has_value() ? *found : Station();
            station.callsign = callsigns[index];
            rows.push_back(
                {station.callsign, station.name, OutOf(counts[index], total), last_seen[index]});
            state->info_stations.push_back(station);
        }

        std::vector<std::string> summary;
        if (total == 0)
        {
            summary.emplace_back("This net has no earlier sessions yet.");
        }
        else if (rows.empty())
        {
            summary.push_back("Every station that checked in to at least half of the last " +
                              std::to_string(total) + " sessions has checked in.");
        }
        else
        {
            summary.push_back("Checked in to at least half of the last " + std::to_string(total) +
                              " sessions, but not yet to this one. Enter checks the highlighted "
                              "one in.");
        }
        std::vector<Station> stations = state->info_stations;
        ShowInfoWindow(state, InfoWindow::kRegulars, "Regulars Not Yet Heard", summary,
                       {{"Callsign", 10, 10, 0, 0},
                        {"Name", 24, 30, 0, 1},
                        {"Sessions", 8, 8, 0, 0},
                        {"Last Seen", 10, 10, 0, 0}},
                       rows);
        state->info_stations = stations;
    }

    void CheckInSelectedRegular(AppState* state)
    {
        if (state->viewing_only || state->info_stations.empty() ||
            state->info_selected >= static_cast<int>(state->info_stations.size()))
        {
            return;
        }
        Station station = state->info_stations[static_cast<std::size_t>(state->info_selected)];
        CloseInfoWindow(state);
        if (!EnsureActiveSessionOpen(state, ""))
        {
            return;
        }
        ClearModalFields(state);
        state->modal_station = station;
        BackfillCountyFromZip(state, &state->modal_station);
        state->modal_remarks =
            state->db->GetSavedNetStationRemarks(state->active_instance.net_id, station.callsign);
        state->show_new_station_modal = true;
        if (state->modal_callsign_input)
        {
            state->modal_callsign_input->TakeFocus();
        }
    }

    void OpenStationCard(AppState* state, const std::string& callsign)
    {
        std::optional<Station> known = state->db->FindStationByCallsign(callsign);
        std::optional<Station> licensed = state->db->FindUlsStationByCallsign(callsign);
        Station station =
            known.has_value() ? *known : (licensed.has_value() ? *licensed : Station());
        StationActivity activity = state->db->GetStationActivity(callsign);

        std::vector<std::string> summary;
        std::string address = station.street_address;
        std::string place = CityAndState(station);
        if (!station.zip.empty())
        {
            place += (place.empty() ? "" : " ") + station.zip;
        }
        summary.push_back("Name:           " + station.name);
        summary.push_back("Address:        " + address +
                          (address.empty() || place.empty() ? "" : ", ") + place);
        summary.push_back("County:         " + station.county);
        summary.push_back("Grid Square:    " + station.grid_square);
        summary.push_back("Member ID:      " + station.member_id);
        summary.push_back("License Class:  " +
                          (licensed.has_value() && !licensed->license_class.empty()
                               ? licensed->license_class
                               : std::string("(not in the FCC data)")));
        if (activity.check_ins == 0)
        {
            summary.emplace_back("Check-ins:      none yet");
        }
        else
        {
            summary.push_back("Check-ins:      " + std::to_string(activity.check_ins) + " (first " +
                              FormatLocalDate(activity.first_at) + ", latest " +
                              FormatLocalDate(activity.last_at) + ")");
        }
        std::string nets;
        for (const std::string& net : activity.saved_to_nets)
        {
            nets += (nets.empty() ? "" : ", ") + net;
        }
        summary.push_back("Saved to:       " + (nets.empty() ? std::string("no nets") : nets));
        ShowInfoWindow(state, InfoWindow::kStationCard, "Station: " + callsign, summary, {}, {});
    }

    void OpenSessionSummary(AppState* state)
    {
        std::vector<std::vector<std::string>> rows;
        for (const CheckIn& check_in : state->active_check_ins)
        {
            bool first_time = true;
            for (const StationCheckInRecord& record : state->db->GetStationCheckInsForNet(
                     state->active_instance.net_id, check_in.callsign))
            {
                first_time = first_time && record.instance.id == state->active_instance.id;
            }
            if (first_time)
            {
                rows.push_back({std::to_string(check_in.sequence_number), check_in.callsign,
                                StationName(state->db, check_in.callsign)});
            }
        }

        std::vector<std::string> summary;
        summary.push_back(
            CountCheckIns(state->active_check_ins.size()) + " so far" +
            (state->active_instance.started_at > 0
                 ? " (started " + FormatLocalTimeOfDay(state->active_instance.started_at) + ")."
                 : std::string(".")));
        std::vector<NetInstance> sessions = OtherSessions(state);
        if (sessions.size() > 10)
        {
            sessions.resize(10);
        }
        if (!sessions.empty())
        {
            std::int64_t sum = 0;
            std::int64_t most = 0;
            for (const NetInstance& session : sessions)
            {
                std::int64_t count = 0;
                std::int64_t newest_id = 0;
                state->db->GetCheckInSummary(session.id, &count, &newest_id);
                sum += count;
                most = std::max(most, count);
            }
            char average[32];
            std::snprintf(average, sizeof(average), "%.1f",
                          static_cast<double>(sum) / static_cast<double>(sessions.size()));
            summary.push_back("The last " + std::to_string(sessions.size()) +
                              " sessions averaged " + average + " check-ins (most " +
                              std::to_string(most) + ").");
        }
        summary.push_back(rows.empty() ? "No first-timers yet."
                                       : std::to_string(rows.size()) +
                                             " checking in to this net for the first time:");
        ShowInfoWindow(state, InfoWindow::kSessionSummary,
                       "Session Summary: " + state->active_net_name, summary,
                       rows.empty() ? std::vector<ListColumn>()
                                    : std::vector<ListColumn>({{"#", 3, 3, 0, 0},
                                                               {"Callsign", 10, 10, 0, 0},
                                                               {"Name", 30, 30, 0, 0}}),
                       rows);
    }

    void OpenNetStatistics(AppState* state)
    {
        if (state->history_ad_hoc ||
            state->selected_net_index >= static_cast<int>(state->nets.size()))
        {
            return;
        }
        const Net& net = state->nets[static_cast<std::size_t>(state->selected_net_index)];
        std::vector<NetInstance> sessions = state->db->GetNetInstancesForNet(net.id);

        std::vector<std::string> summary;
        if (sessions.empty())
        {
            summary.emplace_back("No sessions yet.");
            ShowInfoWindow(state, InfoWindow::kNetStatistics, "Net Statistics: " + net.name,
                           summary, {}, {});
            return;
        }
        std::int64_t sum = 0;
        std::int64_t most = -1;
        std::string most_date;
        // Months, newest first: "2026-09", sessions, check-ins.
        std::vector<std::string> months;
        std::vector<int> month_sessions;
        std::vector<std::int64_t> month_check_ins;
        for (const NetInstance& session : sessions)
        {
            std::int64_t count = 0;
            std::int64_t newest_id = 0;
            state->db->GetCheckInSummary(session.id, &count, &newest_id);
            sum += count;
            if (count > most)
            {
                most = count;
                most_date = session.instance_date;
            }
            std::string month = session.instance_date.substr(0, 7);
            if (months.empty() || months.back() != month)
            {
                months.push_back(month);
                month_sessions.push_back(0);
                month_check_ins.push_back(0);
            }
            ++month_sessions.back();
            month_check_ins.back() += count;
        }
        char average[32];
        std::snprintf(average, sizeof(average), "%.1f",
                      static_cast<double>(sum) / static_cast<double>(sessions.size()));
        summary.push_back(std::to_string(sessions.size()) + " sessions, " +
                          sessions.back().instance_date + " to " + sessions.front().instance_date +
                          "; " + std::to_string(sum) + " check-ins in all.");
        summary.push_back(std::string("Average ") + average + " check-ins a session; most " +
                          std::to_string(most) + ", on " + most_date + ".");
        std::string by_month = "Recent months:";
        for (std::size_t i = 0; i < months.size() && i < 6; ++i)
        {
            by_month += (i == 0 ? " " : "; ") + months[i] + " " +
                        std::to_string(month_sessions[i]) +
                        (month_sessions[i] == 1 ? " session/" : " sessions/") +
                        std::to_string(month_check_ins[i]);
        }
        summary.push_back(by_month + " check-ins.");
        summary.emplace_back("Most frequent stations:");

        std::vector<std::vector<std::string>> rows;
        for (const CallsignTally& tally : state->db->GetTopCallsignsForNet(net.id, 15))
        {
            rows.push_back({tally.callsign, StationName(state->db, tally.callsign),
                            std::to_string(tally.count), tally.last_date});
        }
        ShowInfoWindow(state, InfoWindow::kNetStatistics, "Net Statistics: " + net.name, summary,
                       {{"Callsign", 10, 10, 0, 0},
                        {"Name", 24, 30, 0, 1},
                        {"Check-ins", 9, 9, 0, 0},
                        {"Last", 10, 10, 0, 0}},
                       rows);
    }

    // How many check-ins the station search shows at most.
    static constexpr int kStationSearchLimit = 200;

    void RefreshStationSearch(AppState* state)
    {
        state->info_query = NormalizeCallsign(state->info_query);
        state->info_cells.clear();
        state->info_selected = 0;
        state->info_summary.clear();
        if (state->info_query.size() < 3)
        {
            state->info_summary.emplace_back(
                "Type at least 3 characters of a callsign to see its check-ins to every net.");
            FormatInfoTable(state);
            return;
        }
        std::vector<StationCheckInRecord> records =
            state->db->FindCheckInsByCallsign(state->info_query, kStationSearchLimit);
        for (const StationCheckInRecord& record : records)
        {
            state->info_cells.push_back(
                {record.instance.instance_date, record.net_name, record.check_in.callsign,
                 StationName(state->db, record.check_in.callsign),
                 RoleAbbreviation(record.check_in.designated_role), record.check_in.remarks});
        }
        state->info_summary.push_back(
            records.empty() ? std::string("No check-ins found.")
            : records.size() >= static_cast<std::size_t>(kStationSearchLimit)
                ? "The newest " + std::to_string(kStationSearchLimit) + " check-ins found:"
                : std::to_string(records.size()) + " check-ins found:");
        FormatInfoTable(state);
    }

    void OpenStationSearch(AppState* state)
    {
        ShowInfoWindow(state, InfoWindow::kStationSearch, "Find a Station", {},
                       {{"Date", 10, 10, 0, 0},
                        {"Net", 16, 30, 0, 2},
                        {"Callsign", 10, 13, 0, 99},
                        {"Name", 20, 24, 1, 3},
                        {"Role", 5, 5, 0, 0},
                        {"Remarks", 10, 40, 0, 4}},
                       {});
        state->info_query.clear();
        RefreshStationSearch(state);
    }

    void OpenQuietStations(AppState* state)
    {
        std::string cutoff =
            FormatLocalDate(static_cast<std::int64_t>(std::time(nullptr)) - 182 * 24 * 3600);
        std::vector<CallsignTally> tallies = state->db->GetSavedStationActivity(state->edit_net_id);
        std::vector<std::vector<std::string>> rows;
        for (const CallsignTally& tally : tallies)
        {
            if (!tally.last_date.empty() && tally.last_date >= cutoff)
            {
                continue;
            }
            rows.push_back({tally.callsign, StationName(state->db, tally.callsign),
                            tally.last_date.empty() ? "never" : tally.last_date,
                            std::to_string(tally.count)});
        }
        std::vector<std::string> summary;
        summary.push_back(OutOf(static_cast<int>(rows.size()), static_cast<int>(tallies.size())) +
                          " saved stations haven't checked in to this net since " + cutoff +
                          ". To remove one, close this window and use F4.");
        ShowInfoWindow(state, InfoWindow::kQuietStations, "Quiet Saved Stations", summary,
                       {{"Callsign", 10, 10, 0, 0},
                        {"Name", 24, 30, 0, 1},
                        {"Last", 10, 10, 0, 0},
                        {"Check-ins", 9, 9, 0, 0}},
                       rows);
    }

    // One line of the Help window: a key and what it does; `extra` marks a
    // seldom-used key that's only on the key bar of a wide enough terminal.
    struct HelpLine
    {
        const char* key;
        const char* what;
        bool extra;
    };

    static std::vector<HelpLine> HelpFor(const AppState* state)
    {
        switch (state->page)
        {
            case kPageNetList:
                return {
                    {"F2", "Create a new recurring net.", false},
                    {"F3/Enter", "Start the highlighted net, or resume its open session.", false},
                    {"F4", "Settings: your callsign, home ZIP and time format.", false},
                    {"F5", "Ad hoc nets: start one, resume one, or see their history.", false},
                    {"F6", "History of the highlighted net: view, export, delete.", false},
                    {"F7", "Edit a net (by number): its details and saved stations.", false},
                    {"F8", "Export the highlighted net to a file to share.", false},
                    {"F9", "Import a net from a file.", false},
                    {"F10", "Quit.", false},
                    {"Up/Down", "Move the highlight.", false},
                };
            case kPageCreateNet:
                return {
                    {"F2", "Save the new net.", false},
                    {"Esc", "Cancel.", false},
                    {"Tab", "Move to the next field (Up/Down too).", false},
                };
            case kPageSelectRole:
                return {
                    {"Up/Down", "Choose your role, or Viewer to watch an open session.", false},
                    {"F2/Enter", "Continue.", false},
                    {"Esc", "Back to the net list.", false},
                };
            case kPageEnterCallsign:
                return {
                    {"F2/Enter", "Start the net, with you checked in as #1.", false},
                    {"Esc", "Back to choosing a role.", false},
                };
            case kPageActiveNet:
                if (state->viewing_only)
                {
                    return {
                        {"F7", "Export this session's log to a file.", false},
                        {"Esc", "Stop watching; back to the net list.", false},
                        {"F6", "A station's other check-ins to this net (by #).", true},
                        {"F8", "Regulars who haven't checked in yet.", true},
                        {"F9", "Everything known about a station (by #).", true},
                        {"F10", "This session so far: first-timers, recent average.", true},
                        {"Up/Down", "Move the highlight.", false},
                    };
                }
                return {
                    {"F2", "Check in a station (the New Check-In window).", false},
                    {"F3", "Edit a check-in, chosen by its #.", false},
                    {"F4", "Close this session; it moves to History.", false},
                    {"F5", "Delete a check-in, chosen by its #.", false},
                    {"F7", "Export this session's log to a file.", false},
                    {"F6", "A station's other check-ins to this net (by #).", true},
                    {"F8", "Regulars who haven't checked in yet; Enter logs one.", true},
                    {"F9", "Everything known about a station (by #).", true},
                    {"F10", "This session so far: first-timers, recent average.", true},
                    {"Up/Down", "Move the highlight; Enter edits that check-in.", false},
                };
            case kPageSettings:
                return {
                    {"F2", "Save your settings.", false},
                    {"Esc", "Cancel.", false},
                    {"F3", "Refresh the station data now (console only).", false},
                    {"F4", "Manage SSH users (console only).", false},
                    {"Left/Right", "Change the time format.", false},
                };
            case kPageAdHocNet:
                return {
                    {"F2", "Start an ad hoc net with the details entered.", false},
                    {"F3", "Resume an ad hoc session left open (by number).", false},
                    {"F6", "History of every ad hoc net.", false},
                    {"Esc", "Back to the net list.", false},
                };
            case kPageNetHistory:
                return {
                    {"Up/Down", "Choose a session; its check-ins show below.", false},
                    {"F4", "Delete a check-in from that session (by #).", false},
                    {"F5", "Delete a closed session (by number).", false},
                    {"F7", "Export the highlighted session's log.", false},
                    {"F8", "Statistics for this net.", true},
                    {"F9", "Find a station's check-ins to every net.", true},
                    {"Esc", "Back.", false},
                };
            case kPageEditNet:
                return {
                    {"F2", "Save the net's details and return to the list.", false},
                    {"F4", "Remove a saved station (by number).", false},
                    {"F6", "Add a saved station.", false},
                    {"F7", "Export this net's saved stations to a file.", false},
                    {"F8", "Delete this net and all its history.", false},
                    {"F9", "Edit a saved station (by number, or Enter).", false},
                    {"F5", "Saved stations that haven't checked in lately.", true},
                    {"Esc", "Back without saving.", false},
                };
            case kPageImportNet:
                return {
                    {"F2/Enter", "Import the highlighted file.", false},
                    {"F3", "Receive a file from your terminal (ZMODEM).", false},
                    {"Esc", "Back.", false},
                };
            case kPageManageUsers:
                return {
                    {"F2", "Add the SSH user entered (or update their key).", false},
                    {"F3", "Remove an SSH user (by number).", false},
                    {"Esc", "Back to Settings.", false},
                };
            default:
                return {};
        }
    }

    void OpenHelp(AppState* state)
    {
        std::vector<std::vector<std::string>> rows;
        bool any_extra = false;
        for (const HelpLine& line : HelpFor(state))
        {
            rows.push_back({line.key, std::string(line.what) + (line.extra ? " *" : "")});
            any_extra = any_extra || line.extra;
        }
        std::vector<std::string> summary;
        if (any_extra)
        {
            summary.emplace_back(
                "* Seldom-used keys: they always work, and appear on the key bar when the "
                "terminal is wide enough to show them.");
        }
        ShowInfoWindow(state, InfoWindow::kHelp, "Help", summary,
                       {{"Key", 10, 10, 0, 0}, {"What it does", 1, 1, 0, 0}}, rows);
    }

}  // namespace ql
