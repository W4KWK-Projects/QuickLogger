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
        // A non-error confirmation for an action that succeeded but has
        // nothing else on screen to show for it (e.g. "Saved to
        // exports/..."), rendered in a distinct color from form_error (see
        // pages.cpp's StatusLine) so a success doesn't read as a failure.
        // Handlers that set one should clear the other, so at most one of
        // the two is ever showing at a time.
        std::string status_message;

        // ZMODEM send confirmation, shown after a file's already been
        // written locally (see ExportLinesToFile) and before actually
        // running `sz` -- the transfer itself hijacks the real terminal for
        // its raw protocol bytes and can't show anything meaningful while
        // it's in flight, so the operator needs to be told to get their
        // client's receive dialog ready *first*, with a real chance to back
        // out, rather than being dropped into it with no warning. Shared
        // across every page that can trigger an export (active-net,
        // net-history, edit-net) via one AppState-level flag/path rather
        // than a per-page copy, since only one of those pages is ever
        // showing at a time.
        bool show_zmodem_confirm_modal = false;
        std::string zmodem_confirm_path;

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

        // Same idea as the pair above, but for the ZIP-to-county lookup used
        // by BackfillCountyFromZip to fill in Station::county for a
        // ULS-sourced station (which has no county field in FCC's data at
        // all). Maps straight to the county string since there's nothing
        // else per-ZIP worth caching here.
        std::unordered_map<std::string, std::string> zip_county_by_zip;

        // City+state -> county, keyed on "<CITY>|<STATE>" (both uppercased),
        // from Database::ComputeCityCounties -- preferred over
        // zip_county_by_zip in BackfillCountyFromZip, since a city is
        // unambiguous even when one of its ZIPs straddles a county line
        // close to evenly (see CityCounty's doc comment in models.hpp).
        std::unordered_map<std::string, std::string> city_county_by_city_state;

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
        // and which one is highlighted.
        std::vector<NetInstance> history_instances;
        std::vector<std::string> history_instance_labels;  // Kept in sync by RefreshNetHistory.
        int selected_history_index = 0;
        // The highlighted instance's check-ins, formatted for display in a
        // real (scrollable) Menu below the instance list -- kept as an
        // AppState member, not recomputed inline in the renderer, since the
        // Menu binds to this vector by pointer and needs it to stay valid
        // across frames. Refreshed by RefreshHistoryCheckIns, called both
        // from RefreshNetHistory and whenever selected_history_index changes.
        std::vector<std::string> history_check_in_labels;
        int selected_history_check_in_index = 0;

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
        // Name-field Inputs for the three "open a blank/repopulated form"
        // pages below, so each page's Show*Handler can TakeFocus() the Name
        // field every time the page is (re)opened. Without this, a
        // Container::Vertical's remembered focused-child index survives
        // across repeated visits (the component tree is built once in
        // main() and reused, never rebuilt per visit -- same reason
        // LoadSavedStationHandler above needs an explicit TakeFocus()), so
        // opening the form a second time can silently start with focus left
        // on whatever field was focused last time (e.g. Recurrence, if the
        // operator tabbed there before saving) instead of Name. For Create
        // Net and Ad Hoc Net specifically, that's not just an inconvenience:
        // both forms start every field blank, so typing without noticing
        // lands each keystroke run in the wrong field, one position off per
        // repeat visit -- confirmed live by creating several nets in a row
        // and finding Name/Mode/Frequency/Location/Recurrence rotated by an
        // increasing offset in the saved rows.
        ftxui::Component new_net_name_input;
        ftxui::Component ad_hoc_net_name_input;
        ftxui::Component edit_net_name_input;
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
    // callsign, the station's name/member ID/county (not stored on CheckIn
    // itself), its designated role, and remarks. Signal report is collected
    // on the New Station/Edit Check-in forms but deliberately left out of
    // this list -- there wasn't room to keep both it and County, and County
    // is more useful here.
    std::string FormatCheckInRow(const CheckIn& check_in, const std::string& name,
                                 const std::string& member_id, const std::string& county);

    // The column-header line for a list of FormatCheckInRow rows -- same
    // field widths as the row formatter (so it can't drift out of alignment
    // with it), just with plain-English labels instead of data. Pass
    // `above_menu = true` when this sits directly above an ftxui::Menu (its
    // rows get FTXUI's built-in "> "/"  " 2-column indicator prefix, so the
    // header needs the same-width gutter to still line up -- see
    // kMenuEntryIndicatorWidth); every current call site is above a Menu, but
    // the parameter stays explicit rather than defaulted so a future
    // non-Menu use has to consciously pass `false`.
    std::string FormatCheckInHeaderRow(bool above_menu);

    // Formats a whole list of check-ins via FormatCheckInRow, looking up each
    // one's Station along the way. Shared by the active-net page and the net
    // history page so both display check-ins identically.
    std::vector<std::string> FormatCheckInRows(Database* db, const std::vector<CheckIn>& check_ins);

    // The column-header line for a list of FormatNetInstanceRow rows.
    std::string FormatNetInstanceHeaderRow();

    // Reloads AppState::history_instances/history_instance_labels from the
    // database for AppState::nets[selected_net_index], then calls
    // RefreshHistoryCheckIns. Call before showing the net history page.
    void RefreshNetHistory(AppState* state);

    // Reloads AppState::history_check_in_labels from the database for
    // whichever instance AppState::selected_history_index currently points
    // at (or clears it if the index is out of range), and resets
    // AppState::selected_history_check_in_index to 0. Called by
    // RefreshNetHistory and wired as the history instance Menu's on_change,
    // so the detail pane's check-in list -- and its scroll position -- stay
    // in sync as the user moves between instances.
    void RefreshHistoryCheckIns(AppState* state);

    // Permanently deletes the highlighted net instance
    // (AppState::selected_history_index) and all of its check-ins, then
    // refreshes the list. Refuses (setting AppState::form_error) if there's
    // nothing highlighted, or if the instance is still open -- close it from
    // the Active Net page first, since deleting an in-progress net out from
    // under AppState::active_instance would leave that page pointing at a
    // row that no longer exists.
    void DeleteSelectedNetInstance(AppState* state);

    // Writes `instance`'s check-ins to a plain space-delimited text file
    // under exports/ next to the database (see file_export.hpp), containing
    // exactly what's shown on screen for it: the net name, date, role
    // callsigns and status, then the same header/rows FormatCheckInHeaderRow/
    // FormatCheckInRows already produce for the on-screen list. On a
    // failed write, sets AppState::form_error and stops there. On a
    // successful write: if a ZMODEM sender is available, sets
    // AppState::status_message to a "saved" confirmation and opens the
    // ZMODEM confirmation modal (AppState::show_zmodem_confirm_modal) rather
    // than sending immediately -- see ConfirmZmodemSend/CancelZmodemSend;
    // if not, status_message says so and no modal appears. Shared by the
    // active-net page (the currently open instance, from
    // AppState::active_check_ins) and the net-history page (a past
    // instance, re-querying its check-ins fresh the same way
    // RefreshHistoryCheckIns does) so both "download this net's log" entry
    // points produce identical output.
    void ExportNetLog(AppState* state, const std::string& net_name, const NetInstance& instance,
                      const std::vector<CheckIn>& check_ins);

    // Writes `net_name`'s saved-station list to a plain space-delimited text
    // file the same way ExportNetLog does (including the same ZMODEM
    // confirmation-modal behavior on a successful write), containing
    // exactly the Callsign/Name/Member ID columns the edit-net page's
    // saved-station list shows on screen (not every field on the underlying
    // Station record -- this mirrors FormatSavedStationRow, which only ever
    // showed those three).
    void ExportSavedStations(AppState* state, const std::string& net_name,
                             const std::vector<Station>& saved_stations);

    // F2/Enter on the ZMODEM confirmation modal: runs the actual transfer
    // (SendFileViaZmodem, which blocks for as long as it takes -- the
    // operator was already told to get their client's receive dialog ready
    // before this point, via the modal they just dismissed to get here),
    // folds the outcome into AppState::status_message, and closes the
    // modal.
    void ConfirmZmodemSend(AppState* state);

    // Esc on the ZMODEM confirmation modal: skips the transfer entirely --
    // the file stays wherever ExportLinesToFile already wrote it, only the
    // ZMODEM step is declined -- and closes the modal.
    void CancelZmodemSend(AppState* state);

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
    // `above_menu` matches FormatCheckInHeaderRow's parameter of the same
    // name: true prepends the 2-space gutter an ftxui::Menu's own "> "/"  "
    // selection indicator needs the header to line up with; false (for a
    // plain-text export, where nothing draws that indicator) omits it.
    std::string FormatSavedStationHeaderRow(bool above_menu);

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

    // Fills in `station->county` if it's currently blank -- a no-op
    // otherwise, so it's safe to call on every station about to be
    // persisted regardless of where its data came from. Prefers
    // AppState::city_county_by_city_state (keyed on `station`'s city+state)
    // over AppState::zip_county_by_zip (keyed on its ZIP) when both are
    // available: a city is unambiguous even when one of its ZIPs straddles
    // a county line close to evenly, which is exactly where the ZIP-based
    // lookup can pick the wrong side (see CityCounty in models.hpp). Both
    // caches lazily load from the database on first use. This exists
    // specifically because ULS has no county field at all in its data, so a
    // ULS-sourced station's county would otherwise stay permanently blank;
    // call this right before RecordManualCheckInStation/SaveNetStation for
    // any Station that might be ULS-sourced (or just manually entered with
    // a ZIP but no county).
    void BackfillCountyFromZip(AppState* state, Station* station);

}  // namespace ql
