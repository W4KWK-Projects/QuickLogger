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

    static std::string FormatNetInstanceRow(const NetInstance& instance)
    {
        const char* status = instance.status == NetInstanceStatus::kOpen ? "OPEN" : "closed";
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-12s NC:%-11s Alt:%-11s Log:%-11s %s",
                      instance.instance_date.c_str(), instance.net_control_callsign.c_str(),
                      instance.alternate_net_control_callsign.c_str(),
                      instance.logger_callsign.c_str(), status);
        return std::string(buffer);
    }

    static std::string FormatCallsignSuggestion(const Station& station, bool is_this_net)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%-10s %-20s %s", station.callsign.c_str(),
                      station.name.c_str(), is_this_net ? "(this net)" : "(other net)");
        return std::string(buffer);
    }

    static std::string FormatSavedStationRow(const Station& station)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%-10s %-20s %s", station.callsign.c_str(),
                      station.name.c_str(), station.member_id.c_str());
        return std::string(buffer);
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
        std::snprintf(buffer, sizeof(buffer), "%-10s %-20s (ULS, %s)", station.callsign.c_str(),
                      station.name.c_str(), distance_text);
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

    std::string FormatCheckInRow(const CheckIn& check_in, const std::string& name,
                                 const std::string& member_id)
    {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%-3d %-10s %-20s %-10s %-6s %s",
                      check_in.sequence_number, check_in.callsign.c_str(), name.c_str(),
                      member_id.c_str(), check_in.signal_report.c_str(), check_in.remarks.c_str());
        return std::string(buffer);
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

    void RemoveSelectedCheckIn(AppState* state)
    {
        if (state->active_check_ins.empty())
        {
            state->form_error = "No check-ins to remove.";
            return;
        }

        const CheckIn& selected = state->active_check_ins[state->selected_check_in_index];
        state->db->DeleteCheckIn(selected.id);
        state->form_error.clear();
        RefreshActiveCheckIns(state);
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
        state->db->AddCheckIn(check_in);

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

        state->form_error.clear();
        state->show_edit_checkin_modal = true;
    }

    void SaveEditCheckInForm(AppState* state)
    {
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        state->edit_checkin_station.callsign = state->edit_checkin_original.callsign;
        state->db->UpdateStationFields(state->edit_checkin_station, now);

        CheckIn check_in = state->edit_checkin_original;
        check_in.signal_report = state->edit_checkin_signal_report;
        check_in.remarks = state->edit_checkin_remarks;
        check_in.comment = state->edit_checkin_comment;
        state->db->UpdateCheckIn(check_in);

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
