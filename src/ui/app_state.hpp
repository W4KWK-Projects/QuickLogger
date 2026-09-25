#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

#include "../db/database.hpp"
#include "../models.hpp"
#include "../settings.hpp"
#include "list_columns.hpp"

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
    constexpr int kPageImportNet = 9;
    constexpr int kPageManageUsers = 10;

    // Which ZMODEM operation the confirmation modal (AppState::
    // show_zmodem_confirm_modal) is currently about to run -- see
    // ConfirmZmodemAction/CancelZmodemAction. The same modal/keys are
    // shared by every page that can trigger either direction; this is what
    // tells them apart.
    enum class ZmodemAction
    {
        kSend,
        kReceive,
    };

    // Edit and delete on a list work by number: the key (e.g. F5 Delete
    // Check-In) puts the list into "pick" mode, where every row shows a
    // number; the operator types a number and presses Enter. An edit then
    // opens that row; a delete first asks for confirmation in a modal. These
    // are the actions that work that way -- see StartRowPick.
    enum class RowPickAction
    {
        kNone,
        kEditNet,               // Recurring Nets, F7
        kEditCheckIn,           // active net, F3
        kDeleteCheckIn,         // active net, F5
        kEditSavedStation,      // Edit Net, F9
        kRemoveSavedStation,    // Edit Net, F4 (un-save from this net)
        kDeleteNetInstance,     // History, F5
        kDeleteHistoryCheckIn,  // History, F4 (a check-in in a past log)
        kRemoveUser,            // Manage Users, F3
        kResumeAdHocSession,    // Ad Hoc Net, F3 (an ad hoc session left open)
        kViewStationHistory,    // active net, F6 (an extra key; see InfoWindow)
        kViewStationCard,       // active net, F9 (an extra key)
    };

    // The seldom-used windows. Their keys always work, but are only shown on
    // a page's key bar when there's room for them (see AddExtraKeysThatFit);
    // F1 Help lists them either way. Each is a read-only window over the
    // page, closed with Esc.
    enum class InfoWindow
    {
        kNone,
        kStationHistory,  // Active net: a station's other check-ins to this net.
        kRegulars,        // Active net: regulars not yet checked in this session.
        kStationCard,     // Active net: everything known about a station.
        kSessionSummary,  // Active net: this session so far.
        kNetStatistics,   // History: the whole net.
        kStationSearch,   // History: a callsign's check-ins to every net.
        kQuietStations,   // Edit Net: saved stations that haven't checked in lately.
        kHelp,            // Any page, F1: what each of the page's keys does.
    };

    // A yes/no-style question that pops up over the page before an action
    // that can't be taken back (see AppState::confirm_prompt).
    enum class ConfirmPrompt
    {
        kNone,
        kResumeNet,  // Starting a net that already has a session open.
        kCloseNet,   // F4 on the active net.
        // Someone else closed (or deleted) the active net's session: Enter
        // returns to the net list. Not a question; nothing else closes it.
        kSessionClosed,
    };

    // The on-screen lists a RowPickAction picks from.
    enum class PickList
    {
        kNone,
        kNets,
        kActiveCheckIns,
        kSavedStations,
        kNetInstances,
        kHistoryCheckIns,
        kUsers,
        kOpenAdHocSessions,
    };

    // All mutable state shared across the app's pages. Every page-building
    // function and event-handler class receives a pointer to this rather than
    // capturing individual fields, since FTXUI's callbacks need to read and
    // write whichever fields are relevant to the page they belong to.
    struct AppState
    {
        Database* db = nullptr;
        std::string db_path;
        ftxui::ScreenInteractive* screen = nullptr;
        // True for the session launched directly by main() at the real
        // console; false for a session handed off from an SSH connection
        // (see ssh_server.hpp). The console is a permanently trusted,
        // exempt path -- it's how the very first SSH user gets added on a
        // fresh install, and it's also the only place Manage Users (see
        // kPageManageUsers) is reachable at all, deliberately never over
        // SSH, so there's no admin/permission concept to build or attack.
        bool is_console_session = true;

        int page = kPageNetList;
        std::string form_error;
        // A non-error confirmation for an action that succeeded but has
        // nothing else on screen to show for it (e.g. "Saved to
        // exports/..."), rendered in a distinct color from form_error (see
        // pages.cpp's StatusLine) so a success doesn't read as a failure.
        // Handlers that set one should clear the other, so at most one of
        // the two is ever showing at a time.
        std::string status_message;

        // ZMODEM send/receive confirmation, shown before actually running
        // `sz`/`rz` -- the transfer itself hijacks the real terminal for its
        // raw protocol bytes and can't show anything meaningful while it's
        // in flight, so the operator needs to be told to get their client
        // ready *first*, with a real chance to back out, rather than being
        // dropped into it with no warning. Shared across every page that
        // can trigger either direction (active-net/net-history/edit-net
        // export via OfferZmodemSend; the import-net page's receive) via
        // one AppState-level flag rather than a per-page copy, since only
        // one page is ever showing at a time. `zmodem_action` says which
        // operation Confirm/CancelZmodemAction should actually run;
        // `zmodem_confirm_path` only matters for kSend (the file already
        // written and waiting to go out -- kReceive has no path yet, it
        // always lands under ImportsDir).
        bool show_zmodem_confirm_modal = false;
        ZmodemAction zmodem_action = ZmodemAction::kSend;
        std::string zmodem_confirm_path;

        // Edit Net page: confirmation before Database::DeleteNetCompletely
        // (F8 there) -- this permanently erases the net's whole history
        // (every instance and check-in, plus its saved-station list), unlike
        // the numbered deletes, which each remove a single row (see
        // RowPickAction). Same bare-Renderer-modal shape as the ZMODEM
        // confirmation above.
        bool show_delete_net_confirm_modal = false;
        // The Saved Station window over the Edit Net page (F6 Add Station,
        // F9 Edit Station), holding AppState::saved_station.
        bool show_saved_station_modal = false;

        // The open InfoWindow, if any (show_info_window mirrors it for
        // ftxui::Modal): a title, summary lines, and a scrollable list of rows
        // under a column header, one row highlighted.
        InfoWindow info_window = InfoWindow::kNone;
        bool show_info_window = false;
        std::string info_title;
        std::vector<std::string> info_summary;
        std::string info_header;
        std::vector<std::string> info_rows;
        // The table behind info_header/info_rows (see list_columns.hpp), so
        // it's laid out again when the terminal is resized.
        std::vector<ListColumn> info_columns;
        std::vector<std::vector<std::string>> info_cells;
        // A tag shown after one column's text on some rows, e.g. " (ad hoc)"
        // after an ad hoc net's name in Find a Station: one per row of
        // info_cells ("" for none), or empty. The text is cut to make room
        // for the tag rather than the tag being cut, however narrow the
        // column. See FormatInfoTable.
        std::vector<std::string> info_cell_tags;
        std::size_t info_tag_column = 0;
        int info_selected = 0;
        // kRegulars: the station on each row (Enter checks it in).
        std::vector<Station> info_stations;
        // kStationSearch: the callsign being searched for.
        std::string info_query;

        // A ConfirmPrompt showing over the page: which one, and its text.
        // `resume_instance` is the open session kResumeNet offers to resume.
        bool show_confirm_prompt = false;
        ConfirmPrompt confirm_prompt = ConfirmPrompt::kNone;
        std::string confirm_prompt_title;
        std::vector<std::string> confirm_prompt_lines;
        NetInstance resume_instance;

        // Row picking (see RowPickAction). While row_pick_action isn't
        // kNone, the list it belongs to shows row_pick_numbers[i] beside row
        // i and every key goes to HandleRowPickKey; row_pick_digits is what's
        // been typed so far.
        RowPickAction row_pick_action = RowPickAction::kNone;
        std::string row_pick_digits;
        std::vector<int> row_pick_numbers;
        // The delete confirmation a pick leads to: which action, on which
        // row, and the text to show.
        bool show_row_delete_confirm_modal = false;
        RowPickAction row_delete_action = RowPickAction::kNone;
        int row_delete_index = -1;
        std::string row_delete_title;
        std::vector<std::string> row_delete_lines;

        // Import-net page: the *.qlnet files found under ImportsDir(db_path)
        // last time it was (re)opened or a ZMODEM receive completed, and
        // which one is highlighted. Refreshed by RefreshImportNetFiles.
        std::vector<std::string> import_net_files;
        int selected_import_file_index = 0;

        // Manage Users page (console-only -- see kPageManageUsers and
        // AppState::is_console_session): the current SSH login roster and
        // which one is highlighted, refreshed by RefreshUsers. `new_user_*`
        // are the add-user mini-form's working fields, cleared after a
        // successful add.
        std::vector<User> manage_users;
        std::vector<std::string> manage_users_labels;
        int selected_user_index = 0;
        std::string new_user_username;
        std::string new_user_public_key;

        // The operator's saved settings, and where they live on disk. `settings`
        // is the last-saved value (used elsewhere in the app, e.g. to prefill
        // and stamp "created by"); `settings_form` is the page's working copy,
        // so "Cancel" can discard in-progress edits without touching `settings`.
        std::string settings_path;
        AppSettings settings;
        AppSettings settings_form;
        // The Settings page's clock choice (0 = 12-hour, 1 = 24-hour), an
        // index into settings_time_format_labels for its Toggle; copied into
        // settings_form.use_24_hour_clock on save.
        std::vector<std::string> settings_time_format_labels{"12-hour (3:42 PM)",
                                                             "24-hour (15:42)"};
        int settings_time_format_index = 0;
        // The Settings page's Nearby Radius field, in miles (digits only);
        // copied into settings_form.nearby_radius_miles on save.
        std::string settings_radius_text;

        // In-memory cache of the whole zip_centroids table (see
        // EnsureZipCentroidsCached in app_state.cpp), so proximity lookups
        // for the saved-station form's ULS tier (RefreshNearbyZips,
        // RefreshSavedStationSuggestions) don't re-query the database on
        // every edit-net-page-open or keystroke -- ZIP centroids are
        // effectively static once loaded, so a one-time load per process
        // run is enough. Empty until the first call that needs it (lazy),
        // and left empty (harmlessly retried next time) if the database
        // doesn't have the data yet.
        std::vector<ZipCentroid> zip_centroids_cache;
        std::unordered_map<std::string, ZipCentroid> zip_centroids_by_zip;

        // Same idea as the pair above, for the ZIP-to-county data used by
        // BackfillCountyFromZip: each ZIP's county, and -- for ZIPs that cross
        // a county line -- the county of each town inside them, keyed on
        // "<ZIP>|<TOWN>" (see ZipPlaceCounty in models.hpp).
        std::unordered_map<std::string, std::string> zip_county_by_zip;
        std::unordered_map<std::string, std::string> zip_place_county_by_key;

        // Net list page: the recurring nets a user can select and start (never
        // ad hoc ones -- see Net::is_ad_hoc).
        std::vector<Net> nets;
        std::vector<std::string> net_names;  // Kept in sync with `nets` by RefreshNets.
        int selected_net_index = 0;
        // The nets (by id) with a session open, as of the last RefreshNets.
        std::vector<std::int64_t> open_net_ids;
        // Read by the session's ScreenTicker thread, which reloads the list
        // every few seconds while it's showing, so a session someone else
        // opens or closes shows up on it (see SafeAppEventDispatcher::Render).
        std::atomic<bool> showing_net_list{false};

        // The terminal width the lists are laid out for (never less than 80;
        // see UpdateListWidths), and each list's rows as cells (see
        // list_columns.hpp), kept so that a resize only lays them out again:
        // no database reads. Each list's display lines (net_names,
        // active_display_rows...) are made from these.
        int list_width = 80;
        std::vector<std::vector<std::string>> net_cells;
        int net_name_width = 0;
        std::vector<std::vector<std::string>> active_check_in_cells;
        std::vector<std::vector<std::string>> history_instance_cells;
        std::vector<std::vector<std::string>> history_check_in_cells;
        std::vector<std::vector<std::string>> saved_station_cells;
        // Where each autocomplete match came from ("(this net)", "(ULS, ~4
        // mi)"...), one per entry of modal_callsign_suggestions /
        // saved_station_suggestions.
        std::vector<std::string> modal_callsign_suggestion_sources;
        std::vector<std::string> saved_station_suggestion_sources;

        // Create-net page: fields for a new recurring net.
        std::string new_net_name;
        std::string new_net_mode;
        std::string new_net_frequency;
        std::string new_net_location;
        std::string new_net_recurrence;

        // The net being started or resumed: set by StartSelectedNet, the Ad
        // Hoc page and the resume prompt, and read by the Select Role and
        // Enter Callsign pages. A copy, not an index into `nets`, since an ad
        // hoc net isn't in that list.
        Net start_net;

        // Ad Hoc Net page: ad hoc sessions still open (e.g. after a dropped
        // connection), which F3 offers to resume. Refreshed by
        // RefreshOpenAdHocSessions.
        std::vector<NetInstance> open_ad_hoc_sessions;
        std::vector<std::string> open_ad_hoc_labels;
        int selected_open_ad_hoc_index = 0;

        // Select-role page: which role the operator is filling for this
        // instance -- or kRoleViewer, to watch an open session.
        std::vector<std::string> role_labels{"Net Control", "Alternate Net Control", "Logger",
                                             "Viewer"};
        int selected_role_index = kRoleNetControl;

        // Enter-callsign page: the operator's own callsign for that role.
        std::string operator_callsign;

        // Active-net page: the net instance currently being logged, and its
        // check-ins so far.
        NetInstance active_instance;
        std::string active_net_name;
        // Watching the session as a Viewer: its check-ins can be seen,
        // exported and looked into, but nothing can be logged, edited,
        // deleted or closed.
        bool viewing_only = false;
        // The active net's ZIP (Net::default_location), for nearby-station
        // autocomplete; see RefreshNearbyZips.
        std::string active_net_zip;
        bool active_net_is_ad_hoc = false;
        std::vector<CheckIn> active_check_ins;
        // One formatted display line per entry in `active_check_ins`, rebuilt by
        // RefreshActiveCheckIns whenever a check-in is added. Kept pre-formatted
        // (rather than joining against Station in the renderer) so the renderer
        // doesn't have to hit the database on every frame.
        std::vector<std::string> active_display_rows;
        int selected_check_in_index = 0;  // Which row is highlighted in the check-in list.
        // Read by the session's ScreenTicker thread, which checks every few
        // seconds whether someone else sharing the session has logged or
        // deleted a check-in: the session the active net page is showing (0
        // when it isn't showing one), and the count and newest id of the
        // check-ins it last loaded. Set by RefreshActiveCheckIns.
        std::atomic<std::int64_t> watched_instance_id{0};
        std::atomic<std::int64_t> shown_check_in_count{0};
        std::atomic<std::int64_t> shown_newest_check_in_id{0};

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
        // ahead of tier 2 (stations known to other nets). Kept in sync 1:1 with
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

        // Net history page: past instances of AppState::nets[selected_net_index]
        // -- or, when history_ad_hoc is set (F6 on the Ad Hoc page), of every
        // ad hoc net -- and which one is highlighted.
        bool history_ad_hoc = false;
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
        // The check-ins behind history_check_in_labels, same order.
        std::vector<CheckIn> history_check_ins;
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
        // The ZIP codes within the operator's Nearby Radius of the net's ZIP,
        // or the operator's home ZIP when the net has none (with their
        // distances, nearest first), and their ZIP3 prefixes, for the ULS tier
        // of both autocompletes (the New Station modal and the saved-station
        // form). Recomputed by RefreshNearbyZips only when that origin changes
        // -- see nearby_zips_origin -- not on every keystroke. Empty if
        // neither ZIP is a recognized one, in which case the ULS tier just
        // stays empty rather than erroring.
        std::vector<NearbyZip> nearby_zips;
        std::vector<std::string> nearby_zip3_prefixes;
        // The origin ZIP and radius the two above were computed for.
        std::string nearby_zips_origin;
        int nearby_zips_radius = 0;
        // Every ULS licensee near that origin, nearest first, in the order
        // Database::SearchNearbyUlsStations returns them. Loaded once per
        // origin (and again after kNearbyUlsReloadSeconds, to pick up a
        // station data refresh) so each keystroke is a substring scan in
        // memory rather than a database query: with many sessions typing at
        // once, that query was most of the server's CPU. A few hundred KB.
        std::vector<NearbyUlsCallsign> nearby_uls_callsigns;
        std::string nearby_uls_origin;
        std::int64_t nearby_uls_loaded_at = 0;
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

    // Loads the Settings page's working copy (AppState::settings_form and the
    // clock choice) from the saved settings. Call when opening the page.
    void OpenSettingsForm(AppState* state);

    // F2 on Settings: validates and saves AppState::settings_form (with the
    // clock choice) and applies it -- including switching every time shown
    // to the chosen clock. Returns false, setting AppState::form_error, if
    // the callsign or ZIP code is missing or the Nearby Radius is out of
    // range.
    bool SaveSettingsForm(AppState* state);

    // Reloads AppState::nets/net_names from the database. Call once at
    // startup and after any change that adds or removes a Net. Each label is
    // the net's name, when it was created or imported (so two nets with the
    // same name can be told apart), and "session open" if one is.
    void RefreshNets(AppState* state);

    // Whether the highlighted net on the net list has a session open.
    bool SelectedNetHasOpenSession(const AppState* state);

    // F3/Enter on the net list: starts a new session of the highlighted net
    // -- unless the net already has a session open (someone is logging it
    // right now, or a session ended without being closed, e.g. a dropped
    // SSH connection), in which case it first asks whether to resume that
    // session or close it and start a new one (ConfirmPrompt::kResumeNet).
    void StartSelectedNet(AppState* state);

    // Reloads AppState::open_ad_hoc_sessions/_labels. Call when showing the
    // Ad Hoc Net page.
    void RefreshOpenAdHocSessions(AppState* state);

    // F2 on the Ad Hoc Net page: saves the form as a new ad hoc net
    // (Net::is_ad_hoc) and goes on to pick a role. Sets
    // AppState::form_error instead if the form isn't valid.
    void StartAdHocNet(AppState* state);

    // The kResumeNet answers: carry on logging the open session, or close it
    // and go on to start a new one.
    void ResumeOpenNet(AppState* state);

    // Joins AppState::resume_instance as a Viewer (see
    // AppState::viewing_only) -- F4 on the resume prompt.
    void ViewOpenNet(AppState* state);

    // Continue on the role page with Viewer chosen: watches the start net's
    // open session, or says there isn't one yet.
    void ViewStartNet(AppState* state);

    // Esc while viewing: back to the net list, leaving the session as it is.
    void StopViewing(AppState* state);
    void CloseOpenNetAndStartNew(AppState* state);

    // F4 on the active net: asks before closing it (ConfirmPrompt::kCloseNet)
    // -- a closed session can't be reopened.
    void RequestCloseActiveNet(AppState* state);

    // Closes the active session and returns to the net list. If someone
    // else closed it in the meantime, their end time is kept, and it says
    // so (ShowSessionClosedPrompt).
    void CloseActiveNet(AppState* state);

    // True if AppState::active_instance is still open for logging. If
    // someone else has closed or deleted it (it may be shared -- see
    // ResumeOpenNet), shows ShowSessionClosedPrompt, naming
    // `unlogged_callsign` (if not empty) as not logged, and returns false.
    bool EnsureActiveSessionOpen(AppState* state, const std::string& unlogged_callsign);

    // Someone else has closed or deleted the active net's session -- seen by
    // the ScreenTicker within a few seconds, or when trying to log to or
    // close it. Closes anything open over the page and says so, with when it
    // was closed and any callsign typed but not logged
    // (ConfirmPrompt::kSessionClosed). The one who closed it never sees this.
    void ShowSessionClosedPrompt(AppState* state, const std::string& unlogged_callsign);

    // Enter on that prompt: back to the net list.
    void LeaveClosedSession(AppState* state);

    // Esc on any ConfirmPrompt: closes it, doing nothing.
    void CancelConfirmPrompt(AppState* state);

    // Clears the create-net form fields and any validation error.
    void ResetCreateNetForm(AppState* state);

    // Clears the role/callsign fields ahead of a fresh "start a net" flow.
    void ResetStartNetFlow(AppState* state);

    // Reloads AppState::active_check_ins/active_display_rows from the database
    // for AppState::active_instance. Call after any change to that instance's
    // check-ins.
    void RefreshActiveCheckIns(AppState* state);

    // Called on the UI thread when the ScreenTicker sees that session
    // `instance_id`'s check-ins have changed (someone else logging it):
    // reloads them and redraws, if the active net page is still showing
    // that session. Waits (does nothing; the ticker asks again) while a
    // numbered pick, a delete confirmation or the Edit Check-In dialog is
    // working from the current list.
    void RefreshActiveCheckInsFromOthers(AppState* state, std::int64_t instance_id);

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

    // ---- Lists laid out for the terminal's width (see list_columns.hpp) ----

    // Lays every list out again if `terminal_width` differs from
    // AppState::list_width (never less than 80). Called before each redraw,
    // so it costs nothing unless the terminal was resized.
    void UpdateListWidths(AppState* state, int terminal_width);

    // A check-in list's rows as cells: number, time, callsign, the station's
    // name/member ID/city and state/county (looked up, not stored on
    // CheckIn), role, signal report, remarks and comment.
    std::vector<std::vector<std::string>> CheckInCells(Database* db,
                                                       const std::vector<CheckIn>& check_ins);
    // Those rows as lines, and the header line above them (with the Menu
    // gutter), laid out for a `terminal_width`-column terminal.
    std::vector<std::string> FormatCheckInList(const std::vector<std::vector<std::string>>& cells,
                                               int terminal_width);
    std::string CheckInListHeader(int terminal_width);

    // The header line above History's sessions -- for the ad hoc history
    // (AppState::history_ad_hoc), which has a Net column -- laid out for a
    // `terminal_width`-column terminal, with the Menu gutter.
    std::string NetInstanceListHeader(int terminal_width, bool ad_hoc);

    // Reloads AppState::history_instances/history_instance_labels from the
    // database for AppState::nets[selected_net_index] (or every ad hoc net,
    // if AppState::history_ad_hoc), then calls RefreshHistoryCheckIns. Call
    // before showing the net history page.
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
    // successful write, hands off to OfferZmodemSend (see below). Shared by
    // the active-net page (the currently open instance, from
    // AppState::active_check_ins) and the net-history page (a past
    // instance, re-querying its check-ins fresh the same way
    // RefreshHistoryCheckIns does) so both "download this net's log" entry
    // points produce identical output.
    void ExportNetLog(AppState* state, const std::string& net_name, const NetInstance& instance,
                      const std::vector<CheckIn>& check_ins);

    // Writes `net_name`'s saved-station list to a plain space-delimited text
    // file the same way ExportNetLog does (including the same
    // OfferZmodemSend handoff on a successful write), containing exactly
    // the Callsign/Name/Member ID columns the edit-net page's saved-station
    // list shows on screen (not every field on the underlying Station
    // record -- this mirrors FormatSavedStationRow, which only ever showed
    // those three).
    void ExportSavedStations(AppState* state, const std::string& net_name,
                             const std::vector<Station>& saved_stations);

    // Writes everything about `net` -- its own definition, every station
    // saved to it or that's ever checked in, its instances, and their
    // check-ins (see net_slice.hpp) -- to a standalone .qlnet file under
    // exports/, for handing the whole net off to a new user of the
    // software to import into their own database. On success, hands off to
    // OfferZmodemSend the same way the plain-text exports do; on failure,
    // sets AppState::form_error.
    void ExportNetSlice(AppState* state, const Net& net);

    // Shared final step of every export above once its file is already
    // written successfully: if a ZMODEM sender (`sz`) is available, sets
    // status_message to a "saved" confirmation and opens the ZMODEM
    // confirmation modal (AppState::show_zmodem_confirm_modal,
    // zmodem_action = kSend) rather than sending immediately -- the
    // transfer hijacks the real terminal and can't show anything while in
    // flight, so the operator needs a chance to get their client ready
    // first (see ConfirmZmodemAction/CancelZmodemAction). If `sz` isn't
    // installed, status_message just says so and no modal appears.
    void OfferZmodemSend(AppState* state, const std::string& path);

    // Reloads AppState::import_net_files from whatever *.qlnet files are
    // currently sitting under ImportsDir(db_path). Call when opening the
    // import-net page and after a ZMODEM receive completes.
    void RefreshImportNetFiles(AppState* state);

    // F2 on the import-net page: reads the highlighted file
    // (AppState::import_net_files[selected_import_file_index]) via
    // ReadNetSliceFile, inserts it into the live database as a brand new
    // net via ApplyNetSlice, refreshes AppState::nets, and returns to the
    // net list. Sets AppState::form_error instead (leaving the page open)
    // if there's nothing highlighted or the file can't be read.
    void ImportSelectedNetSlice(AppState* state);

    // F3 on the import-net page: opens the ZMODEM confirmation modal with
    // zmodem_action = kReceive, so ConfirmZmodemAction runs
    // ReceiveFileViaZmodem (into ImportsDir) instead of a send.
    void StartZmodemReceive(AppState* state);

    // F2/Enter on the ZMODEM confirmation modal: runs whichever operation
    // AppState::zmodem_action names (SendFileViaZmodem for kSend,
    // ReceiveFileViaZmodem for kReceive -- both block for as long as they
    // take, which is fine here since the operator was already told what to
    // do via the modal they just dismissed to get here), folds the outcome
    // into AppState::status_message (refreshing AppState::import_net_files
    // too on a successful receive), and closes the modal.
    void ConfirmZmodemAction(AppState* state);

    // Esc on the ZMODEM confirmation modal: declines whichever operation
    // was pending -- for a send, the file stays wherever it was already
    // written; for a receive, nothing arrives -- and closes the modal.
    void CancelZmodemAction(AppState* state);

    // F8 on the edit-net page: opens the delete-net confirmation modal.
    // Nothing is deleted yet -- see ConfirmDeleteNet.
    void RequestDeleteNet(AppState* state);

    // F2/Enter on the delete-net confirmation modal: permanently deletes
    // AppState::edit_net_id via Database::DeleteNetCompletely, refreshes
    // AppState::nets, returns to the net list with a confirmation message,
    // and closes the modal.
    void ConfirmDeleteNet(AppState* state);

    // Esc on the delete-net confirmation modal: closes it without deleting
    // anything.
    void CancelDeleteNet(AppState* state);

    // ---- Picking a row by number (see RowPickAction) ----

    // Which list `action` picks from.
    PickList RowPickListFor(RowPickAction action);

    // Puts the list for `action` into pick mode: numbers every row and
    // clears anything typed. Sets AppState::form_error instead if the list is
    // empty.
    void StartRowPick(AppState* state, RowPickAction action);

    // Leaves pick mode without doing anything.
    void CancelRowPick(AppState* state);

    // A digit typed while picking (ignored past four digits), and Backspace.
    // Each moves the list's highlight to the row whose number now matches,
    // if any, so the operator sees which row they're about to pick.
    void TypeRowPickDigit(AppState* state, char digit);
    void EraseRowPickDigit(AppState* state);

    // Up/Down while picking: moves the list's highlight by `delta` rows and
    // clears anything typed, since Enter will now pick the highlighted row.
    void MoveRowPickHighlight(AppState* state, int delta);

    // The verb for the pick-mode Enter key ("Edit", "Delete", "Remove").
    std::string RowPickVerbFor(RowPickAction action);

    // Enter while picking: finds the row whose number was typed (or, if
    // nothing was typed, the highlighted row), leaves pick mode and either
    // opens that row (edits) or asks to confirm (deletes). If the typed
    // number isn't on the list, says so and stays in pick mode.
    void FinishRowPick(AppState* state);

    // The one-line prompt shown under a list in pick mode, e.g. "Delete
    // which check-in? Type its number, then Enter."
    std::string RowPickPrompt(const AppState* state);

    // F2/Enter on the delete confirmation: does the delete.
    void ConfirmRowDelete(AppState* state);

    // Esc on the delete confirmation: closes it, deleting nothing.
    void CancelRowDelete(AppState* state);

    // Reloads AppState::manage_users/_labels from Database::ListUsers. Call
    // when opening the Manage Users page and after any add/remove.
    void RefreshUsers(AppState* state);

    // F2 on the Manage Users page: creates (or overwrites the public key
    // of, if the username already exists -- see Database::CreateUser) a
    // user from AppState::new_user_username/new_user_public_key, then
    // clears the form and refreshes the list. Sets AppState::form_error
    // instead, saving nothing, if either field is blank or the key isn't a
    // valid OpenSSH public key (see ValidatePublicKey), saying what's wrong
    // and what a key should look like.
    void AddUserFromForm(AppState* state);

    // F3 on the Manage Users page: deletes the highlighted user
    // (AppState::manage_users[selected_user_index]) and refreshes the list.
    // A no-op if the list is empty.
    void RemoveSelectedUser(AppState* state);

    // Reloads AppState::modal_callsign_suggestions/_labels from
    // AppState::modal_station.callsign: tier 1 (SearchNetStationsByCallsignSubstring
    // against AppState::active_instance.net_id) first, then tier 2
    // (SearchStationsByCallsignSubstring) for anything tier 1 didn't already
    // surface, then tier 3, licensed stations from the FCC data near the
    // operator's home ZIP (same as the saved-station form's ULS tier), capped
    // to a handful of results. Clears the suggestions (rather than matching
    // everything) when the callsign field is empty.
    void RefreshCallsignSuggestions(AppState* state);

    // The header line above the autocomplete matches (the New Check-In and
    // Saved Station windows), laid out for a `terminal_width`-column
    // terminal, with the matches' "> " gutter.
    std::string MatchListHeader(int terminal_width);

    // Copies the highlighted entry of AppState::modal_callsign_suggestions
    // (AppState::selected_suggestion_index) into AppState::modal_station, pulls
    // its default remarks the same way CallsignLookupHandler does, and clears
    // the suggestion list. Called by CallsignLookupHandler (Enter on the
    // callsign field while suggestions are showing). Does nothing if there
    // are no suggestions.
    void ApplySelectedCallsignSuggestion(AppState* state);

    // Loads AppState::edit_net_* fields from `net` and its saved stations,
    // ready for the edit-net page.
    void OpenEditNetForm(AppState* state, const Net& net);

    // Validates and persists the edit-net page's field edits back to the Net
    // row (AppState::edit_net_id unchanged), then returns to the net list
    // saying so. Returns false (and sets AppState::form_error), changing
    // nothing and staying on the page, if the name is empty or the ZIP isn't
    // valid.
    bool SaveEditNetForm(AppState* state);

    // Reloads AppState::edit_net_saved_stations/_labels for AppState::edit_net_id. Call
    // after opening the edit-net page and after any saved-station add/remove.
    void RefreshEditNetSavedStations(AppState* state);

    // The header line above Edit Net's saved stations, laid out for a
    // `terminal_width`-column terminal, with the Menu gutter.
    std::string SavedStationListHeader(int terminal_width);

    // The Recurring Nets list's column headings, laid out like its rows.
    std::string NetListHeader(const AppState* state);

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

    // Opens the Saved Station window with `saved`'s fields (and its current
    // default remarks) in AppState::saved_station/saved_station_remarks, so an
    // existing saved station created with only a callsign can have more
    // details filled in and saved via SaveNetStationForm rather than retyped
    // from scratch.
    void LoadSavedStationIntoForm(AppState* state, const Station& saved);

    // Opens the Saved Station window empty, for a new station (F6).
    void OpenNewSavedStationForm(AppState* state);

    // Closes the Saved Station window, discarding whatever is in it.
    void CloseSavedStationForm(AppState* state);

    // ---- The seldom-used windows (see InfoWindow) ----

    // Active net: `check_in`'s station's other check-ins to this net.
    void OpenStationHistory(AppState* state, const CheckIn& check_in);
    // Active net: stations that checked in to at least half of this net's
    // last 10 sessions (or of all of them, if fewer) but not yet to this one.
    void OpenRegulars(AppState* state);
    // Enter in the regulars window: opens New Check-In with the highlighted
    // station filled in.
    void CheckInSelectedRegular(AppState* state);
    // Active net: everything known about `callsign`'s station.
    void OpenStationCard(AppState* state, const std::string& callsign);
    // Active net: this session's check-ins so far, its first-timers, and how
    // it compares with the net's recent sessions.
    void OpenSessionSummary(AppState* state);
    // History: statistics for the net whose history is showing.
    void OpenNetStatistics(AppState* state);
    // History: a window to search every net's check-ins by callsign, and
    // re-running that search as AppState::info_query changes.
    void OpenStationSearch(AppState* state);
    void RefreshStationSearch(AppState* state);
    // Edit Net: saved stations that haven't checked in to this net for six
    // months, or ever.
    void OpenQuietStations(AppState* state);
    // F1 on any page: the Help window, explaining every key the page has --
    // extra keys included, noting they need a wider terminal.
    void OpenHelp(AppState* state);
    // Up/Down in an InfoWindow, and Esc.
    void MoveInfoSelection(AppState* state, int delta);
    void CloseInfoWindow(AppState* state);

    // Removes the highlighted saved station (AppState::selected_saved_station_index)
    // from AppState::edit_net_id and refreshes the saved-station list. If
    // that leaves the station unused -- saved to no net, never checked in --
    // its record is deleted too (Database::DeleteUnusedStations), and the
    // status message says so. Sets AppState::form_error instead if there's
    // nothing to remove.
    void RemoveSelectedSavedNetStation(AppState* state);

    // Deletes the highlighted check-in of the History page's highlighted
    // session (AppState::selected_history_check_in_index), clearing the
    // session's Alternate NC/Logger callsign if that check-in held the role,
    // and refreshes the page.
    void DeleteSelectedHistoryCheckIn(AppState* state);

    // Recomputes AppState::nearby_zips/nearby_zip3_prefixes around
    // `net_zip` -- the ZIP of the net being logged or edited -- or, when that
    // is blank or not a ZIP on file, the operator's home ZIP
    // (AppState::settings.location). Called by both autocompletes before
    // their ULS tier; it only does the work when the origin has changed since
    // last time (or the ZIP data has only just loaded).
    void RefreshNearbyZips(AppState* state, const std::string& net_zip);

    // True if `callsign` is a valid US or Canadian call sign (see
    // IsValidCallsign). Otherwise sets AppState::form_error and returns
    // false.
    bool CheckCallsign(AppState* state, const std::string& callsign);

    // True if `zip` is acceptable as a net's ZIP: blank or 5 digits.
    // Otherwise sets AppState::form_error and returns false.
    bool CheckNetZip(AppState* state, const std::string& zip);

    // Reloads AppState::saved_station_suggestions/_labels from
    // AppState::saved_station.callsign, same three-tier priority as
    // RefreshCallsignSuggestions plus a ULS tier: tier 1 (known to this net),
    // tier 2 (known to other nets), then tier 3 (ULS-imported stations whose
    // ZIP falls in AppState::nearby_zip3_prefixes and, when that
    // station's own ZIP centroid is known, within the operator's Nearby Radius
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
    // persisted regardless of where its data came from. Uses the station's
    // ZIP: if that ZIP crosses a county line and the station's city is one
    // of the towns inside it, that town's county; otherwise the ZIP's own
    // county (where most of its residents live). See ZipCounty and
    // ZipPlaceCounty in models.hpp. The lookup tables load lazily from the
    // database on first use. This exists because ULS has no county field at
    // all, so a ULS-sourced station's county would otherwise stay blank;
    // call it right before RecordManualCheckInStation/SaveNetStation for any
    // Station that might be ULS-sourced (or manually entered with a ZIP but
    // no county).
    void BackfillCountyFromZip(AppState* state, Station* station);

}  // namespace ql
