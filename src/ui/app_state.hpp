#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

#include "../db/database.hpp"
#include "../models.hpp"
#include "../settings.hpp"
#include "../uls_import.hpp"

namespace ql
{

    // Which page is currently shown inside the app's Container::Tab. Plain
    // ints (not an enum) because Container::Tab's selector must be an int*.
    constexpr int kPageNetList = 0;
    constexpr int kPageCreateNet = 1;
    constexpr int kPageSelectRole = 2;
    constexpr int kPageEnterCallsign = 3;
    constexpr int kPageActiveNet = 4;
    constexpr int kPageSettings = 5;
    constexpr int kPageAdHocNet = 6;
    constexpr int kPageNetHistory = 7;
    constexpr int kPageEditNet = 8;

    // All mutable state shared across the app's pages. Every page-building
    // function and event-handler class receives a pointer to this rather than
    // capturing individual fields, since FTXUI's callbacks need to read and
    // write whichever fields are relevant to the page they belong to.
    struct AppState
    {
        Database* db = nullptr;
        std::string db_path;  // Passed to StartUlsImport, which opens its own connection.
        ftxui::ScreenInteractive* screen = nullptr;

        int page = kPageNetList;
        std::string form_error;

        // The operator's saved settings, and where they live on disk. `settings`
        // is the last-saved value (used elsewhere in the app, e.g. to prefill
        // and stamp "created by"); `settings_form` is the page's working copy,
        // so "Cancel" can discard in-progress edits without touching `settings`.
        std::string settings_path;
        AppSettings settings;
        AppSettings settings_form;

        // FCC ULS background import, triggered from the Settings page (F3) or
        // automatically at startup when stale -- see uls_import.hpp. This is
        // the live, in-memory state a running import thread updates; once it
        // finishes, DescribeUlsImportStatus reads the persisted import_runs
        // row straight from the database on every render instead of caching
        // it here, so the displayed status can never go stale while sitting
        // on the Settings page (the query is cheap, same as e.g.
        // NetHistoryRenderer's per-frame check-in query).
        UlsImportProgress uls_import_progress;

        // In-memory cache of the whole zip_centroids table (see
        // EnsureZipCentroidsCached in app_state.cpp), so proximity lookups
        // for the saved-station form's ULS tier (RefreshNearbyZip3Prefixes,
        // RefreshSavedStationSuggestions) don't re-query the database on
        // every edit-net-page-open or keystroke -- ZIP centroids are
        // effectively static once loaded, so a one-time load per process
        // run is enough. Empty until the first call that needs it (lazy),
        // and left empty (harmlessly retried next time) if the database
        // doesn't have the data yet.
        std::vector<ZipCentroid> zip_centroids_cache;
        std::unordered_map<std::string, ZipCentroid> zip_centroids_by_zip;

        // Net list page: the recurring nets a user can select and start.
        std::vector<Net> nets;
        std::vector<std::string> net_names;  // Kept in sync with `nets` by RefreshNets.
        int selected_net_index = 0;

        // Create-net page: fields for a new recurring net.
        std::string new_net_name;
        std::string new_net_mode;
        std::string new_net_frequency;
        std::string new_net_location;
        std::string new_net_recurrence;

        // Select-role page: which role the operator is filling for this instance.
        std::vector<std::string> role_labels{"Net Control", "Alternate Net Control", "Logger"};
        int selected_role_index = kRoleNetControl;

        // Enter-callsign page: the operator's own callsign for that role.
        std::string operator_callsign;

        // Active-net page: the net instance currently being logged, and its
        // check-ins so far.
        NetInstance active_instance;
        std::string active_net_name;
        std::vector<CheckIn> active_check_ins;
        // One formatted display line per entry in `active_check_ins`, rebuilt by
        // RefreshActiveCheckIns whenever a check-in is added. Kept pre-formatted
        // (rather than joining against Station in the renderer) so the renderer
        // doesn't have to hit the database on every frame.
        std::vector<std::string> active_display_rows;
        int selected_check_in_index = 0;  // Which row is highlighted in the check-in list.

        // New Station modal, opened from the active-net page. `modal_station`
        // holds every editable Station field (callsign, name, member_id,
        // street_address, city, county, state, zip, grid_square) as one
        // struct, so autocomplete can copy a found/picked Station into it in
        // one assignment instead of field-by-field.
        bool show_new_station_modal = false;
        Station modal_station;
        std::string modal_signal_report;
        std::string modal_remarks;
        std::string modal_comment;
        // The callsign Input component, so handlers can call TakeFocus() on it
        // (e.g. right after opening the modal, or after logging a station).
        ftxui::Component modal_callsign_input;
        // Callsign autocomplete suggestions, refreshed live as the operator
        // types (see RefreshCallsignSuggestions). Tier 1 (stations known to
        // this specific net, via real check-ins or SaveNetStation) is listed
        // ahead of tier 2 (stations known to other nets -- today's stand-in
        // for the not-yet-built QRZ/ULS tier). Kept in sync 1:1 with
        // `modal_callsign_suggestion_labels` by index.
        std::vector<Station> modal_callsign_suggestions;
        std::vector<std::string> modal_callsign_suggestion_labels;
        int selected_suggestion_index = 0;
        // Optional "additional role" for this check-in (Alternate Net
        // Control / Logger -- never the operator's own role, see
        // NetInstance::operator_role). Index into `modal_role_choice_labels`;
        // both are rebuilt by RoleChoiceLabels whenever the modal is opened.
        // See ApplyCheckInRoleDesignation for how the choice is applied.
        std::vector<std::string> modal_role_choice_labels;
        int modal_role_choice_index = 0;

        // Edit Check-in modal, opened from the check-in list on the active-net
        // page. Callsign isn't editable here (that's a bigger structural change
        // than "correct a typo"), so `edit_checkin_original` just carries the
        // unchanged id/net_instance_id/callsign/sequence_number/checked_in_at
        // through to the save step; `edit_checkin_station` holds the editable
        // Station fields (its callsign is kept equal to
        // edit_checkin_original.callsign, just never shown as an Input).
        bool show_edit_checkin_modal = false;
        CheckIn edit_checkin_original;
        Station edit_checkin_station;
        std::string edit_checkin_signal_report;
        std::string edit_checkin_remarks;
        std::string edit_checkin_comment;
        // Same idea as modal_role_choice_labels/index above, for this
        // already-logged check-in.
        std::vector<std::string> edit_checkin_role_choice_labels;
        int edit_checkin_role_choice_index = 0;

        // Net history page: past instances of AppState::nets[selected_net_index],
        // and which one is highlighted. The highlighted instance's check-ins
        // are queried fresh in the renderer rather than cached here, since
        // that's simple and the data sets involved are small.
        std::vector<NetInstance> history_instances;
        std::vector<std::string> history_instance_labels;  // Kept in sync by RefreshNetHistory.
        int selected_history_index = 0;

        // Edit-net page: which Net is being edited, its field values (a
        // working copy, like settings_form), and the stations saved to it
        // (known to the net without a real check-in -- e.g. imported from
        // another logging program's history).
        std::int64_t edit_net_id = 0;
        std::string edit_net_name;
        std::string edit_net_mode;
        std::string edit_net_frequency;
        std::string edit_net_location;
        std::string edit_net_recurrence;
        std::vector<Station> edit_net_saved_stations;
        std::vector<std::string>
            edit_net_saved_station_labels;  // Kept in sync by RefreshEditNetSavedStations.
        int selected_saved_station_index = 0;

        // Edit-net page's "add/edit a saved station" mini-form.
        // `saved_station_remarks` is that station's default remarks (see
        // Database::SaveNetStation), not a Station field -- it's copied into
        // the New Station modal's Remarks when this station is picked via
        // autocomplete for this net.
        Station saved_station;
        std::string saved_station_remarks;
        // The saved-station callsign Input, so LoadSavedStationHandler can
        // TakeFocus() it after loading a row, jumping straight into editing.
        ftxui::Component saved_station_callsign_input;
        // The ZIP3 prefixes within geo_utils::kNearbyRadiusMiles of
        // AppState::settings.location, recomputed by RefreshNearbyZip3Prefixes
        // whenever the edit-net page opens or Settings is saved (not on every
        // keystroke -- it only depends on the operator's own location, not
        // what's typed). Empty if the operator hasn't set a ZIP or it's not a
        // recognized one, in which case the ULS suggestion tier below just
        // stays empty rather than erroring.
        std::vector<std::string> saved_station_nearby_zip3_prefixes;
        // Autocomplete candidates for the saved-station mini-form (see
        // RefreshSavedStationSuggestions), refreshed live as the operator
        // types the callsign: tier 1 (already known to this net, real
        // check-in or previously saved), tier 2 (known to other nets), then
        // tier 3 (nearby ULS-imported stations, if the operator has a
        // resolvable location set) -- the same three-tier priority model as
        // modal_callsign_suggestions, just with a ULS tier appended. Kept in
        // sync 1:1 with `saved_station_suggestion_labels` by index.
        std::vector<Station> saved_station_suggestions;
        std::vector<std::string> saved_station_suggestion_labels;
        int selected_saved_station_suggestion_index = 0;
    };

    // Reloads AppState::nets/net_names from the database. Call once at
    // startup and after any change that adds or removes a Net.
    void RefreshNets(AppState* state);

    // Clears the create-net form fields and any validation error.
    void ResetCreateNetForm(AppState* state);

    // Clears the role/callsign fields ahead of a fresh "start a net" flow.
    void ResetStartNetFlow(AppState* state);

    // Reloads AppState::active_check_ins/active_display_rows from the database
    // for AppState::active_instance. Call after any change to that instance's
    // check-ins.
    void RefreshActiveCheckIns(AppState* state);

    // Logs the operator (AppState::operator_callsign) as check-in #1 on the
    // just-created AppState::active_instance, designated with whichever role
    // they picked (AppState::selected_role_index) -- the same role already
    // recorded on the instance itself (NetInstance::operator_role), so the
    // check-in list's Role column shows it like any other designation. Station
    // info is resolved the same way any other check-in's would be: a known
    // station (Database::FindStationByCallsign) wins; failing that, an exact
    // ULS match; failing that, just the bare callsign. Either way the result
    // is promoted into `stations` (fill-blanks-only), same as
    // RecordManualCheckInStation elsewhere. Call once, right after
    // AppState::active_instance is set for a freshly-started net -- this is
    // what puts the operator in their own log without making them type
    // themselves into the New Station modal.
    void LogOperatorCheckIn(AppState* state);

    // Deletes the highlighted check-in (AppState::selected_check_in_index)
    // from AppState::active_instance and refreshes the list. Does not
    // renumber other check-ins' sequence numbers (a gap is harmless). Sets
    // AppState::form_error instead if there's nothing to remove. If the
    // check-in being removed held a designated role, that role's
    // NetInstance callsign field is cleared too, rather than left pointing
    // at a station that's no longer logged.
    void RemoveSelectedCheckIn(AppState* state);

    // The "additional role" choices offered for a check-in on
    // `state->active_instance`: "No additional role" plus whichever of
    // Net Control/Alternate Net Control/Logger the operator did NOT already
    // claim (NetInstance::operator_role) -- always 3 entries, since the
    // operator claims exactly one of the three. Rebuild whenever a modal
    // that shows this choice is opened.
    std::vector<std::string> RoleChoiceLabels(const AppState* state);

    // Maps a CheckIn::designated_role value to its index in the vector
    // RoleChoiceLabels(state) returns, for preselecting an already-set
    // choice (e.g. when opening Edit Check-in). kRoleNone maps to index 0.
    int RoleChoiceIndexFromRole(const AppState* state, int role);

    // The inverse of RoleChoiceIndexFromRole: maps a selected index back to
    // a kRole* constant (or kRoleNone for index 0) for persisting.
    int RoleFromRoleChoiceIndex(const AppState* state, int index);

    // Applies a check-in's role designation change: clears whichever other
    // check-in on this instance previously held `new_role` (only one
    // check-in can hold a given role at a time), updates
    // AppState::active_instance's corresponding role-callsign column (and
    // blanks the one for `old_role`, if any) in the database, and refreshes
    // AppState::active_instance from it. Does not touch the check-in's own
    // `designated_role` field/row -- the caller persists that itself (it
    // already has the whole CheckIn to save). A no-op if `new_role` equals
    // `old_role`, or if `new_role` is the operator's own role -- the UI
    // never offers that choice, this just refuses to apply it if asked.
    void ApplyCheckInRoleDesignation(AppState* state, std::int64_t check_in_id, int old_role,
                                     int new_role, const std::string& callsign);

    // Clears the New Station modal's input fields and any validation error.
    void ClearModalFields(AppState* state);

    // Validates and persists the New Station modal's current fields as a
    // check-in against AppState::active_instance. Returns false (and sets
    // AppState::form_error) without changing anything if the callsign field is
    // empty; the caller decides what to do next either way.
    bool LogStationCheckIn(AppState* state);

    // Populates the Edit Check-in modal's fields from `check_in` (and that
    // station's current editable fields, into AppState::edit_checkin_station)
    // and shows it.
    void OpenEditCheckInForm(AppState* state, const CheckIn& check_in);

    // Persists the Edit Check-in modal's fields: a direct update to the
    // Station's editable fields (blank values are saved as-is, since this is
    // an explicit correction, unlike the New Station modal) and to the
    // CheckIn's Signal Report/Remarks/Comment. There's no required field, so
    // this always succeeds.
    void SaveEditCheckInForm(AppState* state);

    // Formats one check-in as a single display line: sequence number,
    // callsign, the station's name/member ID (not stored on CheckIn itself),
    // signal report, and remarks.
    std::string FormatCheckInRow(const CheckIn& check_in, const std::string& name,
                                 const std::string& member_id);

    // The column-header line for a list of FormatCheckInRow rows -- same
    // field widths as the row formatter (so it can't drift out of alignment
    // with it), just with plain-English labels instead of data. Pass
    // `above_menu = true` when this sits directly above an ftxui::Menu (its
    // rows get FTXUI's built-in "> "/"  " 2-column indicator prefix, so the
    // header needs the same-width gutter to still line up -- see
    // kMenuEntryIndicatorWidth); pass `false` above a plain vbox of
    // ftxui::text rows (e.g. the net-history page's read-only check-in
    // detail pane), which has no such prefix.
    std::string FormatCheckInHeaderRow(bool above_menu);

    // Formats a whole list of check-ins via FormatCheckInRow, looking up each
    // one's Station along the way. Shared by the active-net page and the net
    // history page so both display check-ins identically.
    std::vector<std::string> FormatCheckInRows(Database* db, const std::vector<CheckIn>& check_ins);

    // The column-header line for a list of FormatNetInstanceRow rows.
    std::string FormatNetInstanceHeaderRow();

    // Reloads AppState::history_instances/history_instance_labels from the
    // database for AppState::nets[selected_net_index]. Call before showing the
    // net history page.
    void RefreshNetHistory(AppState* state);

    // Permanently deletes the highlighted net instance
    // (AppState::selected_history_index) and all of its check-ins, then
    // refreshes the list. Refuses (setting AppState::form_error) if there's
    // nothing highlighted, or if the instance is still open -- close it from
    // the Active Net page first, since deleting an in-progress net out from
    // under AppState::active_instance would leave that page pointing at a
    // row that no longer exists.
    void DeleteSelectedNetInstance(AppState* state);

    // Reloads AppState::modal_callsign_suggestions/_labels from
    // AppState::modal_station.callsign: tier 1 (SearchNetStationsByCallsignSubstring
    // against AppState::active_instance.net_id) first, then tier 2
    // (SearchStationsByCallsignSubstring) for anything tier 1 didn't already
    // surface, capped to a handful of results. Clears the suggestions (rather
    // than matching everything) when the callsign field is empty.
    void RefreshCallsignSuggestions(AppState* state);

    // The column-header line for a list of FormatCallsignSuggestion/
    // FormatUlsSuggestion rows (both use the same Callsign/Name column
    // widths) -- shared by the New Station modal's suggestion menu and the
    // saved-station form's suggestion menu.
    std::string FormatCallsignSuggestionHeaderRow();

    // Copies the highlighted entry of AppState::modal_callsign_suggestions
    // (AppState::selected_suggestion_index) into AppState::modal_station, pulls
    // its default remarks the same way CallsignLookupHandler does, and clears
    // the suggestion list. Shared by SelectCallsignSuggestionHandler (Enter on
    // the suggestion menu itself) and CallsignLookupHandler (Enter on the
    // callsign field while suggestions are showing), so both "accept a
    // suggestion" paths behave identically. Does nothing if there are no
    // suggestions.
    void ApplySelectedCallsignSuggestion(AppState* state);

    // Loads AppState::edit_net_* fields from `net` and its saved stations,
    // ready for the edit-net page.
    void OpenEditNetForm(AppState* state, const Net& net);

    // Validates and persists the edit-net page's field edits back to the Net
    // row (AppState::edit_net_id unchanged). Returns false (and sets
    // AppState::form_error) without changing anything if the name is empty.
    bool SaveEditNetForm(AppState* state);

    // Reloads AppState::edit_net_saved_stations/_labels for AppState::edit_net_id. Call
    // after opening the edit-net page and after any saved-station add/remove.
    void RefreshEditNetSavedStations(AppState* state);

    // The column-header line for a list of FormatSavedStationRow rows.
    std::string FormatSavedStationHeaderRow();

    // Saves AppState::saved_station (plus AppState::saved_station_remarks as its default
    // remarks) as a saved station for AppState::edit_net_id, then clears the
    // mini-form and refreshes the saved-station list. If AppState::saved_station.callsign
    // already appears in AppState::edit_net_saved_stations, this is an edit of that
    // existing saved station (direct-set via Database::UpdateSavedNetStation, so a
    // cleared field actually clears); otherwise it's a new saved station (merge-upsert
    // via Database::SaveNetStation, so a blank field just means "I don't have
    // that detail yet" rather than "erase it"). Returns false (and sets
    // AppState::form_error) without changing anything if the callsign is empty.
    bool SaveNetStationForm(AppState* state);

    // Loads `saved`'s fields (and its current default remarks) into
    // AppState::saved_station/saved_station_remarks, so an existing saved station created
    // with only a callsign can have more details filled in and saved via
    // SaveNetStationForm rather than retyped from scratch.
    void LoadSavedStationIntoForm(AppState* state, const Station& saved);

    // Removes the highlighted saved station (AppState::selected_saved_station_index)
    // from AppState::edit_net_id and refreshes the saved-station list. Sets
    // AppState::form_error instead if there's nothing to remove.
    void RemoveSelectedSavedNetStation(AppState* state);

    // Deletes the highlighted saved station (AppState::selected_saved_station_index)
    // entirely -- its saved association with every net, and the Station
    // record itself -- via Database::DeleteStationCompletely. Sets
    // AppState::form_error instead of deleting anything if the station has
    // real check-in history (see DeleteStationCompletely) or if there's
    // nothing selected to delete.
    void DeleteSelectedSavedStationCompletely(AppState* state);

    // Recomputes AppState::saved_station_nearby_zip3_prefixes from
    // AppState::settings.location. Call when the edit-net page opens and
    // whenever Settings is saved, since the result only depends on the
    // operator's own location, not on anything typed in the mini-form.
    void RefreshNearbyZip3Prefixes(AppState* state);

    // Reloads AppState::saved_station_suggestions/_labels from
    // AppState::saved_station.callsign, same three-tier priority as
    // RefreshCallsignSuggestions plus a ULS tier: tier 1 (known to this net),
    // tier 2 (known to other nets), then tier 3 (ULS-imported stations whose
    // ZIP falls in AppState::saved_station_nearby_zip3_prefixes and, when that
    // station's own ZIP centroid is known, within geo_utils::kNearbyRadiusMiles
    // of the operator's location -- skipped entirely if the operator has no
    // resolvable location set). Capped to a handful of results total; tier 3
    // never duplicates a callsign already surfaced by tier 1/2. Clears the
    // suggestions if the callsign field is empty.
    void RefreshSavedStationSuggestions(AppState* state);

    // Copies the highlighted entry of AppState::saved_station_suggestions
    // into AppState::saved_station and clears the suggestion list. Mirrors
    // ApplySelectedCallsignSuggestion for the saved-station mini-form. Does
    // nothing if there are no suggestions.
    void ApplySelectedSavedStationSuggestion(AppState* state);

}  // namespace ql
