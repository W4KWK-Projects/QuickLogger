#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

#include "../db/database.hpp"
#include "../mode_rules.hpp"
#include "../models.hpp"
#include "../net_slice.hpp"
#include "../settings.hpp"
#include "../upstream_pull.hpp"
#include "../upstream_push.hpp"
#include "list_columns.hpp"

namespace ql
{

    class PreciseGridLookup;

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
    // tells them apart. kShowFolder isn't ZMODEM: at a local terminal (see
    // IsLocalTerminal) the same modal instead offers to open the exports
    // folder in the desktop's file manager.
    enum class ZmodemAction
    {
        kSend,
        kReceive,
        kShowFolder,
        // Neither ZMODEM nor a folder to show (a console with no file manager,
        // Mosh, Windows): the modal only offers pushing the exported net
        // upstream (AppState::export_push_net_id).
        kPushOnly,
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
        kRemoveUser,            // Manage Users, F3 (the user and all their keys)
        kEditUser,              // Manage Users, F4 (opens the Edit User window)
        kRemoveUserKey,         // Manage Users' Edit User window, F3
        kResumeAdHocSession,    // Ad Hoc Net, F3 (an ad hoc session left open)
        kViewAdHocSession,      // Ad Hoc Net, F3, for a view-only user
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
        // Importing a session logged under a net name nothing like the
        // History it's going into (see NetNamesLookAlike).
        kImportOtherNet,
        // The upstream has no net of the pushed session's name, but one that
        // looks like it (AppState::push_upstream_net): push it there?
        kPushToNet,
    };

    // Runs a push (PushSessionFile) on a thread of its own and hands the
    // result back to the UI thread (FinishPush); see push_runner.hpp.
    class PushRunner;

    // Runs a pull step (PullNetList, PullNetFiles) on a thread of its own and
    // hands the result back to the UI thread; see pull_runner.hpp.
    class PullRunner;

    // The Pull window over the import page (F4): asking the upstream for its
    // nets, choosing one, then fetching what's wanted from it.
    enum class PullStage
    {
        kNone,
        kListing,   // Waiting for the list of nets.
        kChoosing,  // The nets are listed; one is highlighted.
        kFetching,  // Waiting for the files.
    };

    // Importing a .qlnet that looks like a net already here (see
    // OpenNetMergeChoice): first which net, and whether to import it as a
    // new one instead; then what merging into that net would do.
    enum class MergeStage
    {
        kNone,
        kChooseNet,
        kSummary,
    };

    // A row of the merge summary's sessions that differ (see
    // AppState::merge_conflicts), as it reads: when the file's session was
    // started ("2026-09-14  07:30 PM") and what differs ("check-ins: K4AAA
    // differs; notes differ").
    struct MergeConflictText
    {
        std::string when;
        std::string what;
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
        kUserKeys,
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
        // Looks up a picked US station's exact grid in the background (see
        // precise_grid.hpp); null where there's none (tests).
        PreciseGridLookup* grid_lookup = nullptr;
        // True for the session launched directly by main() at the real
        // console; false for a session handed off from an SSH connection
        // (see ssh_server.hpp). The console is a permanently trusted,
        // exempt path -- it's how the very first SSH user gets added on a
        // fresh install, and it's also the only place Manage Users (see
        // kPageManageUsers) is reachable at all, deliberately never over
        // SSH, so there's no admin/permission concept to build or attack.
        bool is_console_session = true;
        // An SSH user's session over Mosh (see mosh_bridge.hpp). Mosh keeps
        // the screen in step rather than passing bytes through, so ZMODEM
        // can't work: files are copied with scp or sftp instead.
        bool over_mosh = false;
        // The shared station data's status, a line each, as Settings shows
        // it (see ShowStationDataStatus): kept here so its renderer never
        // reads the database.
        std::vector<std::string> station_status_lines;
        // A view-only SSH user (see User::view_only): they can watch open
        // net sessions, look at and export history, and change their own
        // settings -- nothing else. Every page shows only the keys they
        // can use, and the functions that change anything refuse them
        // too (see RefuseViewOnly), whatever the key.
        bool view_only_user = false;
        // Who logged in over SSH; blank at the console. Their exports and
        // received files go in their own directories, cleaned up after a
        // while (see SessionExportsDir).
        std::string ssh_username;
        // False for an SSH user: their call signs are set in Manage Users
        // (User::amateur_callsign), shown on Settings but not editable
        // there. The console edits its own.
        bool callsign_editable = true;
        // Which of the Settings page's fields has focus, in the order its
        // container lists them (see BuildSettingsPage). An SSH user starts
        // on My ZIP Code: their call sign fields can't be focused.
        int settings_focus = 0;

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
        // `zmodem_send_paths` only matters for kSend (the files already
        // written and waiting to go out -- kReceive has no path yet, it
        // always lands under ImportsDir).
        bool show_zmodem_confirm_modal = false;
        ZmodemAction zmodem_action = ZmodemAction::kSend;
        std::vector<std::string> zmodem_send_paths;
        // When zmodem_send_paths is a session export's .zip: the files in
        // it, which stay in exports/. The .zip is only for the transfer, so
        // it's removed once ZMODEM is done with it (sent, failed or
        // skipped); F7 makes a fresh one.
        std::vector<std::string> zmodem_zip_contents;

        // The Session Notes window (F12, on the active net and in History):
        // which session's notes, the working copy being edited (saved to the
        // session only by F2), and the cursor in it (a byte offset; see
        // NotesEditor). Read-only for a Viewer and a view-only user.
        // `session_notes_width`/`_height` are the editing area's size, set
        // as the window is drawn.
        bool show_session_notes_modal = false;
        std::int64_t session_notes_instance_id = 0;
        std::string session_notes_title;
        std::string session_notes_text;
        int session_notes_cursor = 0;
        bool session_notes_read_only = false;
        int session_notes_width = 60;
        int session_notes_height = 10;

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
        // Each row's number as pick mode shows it, right-aligned to the
        // widest with a space after: made once when picking starts, not on
        // every frame.
        std::vector<std::string> row_pick_number_texts;
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
        // The same page, opened with F6 on History to import one session
        // (*.qlsession) rather than a whole net: into net
        // import_session_net_id, or, from ad hoc History, as a new ad hoc
        // net (import_session_ad_hoc). Esc returns to History.
        bool import_session = false;
        bool import_session_ad_hoc = false;
        std::int64_t import_session_net_id = 0;
        std::string import_session_net_name;

        // The Import or Merge window over the import page (see MergeStage):
        // the file's net, the nets here it looks like (labels kept in step),
        // which is highlighted, and whether one has the very same name, so
        // it can't be imported as a new net at all. Then, for the chosen
        // net, the plan (see PlanNetMerge), the plan's sessions that differ
        // here (indexes into merge_plan.sessions), their rows' text (made
        // with the plan, not on every frame) and which is highlighted.
        // show_merge_modal is merge_stage != kNone, for LayeredModal.
        MergeStage merge_stage = MergeStage::kNone;
        bool show_merge_modal = false;
        NetSlice merge_slice;
        bool merge_name_taken = false;
        std::vector<Net> merge_candidates;
        std::vector<std::string> merge_candidate_labels;
        int selected_merge_candidate = 0;
        std::string merge_target_name;
        NetMergePlan merge_plan;
        std::vector<std::size_t> merge_conflicts;
        std::vector<MergeConflictText> merge_conflict_texts;
        // For each of merge_plan.station_conflicts, its callsign line's text
        // and a line for each detail that differs, made once with the plan
        // rather than on every frame.
        std::vector<std::string> merge_station_callsign_texts;
        std::vector<std::vector<std::string>> merge_station_difference_texts;
        int selected_merge_conflict = 0;

        // Manage Users page (console-only -- see kPageManageUsers and
        // AppState::is_console_session), refreshed by RefreshUsers:
        // `manage_users` is every key (a row of the users table per key),
        // and the page lists one row per user -- `manage_user_names`, in
        // order, with `selected_user_index` into it -- whose access applies
        // to all their keys. The cells are laid out into the labels for
        // AppState::list_width. `new_user_*` are the add-user mini-form's
        // working fields, cleared after a successful add.
        std::vector<User> manage_users;
        std::vector<std::string> manage_user_names;
        std::vector<std::vector<std::string>> manage_users_cells;
        std::vector<std::string> manage_users_labels;
        int selected_user_index = 0;
        // The Edit User window over Manage Users (F4, or Enter on a user):
        // `rename_username` and `edit_user_access_index` (0 full access, 1
        // view-only), saved with F2; the user's keys, a row each; and a
        // field to paste another key into.
        bool show_user_keys_modal = false;
        std::string user_keys_username;
        std::vector<User> user_keys;
        std::vector<std::vector<std::string>> user_keys_cells;
        std::vector<std::string> user_keys_labels;
        int selected_user_key_index = 0;
        std::string new_key_text;
        // The My Keys window over an SSH user's Settings (F4): their own keys,
        // a row each, and the comment of the highlighted one to edit (F2
        // saves it). `ssh_key_id` is the key this session logged in with, 0
        // if unknown (over Mosh, or at the console).
        bool show_my_keys_window = false;
        std::int64_t ssh_key_id = 0;
        std::vector<User> my_keys;
        std::vector<std::string> my_keys_labels;
        int selected_my_key_index = 0;
        std::string my_key_comment_text;
        // The highlighted key's Transfer method (an index into
        // my_key_transfer_labels: 0 Ask, 1 ZMODEM, 2 SFTP), saved with F2.
        std::vector<std::string> my_key_transfer_labels{"Ask", "ZMODEM", "SFTP"};
        int my_key_transfer_index = 0;
        // This session's own key's Transfer method (kTransfer*), read at login
        // and when My Keys saves it; Ask if unknown (the console, Mosh).
        int ssh_transfer_method = 0;
        // What sends and receives files by ZMODEM; null is the real thing
        // (SendFilesViaZmodem, ReceiveFileViaZmodem). Tests put their own
        // here, since the real ones need a terminal.
        bool (*zmodem_send)(ftxui::ScreenInteractive*, const std::vector<std::string>&, std::string*, int) = nullptr;
        bool (*zmodem_receive)(ftxui::ScreenInteractive*, const std::string&, std::string*) = nullptr;
        int my_keys_focus = 0;
        std::string rename_username;
        int edit_user_access_index = 0;
        // The user's call signs in the Edit User window (see
        // User::amateur_callsign), saved with its F2.
        std::string edit_user_amateur_callsign;
        std::string edit_user_gmrs_callsign;
        std::string new_user_username;
        std::string new_user_public_key;
        std::string new_user_amateur_callsign;
        std::string new_user_gmrs_callsign;
        // The add-user form's Access choice: 0 full access, 1 view-only.
        int new_user_access_index = 0;
        std::vector<std::string> new_user_access_labels{"Full access", "View-only"};

        // The operator's saved settings, and where they live on disk. `settings`
        // is the last-saved value (used elsewhere in the app, e.g. to prefill
        // and stamp "created by"); `settings_form` is the page's working copy,
        // so "Cancel" can discard in-progress edits without touching `settings`.
        std::string settings_path;
        // The console's settings file, where the server address lives (see
        // server_address.hpp); SSH sessions read it from here too.
        std::string console_settings_path = "settings.txt";
        // The scp command that uploads a file to this user's /imports, worked
        // out once per session (see ScpUploadCommand).
        std::string scp_upload_command;
        bool scp_upload_command_ready = false;
        // Who and where scp commands go (see ScpTarget), worked out once
        // per session: that can ask DNS, which can block.
        mutable bool scp_target_ready = false;
        mutable bool scp_target_found = false;
        mutable std::string scp_user_at_host;
        mutable std::string scp_port_option;
        AppSettings settings;
        AppSettings settings_form;
        // The Settings page's clock choice (0 = 12-hour, 1 = 24-hour), an
        // index into settings_time_format_labels for its Toggle; copied into
        // settings_form.use_24_hour_clock on save.
        std::vector<std::string> settings_time_format_labels{"12-hour (3:42 PM)", "24-hour (15:42)"};
        int settings_time_format_index = 0;
        // The Settings page's Update Check choice (0 = on, 1 = off), shown
        // only at the console; copied into settings_form.check_for_updates
        // on save.
        std::vector<std::string> settings_update_check_labels{"On", "Off"};
        int settings_update_check_index = 0;
        // The Settings page's Nearby Radius field, in miles (digits only);
        // copied into settings_form.nearby_radius_miles on save.
        std::string settings_radius_text;
        // The Settings page's Server field, "host" or "host:port" (console
        // only); parsed into settings_form.server_address and server_port on
        // save. The placeholder shows what is used when it is blank.
        std::string settings_server_text;
        std::string settings_server_placeholder;

        // The Upstream Server window over Settings (F5, console only): the
        // upstream QuickLogger sessions are pushed to (Federated Logging,
        // see upstream_push.hpp). Its fields, saved into `settings` (and
        // settings_form) with its own F2.
        bool show_upstream_window = false;
        std::string upstream_host_text;
        std::string upstream_user_text;
        std::string upstream_port_text;
        // Where each field's cursor is: put at the end of what's there when
        // the window opens.
        int upstream_host_cursor = 0;
        int upstream_user_cursor = 0;
        int upstream_port_cursor = 0;
        // Which field has the focus: Host (0) when the window opens.
        int upstream_focus = 0;
        // Whether ssh and scp were found when the window opened.
        bool upstream_tools_available = true;

        // Pushing a session upstream (console only). `push_runner` is the
        // console session's, null elsewhere. While `push_running`, another
        // push is refused. The push's session, the .qlsession made for it
        // (removed once the push is over), the name it's uploaded as, the
        // session's own net name, and for kPushToNet the upstream's net to
        // confirm.
        PushRunner* push_runner = nullptr;
        bool push_running = false;
        std::int64_t push_instance_id = 0;
        std::string push_local_path;
        std::string push_remote_name;
        std::string push_session_net;
        std::string push_upstream_net;
        // The push is of a whole net (push_net_id, as a .qlnet), not a session.
        bool push_is_net = false;
        std::int64_t push_net_id = 0;
        // After an export with an upstream set: the net (F8) or the closed
        // session (F7 in History) that the export modal then offers to push
        // (F3); 0 otherwise. At most one is set.
        std::int64_t export_push_net_id = 0;
        std::int64_t export_push_instance_id = 0;

        // Pulling from the upstream (console only): the Pull window over the
        // import page (F4), for a whole net (pull_sessions false) or the
        // sessions of the net whose History opened the page (true).
        // `pull_runner` is the console session's, null elsewhere.
        // `pull_generation` goes up each time a step starts or the window
        // closes, so a late answer to an abandoned step is ignored. The
        // nets the upstream offered (labels kept in step), the highlighted
        // one, the one chosen, and the folder a session pull's files go in
        // (removed once they're imported). show_pull_modal is
        // pull_stage != kNone, for LayeredModal.
        PullRunner* pull_runner = nullptr;
        PullStage pull_stage = PullStage::kNone;
        bool show_pull_modal = false;
        bool pull_sessions = false;
        int pull_generation = 0;
        std::vector<UpstreamNet> pull_nets;
        std::vector<std::string> pull_net_labels;
        int selected_pull_net = 0;
        std::string pull_net_name;
        std::string pull_dir;

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
        // The same for the Import page: its list of files in the imports
        // folder, and whether it's of sessions (.qlsession) or nets (.qlnet).
        std::atomic<bool> showing_import_list{false};
        std::atomic<bool> import_list_sessions{false};

        // The terminal width the lists are laid out for (never less than 80;
        // see UpdateListWidths), and each list's rows as cells (see
        // list_columns.hpp), kept so that a resize only lays them out again:
        // no database reads. Each list's display lines (net_names,
        // active_display_rows...) are made from these.
        int list_width = 80;
        // The terminal's height, for how many autocomplete matches a
        // callsign window has room for (see MaxCallsignMatches).
        int screen_height = 24;
        std::vector<std::vector<std::string>> net_cells;
        // The net list's Net column: its usual width, and the narrowest it
        // goes to keep Recurrence on a middling terminal (see NetListLayout).
        int net_name_width = 0;
        int net_name_min_width = 0;
        std::vector<std::vector<std::string>> active_check_in_cells;
        std::vector<std::vector<std::string>> history_instance_cells;
        std::vector<std::vector<std::string>> history_check_in_cells;
        // Closed sessions' check-ins and cells already read, by session id,
        // so moving back and forth through History doesn't read them again
        // (see ShowHistoryCheckIns). Emptied whenever History is reloaded,
        // and whenever anyone else has changed the database since they were
        // read (Database::DataVersion, as it was before reading them).
        std::unordered_map<std::int64_t, std::pair<std::vector<CheckIn>, std::vector<std::vector<std::string>>>>
            history_check_ins_read;
        std::int64_t history_check_ins_read_version = 0;
        std::vector<std::vector<std::string>> saved_station_cells;
        // Where each autocomplete match came from ("(this net)", "(ULS, ~4
        // mi)"...), one per entry of modal_callsign_suggestions /
        // saved_station_suggestions.
        std::vector<std::string> modal_callsign_suggestion_sources;
        std::vector<std::string> saved_station_suggestion_sources;

        // Create-net page: fields for a new recurring net.
        std::string new_net_name;
        // An index into mode_labels (FM to start with); also used by the Ad
        // Hoc Net page.
        int new_net_mode_index = 0;
        std::string new_net_frequency;
        std::string new_net_offset;
        std::string new_net_tone;
        std::string new_net_location;
        std::string new_net_recurrence;
        std::string new_net_comments;
        // Partial Matching (see Net::partial_match_canada), an index into
        // partial_match_labels for its toggle; also used by the Ad Hoc Net
        // page.
        int new_net_partial_match_index = 0;
        // The Partial Matching toggle's choices, on New Recurring Net, Ad
        // Hoc Net and Edit Net: 0 is US, 1 is Canada.
        std::vector<std::string> partial_match_labels{"US", "Canada"};
        // The Mode choice's entries, on the same three pages: NetModes().
        std::vector<std::string> mode_labels = NetModes();
        // New Recurring Net and Ad Hoc Net: Amateur Radio (0) or GMRS (1),
        // an index into service_labels, and which of the two sets of radio
        // fields is showing (see SetNewNetService). A GMRS net's frequency
        // is one of GmrsChannels() (gmrs_channels.hpp), new_net_gmrs_channel
        // an index into it; its mode is always FM.
        std::vector<std::string> service_labels{"Amateur Radio", "GMRS"};
        int new_net_service_index = 0;
        bool new_net_amateur = true;
        bool new_net_gmrs = false;
        int new_net_gmrs_channel = 0;

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
        std::vector<std::string> role_labels{"Net Control", "Alternate Net Control", "Logger", "Viewer"};
        // The same, shortened where room may be tight: the session page's
        // info line and the check-in windows' role choices.
        std::vector<std::string> role_short_labels{"Net Control", "Alt. Net Control", "Logger", "Viewer"};
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
        // The active net's Partial Matching (see Net::partial_match_canada).
        bool active_net_partial_match_canada = false;
        // The active net's service (see Net::service): which call signs and
        // licensee data its check-ins use.
        NetService active_net_service = NetService::kAmateur;
        // The active net's frequency, offset and PL tone as the session page
        // shows them (see DescribeNetRadio).
        std::string active_net_radio;
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
        // Its Remarks, Comment and role choice, for the keys that jump
        // straight to them (F4/F5/F6), and the same in Edit Check-In.
        ftxui::Component modal_remarks_input;
        ftxui::Component modal_comment_input;
        ftxui::Component modal_role_input;
        ftxui::Component edit_checkin_remarks_input;
        ftxui::Component edit_checkin_comment_input;
        ftxui::Component edit_checkin_role_input;
        // Where the cursor sits in each of those Remarks and Comment fields.
        // F4/F5 put it at the end, so what's typed adds to a prefilled
        // default remark rather than landing in front of it.
        int modal_remarks_cursor = 0;
        int modal_comment_cursor = 0;
        int edit_checkin_remarks_cursor = 0;
        int edit_checkin_comment_cursor = 0;
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
        int edit_net_mode_index = 0;  // As new_net_mode_index.
        // The net being edited has no mode (its old free-text one wasn't a
        // recognized mode), so Mode shows FM until one is picked; the page
        // says so.
        bool edit_net_mode_was_blank = false;
        std::string edit_net_frequency;
        std::string edit_net_offset;
        std::string edit_net_tone;
        std::string edit_net_location;
        std::string edit_net_recurrence;
        std::string edit_net_comments;
        int edit_net_partial_match_index = 0;  // As new_net_partial_match_index.
        // The net's service, which Edit Net shows but doesn't change, and
        // for a GMRS net its channel (as new_net_gmrs_channel).
        bool edit_net_amateur = true;
        bool edit_net_gmrs = false;
        int edit_net_gmrs_channel = 0;
        std::vector<Station> edit_net_saved_stations;
        // Alongside each: its entry name (see SavedNetStation; it's the
        // Name shown when not blank) and its default remarks.
        std::vector<std::string> edit_net_saved_entry_names;
        std::vector<std::string> edit_net_saved_remarks;
        std::vector<std::string> edit_net_saved_station_labels;  // Kept in sync by RefreshEditNetSavedStations.
        int selected_saved_station_index = 0;

        // Edit-net page's "add/edit a saved station" mini-form.
        // `saved_station_remarks` is that station's default remarks (see
        // Database::SaveNetStation), not a Station field -- it's copied into
        // the New Station modal's Remarks when this station is picked via
        // autocomplete for this net.
        Station saved_station;
        std::string saved_station_remarks;
        // The saved station loaded into the form for editing (see
        // LoadSavedStationIntoForm): its call sign and entry name. Blank for
        // a new one.
        std::string saved_station_loaded_callsign;
        std::string saved_station_loaded_name;
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
        // Where that origin is, and the net and home ZIPs it was found
        // from, so a keystroke with the same ones reads nothing.
        double nearby_zips_origin_lat = 0.0;
        double nearby_zips_origin_lon = 0.0;
        std::string nearby_zips_net_zip;
        std::string nearby_zips_home_zip;
        // Every ULS licensee near that origin, nearest first, in the order
        // Database::SearchNearbyUlsStations returns them. Loaded once per
        // origin (and again after kNearbyUlsReloadSeconds, to pick up a
        // station data refresh) so each keystroke is a substring scan in
        // memory rather than a database query: with many sessions typing at
        // once, that query was most of the server's CPU. A few hundred KB.
        std::vector<NearbyUlsCallsign> nearby_uls_callsigns;
        std::string nearby_uls_origin;
        // Which licensees those are: amateur, or GMRS for a GMRS net.
        LicenseTable nearby_uls_table = LicenseTable::kAmateur;
        std::int64_t nearby_uls_loaded_at = 0;
        // The same for Canadian licensees (ISED's), by FSA.
        std::vector<NearbyUlsCallsign> nearby_ised_callsigns;
        std::string nearby_ised_origin;
        std::int64_t nearby_ised_loaded_at = 0;
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

    // Highlights, on the net list, the recurring net whose newest session
    // this operator started (Database::GetNetLastStartedBy) -- most people
    // log the same net every time, so at login it's usually just F3/Enter.
    // Leaves the highlight alone if they've started none. Call once, after
    // RefreshNets, when the session begins.
    void HighlightLastLoggedNet(AppState* state);

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
    // so (ShowSessionClosedPrompt). Returns true if it was closed here.
    bool CloseActiveNet(AppState* state);

    // F3 on the Close Net prompt, when there's an upstream (CanPushUpstream):
    // closes the session as F2 does, then pushes it (StartPush). It's closed
    // whatever becomes of the push.
    void CloseActiveNetAndPush(AppState* state);

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
    void ApplyCheckInRoleDesignation(AppState* state, std::int64_t check_in_id, int old_role, int new_role,
                                     const std::string& callsign);

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
    // CheckIn's Signal Report/Remarks/Comment. On a GMRS net the Name is
    // the check-in's own (see CheckIn::name), and renaming it to another
    // check-in's under the same call sign is refused: false, with
    // form_error set.
    bool SaveEditCheckInForm(AppState* state);

    // Shows the station data's status: the top bar's `notice` (see
    // DescribeStationDataNotice) and Settings' `status` (see
    // DescribeStationDataStatus), split into its lines. On the UI thread.
    void ShowStationDataStatus(AppState* state, const std::string& notice, bool is_problem, const std::string& status);

    // Reads the station data's status from AppState::db and shows it (see
    // ShowStationDataStatus): before the first frame, and when the 12/24-hour
    // setting changes the times in it. ScreenTicker keeps it current after.
    void ReadStationDataStatus(AppState* state);

    // ---- Lists laid out for the terminal's width (see list_columns.hpp) ----

    // Lays every list out again if `terminal_width` differs from
    // AppState::list_width (never less than 80). Called before each redraw,
    // so it costs nothing unless the terminal was resized.
    void UpdateListWidths(AppState* state, int terminal_width);

    // A check-in list's rows as cells: number, time, callsign, the station's
    // name/member ID/city and state/county (looked up, not stored on
    // CheckIn), role, signal report, remarks and comment.
    std::vector<std::vector<std::string>> CheckInCells(Database* db, const std::vector<CheckIn>& check_ins);
    // Those rows as lines, and the header line above them (with the Menu
    // gutter), laid out for a `terminal_width`-column terminal.
    std::vector<std::string> FormatCheckInList(const std::vector<std::vector<std::string>>& cells, int terminal_width);
    const std::string& CheckInListHeader(int terminal_width);

    // The header line above History's sessions -- for the ad hoc history
    // (AppState::history_ad_hoc), which has a Net column -- laid out for a
    // `terminal_width`-column terminal, with the Menu gutter.
    const std::string& NetInstanceListHeader(int terminal_width, bool ad_hoc);

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

    // The same, for moving the highlight: a closed session already read
    // (AppState::history_check_ins_read) is shown without reading it again.
    void ShowHistoryCheckIns(AppState* state);

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
    // FormatCheckInRows already produce for the on-screen list -- and next
    // to it, the session itself in a .qlsession file (GatherSessionSlice),
    // for History's F6 Import on another QuickLogger. On a failed write,
    // sets AppState::form_error and stops there. Otherwise offers both
    // files for ZMODEM download (OfferZmodemSendFiles). Shared by
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
    void ExportSavedStations(AppState* state, const std::string& net_name);

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
    // installed, status_message just says so and no modal appears. At a
    // local terminal (IsLocalTerminal) ZMODEM is never offered; with a
    // desktop to show it on, the modal offers F2 Show Folder instead
    // (zmodem_action = kShowFolder).
    // With `push_net_id` (or `push_instance_id`, a closed session), the modal
    // also offers F3 to push that net (session) to the upstream, and appears
    // when nothing else is on offer (see kPushOnly).
    void OfferZmodemSend(AppState* state, const std::string& path, std::int64_t push_net_id = 0,
                         std::int64_t push_instance_id = 0);
    // The same for several files written together, sent as one batch.
    // `zip_contents`, if given, are the files a .zip in `paths` holds, which
    // stay in exports/ once the .zip has been sent (see zmodem_zip_contents).
    // For an SSH user whose key is on Ask there is no window: the terminal is
    // asked at once whether it speaks ZMODEM (a short wait), the files are
    // sent if it does and the scp command is shown if not, and the answer is
    // kept as that key's Transfer method.
    void OfferZmodemSendFiles(AppState* state, const std::vector<std::string>& paths, std::int64_t push_net_id = 0,
                              std::int64_t push_instance_id = 0,
                              const std::vector<std::string>* zip_contents = nullptr);
    // True if an SSH user can be shown an scp command: there is an address
    // for it (see ScpTarget).
    bool HasScpAddress(const AppState* state);

    // Reloads AppState::import_net_files from whatever *.qlnet files (or
    // *.qlsession, when AppState::import_session) are currently sitting
    // under ImportsDir(db_path). Call when opening the import page and
    // after a ZMODEM receive completes.
    void RefreshImportNetFiles(AppState* state);

    // F6 on History: opens the import page for one session (see
    // AppState::import_session), to go into the net whose History it is --
    // or, on ad hoc History, to become a new ad hoc net.
    void OpenSessionImport(AppState* state);

    // F2 on the import page when importing a session: reads the highlighted
    // .qlsession file (ReadSessionSliceFile), adds its session to the net
    // (ApplySessionSlice), and returns to History with it highlighted. Sets
    // AppState::form_error instead, leaving the page open, if the file
    // can't be read or the net already has that session.
    // If the file's net name is nothing like the net's (NetNamesLookAlike),
    // it asks first (ConfirmPrompt::kImportOtherNet) instead of importing.
    void ImportSelectedSession(AppState* state);
    // F2/Enter on that question: imports it anyway.
    void ImportSelectedSessionAnyway(AppState* state);

    // Esc on the import page: back to History for a session import, else
    // to the net list.
    void LeaveImportPage(AppState* state);

    // F2 on the import-net page: reads the highlighted file
    // (AppState::import_net_files[selected_import_file_index]) via
    // ReadNetSliceFile, inserts it into the live database as a brand new
    // net via ApplyNetSlice, refreshes AppState::nets, and returns to the
    // net list. Sets AppState::form_error instead (leaving the page open)
    // if there's nothing highlighted or the file can't be read.
    // If nets already here look like the file's (NetNamesLookAlike), or
    // one has its very name, it opens the Import or Merge window instead
    // (see MergeStage).
    void ImportSelectedNetSlice(AppState* state);
    // The Import or Merge window. F2/Enter: imports the file as a new net
    // after all (not when a net here has its name).
    void ImportSelectedNetSliceAnyway(AppState* state);
    // Up/Down: the net to merge into, or in the summary, the session that
    // differs.
    void MoveMergeHighlight(AppState* state, int delta);
    // F3: what merging into the highlighted net would do (MergeStage::
    // kSummary).
    void ChooseMergeTarget(AppState* state);
    // In the summary, Left/Right/Space/Enter: Keep or Replace the
    // highlighted session that differs.
    void ToggleMergeReplace(AppState* state);
    // In the summary, F2: merges (ApplyNetMerge) and returns to the net
    // list, saying what was added.
    void ConfirmNetMerge(AppState* state);
    // Esc: from the summary back to choosing; from choosing, closes it.
    void BackOutOfNetMerge(AppState* state);

    // F3 on the import-net page: opens the ZMODEM confirmation modal with
    // zmodem_action = kReceive, so ConfirmZmodemAction runs
    // ReceiveFileViaZmodem (into ImportsDir) instead of a send.
    void StartZmodemReceive(AppState* state);

    // "scp <FILE> user@host:/imports/" for this SSH session, with -P if the port
    // isn't 22; empty if there is no address to give. Worked out once.
    const std::string& ScpUploadCommand(AppState* state);

    // F2/Enter on the ZMODEM confirmation modal: runs whichever operation
    // AppState::zmodem_action names (SendFilesViaZmodem for kSend,
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

    // A click on row `index` while picking: highlights it and clears
    // anything typed, like moving to it with Up/Down.
    void HighlightRowPickRow(AppState* state, int index);

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

    // Reloads AppState::manage_users/_cells/_labels from Database::ListUsers. Call
    // when opening the Manage Users page and after any add/remove.
    void RefreshUsers(AppState* state);

    // Opens the Edit User window for AppState::manage_user_names[index]
    // (see AppState::show_user_keys_modal).
    void OpenUserKeys(AppState* state, int index);

    // Esc in the Edit User window: closes it, leaving anything not saved.
    void CloseUserKeys(AppState* state);

    // F4 in the Edit User window (or Enter in its key field): adds the key
    // pasted into AppState::new_key_text to the window's user, at once.
    // Sets AppState::form_error instead if it isn't a valid public key.
    void AddKeyToShownUser(AppState* state);

    // F2 in the Edit User window: saves the user's username and access,
    // and closes the window. A new username renames them in the users
    // table, and their settings file and export and import directories
    // with it (see MoveSshUserFiles); it's refused, with AppState::form_error
    // set and nothing saved, if it isn't a valid username (see
    // IsValidUsername) or is already someone else's. Their access and call
    // signs (CheckUserCallsigns) apply to every key of theirs. All of it
    // takes effect from their next login.
    void SaveEditedUser(AppState* state);

    // True for an SSH user: they have keys of their own to look after (the
    // My Keys window, F4 on Settings).
    bool CanEditOwnKeys(const AppState* state);

    // F4 on Settings for an SSH user: opens My Keys with the highlight on the
    // key this session logged in with.
    void OpenMyKeys(AppState* state);
    void CloseMyKeys(AppState* state);

    // Reads this session's key's Transfer method into ssh_transfer_method.
    void LoadSessionTransferMethod(AppState* state);

    // True for an SSH user whose key is set to SFTP: ZMODEM isn't offered.
    bool SessionPrefersSftp(const AppState* state);

    // Puts the highlighted key's comment in the edit field; for when the
    // highlight moves.
    void LoadMyKeyComment(AppState* state);

    // F2 in My Keys: saves the edit field as the highlighted key's comment.
    // Sets AppState::form_error instead if it isn't a good comment.
    void SaveMyKeyComment(AppState* state);

    // Deletes the highlighted key in the Keys window
    // (AppState::user_keys[selected_user_key_index]); with it goes the
    // user, and the window closes, if it was their last.
    void RemoveSelectedUserKey(AppState* state);

    // The header line above the Keys window's list, laid out for a
    // `terminal_width`-column terminal, with the Menu gutter.
    const std::string& UserKeyListHeader(int terminal_width);
    // The same for the My Keys window's list.
    const std::string& MyKeyListHeader(int terminal_width);

    // True if `username` can be an SSH username: 1 to 32 letters, digits,
    // dots, hyphens and underscores, starting with a letter or digit. Any
    // login name, not necessarily a call sign (see User::amateur_callsign).
    bool IsValidUsername(const std::string& username);

    // Normalizes and checks a user's call signs as Manage Users and Settings
    // take them: at least one, the amateur one a US or Canadian call sign
    // without portable indicators, the GMRS one a GMRS call sign. Otherwise
    // sets AppState::form_error and returns false.
    bool CheckUserCallsigns(AppState* state, std::string* amateur, std::string* gmrs);

    // F2 on the Manage Users page: adds the key in
    // AppState::new_user_public_key to AppState::new_user_username -- a new
    // user, or another key for an existing one (see Database::CreateUser)
    // -- then clears the form and refreshes the list. Sets
    // AppState::form_error instead, saving nothing, if either field is
    // blank or the key isn't a valid OpenSSH public key (see
    // ValidatePublicKey), saying what's wrong and what a key should look
    // like.
    void AddUserFromForm(AppState* state);

    // F3 on the Manage Users page (by number, then confirmed): deletes the
    // highlighted user (AppState::manage_user_names[selected_user_index])
    // and every key of theirs, and refreshes the list. A no-op if the list
    // is empty.
    void RemoveSelectedUser(AppState* state);

    // For a view-only user (AppState::view_only_user): says they can't
    // `what` ("edit nets") in AppState::form_error and returns true, so
    // the caller changes nothing. Returns false for anyone else. Every
    // function that changes shared data checks it, whatever key led there.
    bool RefuseViewOnly(AppState* state, const std::string& what);

    // Whether this session can open Manage Users: only the local console,
    // and only in a build with the SSH server (so never on Windows), since
    // the users it manages exist only to log in over SSH.
    bool CanManageUsers(const AppState* state);

    // Whether this session can push sessions upstream: only the local
    // console (the push runs as the account QuickLogger runs under, with
    // its ~/.ssh), and only once an upstream is set (Settings, F5).
    bool CanPushUpstream(const AppState* state);

    // F5 on Settings, at the console: opens the Upstream Server window with
    // the saved upstream.
    void OpenUpstreamWindow(AppState* state);
    // F2 in it: checks and saves the upstream (a blank host is none) and
    // closes it.
    void SaveUpstreamWindow(AppState* state);
    // Esc in it: closes it, saving nothing.
    void CloseUpstreamWindow(AppState* state);

    // Pushes closed session `instance_id` upstream on PushRunner's thread,
    // confirming `confirm_net` if it isn't blank: writes its .qlsession
    // under the console's exports/, and says "Pushing to <host>...". Says
    // why not instead if it can't (no upstream, no ssh, a push already
    // running).
    bool StartPush(AppState* state, std::int64_t instance_id, const std::string& confirm_net);

    // Pushes net `net_id` upstream (as a .qlnet, its closed sessions only)
    // on PushRunner's thread, merging into the upstream's net `confirm_net`
    // if it isn't blank. Says why not instead if it can't.
    bool StartNetPush(AppState* state, std::int64_t net_id, const std::string& confirm_net);

    // True while the export modal offers a push (F3).
    bool ExportOffersPush(const AppState* state);

    // F3 on the export modal: pushes the net or session just exported.
    void PushExport(AppState* state);

    // On the UI thread once a push has ended: records pushed_at, or asks
    // about the upstream's look-alike net (ConfirmPrompt::kPushToNet), or
    // says why it failed; then removes the .qlsession unless it's needed
    // for the confirmation.
    void FinishPush(AppState* state, const PushResult& result);

    // F2/Enter and Esc on ConfirmPrompt::kPushToNet.
    void ConfirmPushToNet(AppState* state);
    void DeclinePushToNet(AppState* state);

    // True at the console with an upstream set: the import page offers F4.
    bool CanPullUpstream(const AppState* state);

    // F4 on the import page: opens the Pull window and asks the upstream
    // (Settings, F5) for its nets. Says why not instead if it can't (no
    // ssh, view-only).
    void OpenPullWindow(AppState* state);

    // On the UI thread once the upstream has answered list-nets: lists its
    // nets for choosing (in a session pull, the one with this net's name,
    // or the first that looks like it, is highlighted), or says why not.
    // Ignored if the window was closed or reopened since (`generation`).
    void FinishPullList(AppState* state, const PullResult& result, int generation);

    // Up/Down in the Pull window.
    void MovePullHighlight(AppState* state, int delta);

    // F2/Enter in the Pull window: has the upstream export the highlighted
    // net and fetches what it wrote: for a net, its .qlnet, then imported as
    // a file received any other way is; for sessions, each of its closed
    // sessions, added to this net's History (the ones it already has are
    // skipped).
    void ChoosePullNet(AppState* state);

    // On the UI thread once the files are here (or the fetch failed).
    // Ignored if the window was closed since (`generation`).
    void FinishPullFiles(AppState* state, const PullResult& result, int generation);

    // Esc in the Pull window: stops whatever is running and closes it.
    void ClosePullWindow(AppState* state);

    // Reloads AppState::modal_callsign_suggestions/_labels from
    // AppState::modal_station.callsign: tier 1 (SearchNetStationsByCallsignSubstring
    // against AppState::active_instance.net_id) first, then tier 2
    // (SearchStationsByCallsignSubstring) for anything tier 1 didn't already
    // surface, then tier 3, licensed stations from the FCC data near the
    // operator's home ZIP (same as the saved-station form's ULS tier), capped
    // to a handful of results. Clears the suggestions (rather than matching
    // everything) when the callsign field is empty.
    void RefreshCallsignSuggestions(AppState* state);

    // Rows a callsign window (New Check-In, Saved Station) needs besides
    // its match list while that list is showing: its border, title,
    // Callsign row, the hint above the list, the list's own border and
    // header, and the separator, key rows and error line below.
    constexpr int kMatchWindowOtherRows = 13;

    // How wide the check-in and Saved Station windows are on a
    // `terminal_width`-column terminal: all but a margin, so they widen
    // with it (to a limit).
    int CheckInWindowWidth(int terminal_width);

    // How many autocomplete matches a callsign window lists: as many as
    // the screen has room for, and at least 8.
    std::size_t MaxCallsignMatches(const AppState* state);

    // Fills in the New Check-In window's blank fields from what's known
    // about the callsign typed, exactly (or, failing that, without a
    // portable indicator): a station known to some net, or else the FCC
    // data at any distance -- whether or not it was among the matches
    // offered. Fields already filled in are kept. Its default remarks for
    // this net fill Remarks if that's blank. Returns whether the station
    // was found.
    bool FillCheckInFromKnownStation(AppState* state);

    // The same for the Saved Station window (Edit Net): fills
    // AppState::saved_station's blank fields, and its default remarks for
    // this net if AppState::saved_station_remarks is blank.
    bool FillSavedStationFromKnownStation(AppState* state);

    // The header line above the autocomplete matches (the New Check-In and
    // Saved Station windows), laid out for a `terminal_width`-column
    // terminal, with the matches' "> " gutter.
    const std::string& MatchListHeader(int terminal_width);

    // Copies the highlighted entry of AppState::modal_callsign_suggestions
    // (AppState::selected_suggestion_index) into AppState::modal_station, pulls
    // its default remarks the same way CallsignLookupHandler does, and clears
    // the suggestion list. Called whenever the callsign field is left while
    // suggestions are showing -- Enter, Tab, F2-F6 (see
    // ActiveNetKeyHandler) -- so the match marked ">" is always the one
    // taken. If the callsign typed is one of the matches, it's the one
    // marked (RefreshCallsignSuggestions). Does nothing if there are no
    // suggestions.
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
    const std::string& SavedStationListHeader(int terminal_width);

    // The header line above Manage Users' key list, laid out for a
    // `terminal_width`-column terminal, with the Menu gutter.
    const std::string& UserListHeader(int terminal_width);

    // The Recurring Nets list's column headings, laid out like its rows.
    const std::string& NetListHeader(const AppState* state);

    // Saves AppState::saved_station (plus AppState::saved_station_remarks as its default
    // remarks) as a saved station for AppState::edit_net_id, then clears the
    // mini-form and refreshes the saved-station list. If AppState::saved_station.callsign
    // already appears in AppState::edit_net_saved_stations, this is an edit of that
    // existing saved station (direct-set via Database::UpdateSavedNetStation, so a
    // cleared field actually clears); otherwise it's a new saved station (merge-upsert
    // via Database::SaveNetStation, so a blank field just means "I don't have
    // that detail yet" rather than "erase it"). On a GMRS net an entry is a
    // call sign and name, and the one loaded can be renamed. Returns false
    // (and sets AppState::form_error) without changing anything if the
    // callsign is empty, or is renamed to another entry's name.
    bool SaveNetStationForm(AppState* state);

    // Opens the Saved Station window with `saved`'s fields (and its current
    // default remarks) in AppState::saved_station/saved_station_remarks, so an
    // existing saved station created with only a callsign can have more
    // details filled in and saved via SaveNetStationForm rather than retyped
    // from scratch.
    void LoadSavedStationIntoForm(AppState* state, std::size_t index);

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
    // A click on the top bar's update notice: opens the
    // newer release's download page in the browser at a desktop console,
    // and otherwise says where it is. Nothing if no newer one's been found.
    void OpenUpdatePage(AppState* state);
    // F12 on the active net: the session's notes, to read and (unless
    // viewing, or a view-only user) edit.
    void OpenActiveSessionNotes(AppState* state);
    // F12 in History: the highlighted session's notes, open or closed.
    void OpenHistorySessionNotes(AppState* state);
    // The Session Notes window's F2: saves the notes to the session (after
    // trimming trailing blank lines and spaces) and closes the window.
    void SaveSessionNotes(AppState* state);
    // Esc: closes the window, leaving the session's notes as they were.
    void CloseSessionNotes(AppState* state);
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

    // The same for a call sign on a net of `service`: on a GMRS net, a GMRS
    // call sign (IsValidGmrsCallsign) instead.
    bool CheckNetCallsign(AppState* state, const std::string& callsign, NetService service);

    // `net`'s frequency, offset and PL tone for the session page, those
    // that are set: "146.940 MHz  -0.6  PL 100.0". Blank if none are.
    std::string DescribeNetRadio(const Net& net);

    // True if a net's frequency, repeater offset and PL tone are each
    // blank or valid (see FrequencyProblem, OffsetProblem, ToneProblem in
    // frequency_rules.hpp), putting `*tone` in its usual one-decimal form;
    // otherwise sets AppState::form_error.
    bool CheckNetRadio(AppState* state, const std::string& frequency, const std::string& offset, std::string* tone);

    // "Amateur Radio" or "GMRS".
    const char* ServiceLabel(NetService service);

    // A saved station's or check-in's own name on a net of `service`: on a
    // GMRS net `name` (trimmed), which tells family members sharing a call
    // sign apart; on an Amateur Radio net blank, the station's own name
    // standing. See Database::SaveNetStation and CheckIn::name.
    std::string SavedEntryName(NetService service, const std::string& name);

    // A call sign's licensee in the FCC's data for `service`: GMRS
    // licenses for GMRS, else the amateur ones (FCC, then ISED).
    std::optional<Station> FindLicensee(Database* db, const std::string& callsign, NetService service);

    // The operator's own call sign for nets of `service` (Settings, or for
    // an SSH user Manage Users): AppSettings::callsign or gmrs_callsign.
    // Blank if they have none.
    const std::string& OwnCallsign(const AppState* state, NetService service);

    // True, with AppState::form_error saying so, if the operator has no
    // call sign for `service`: they can watch its nets but not log one.
    bool RefuseWithoutCallsign(AppState* state, NetService service);

    // The Service toggle on New Recurring Net and Ad Hoc Net: shows the
    // radio fields for AppState::new_net_service_index.
    void SetNewNetService(AppState* state);

    // Fills in `net`'s service, mode, frequency, offset, PL tone and
    // Partial Matching from the New Recurring Net / Ad Hoc Net form, or from
    // Edit Net's, checking them as CheckNetRadio does. A GMRS net gets its
    // channel's frequency and offset, and FM. False (with form_error set)
    // if something's wrong.
    bool ReadNewNetRadio(AppState* state, Net* net);
    bool ReadEditNetRadio(AppState* state, Net* net);

    // True if `zip` is acceptable as a net's ZIP: blank or 5 digits.
    // Otherwise sets AppState::form_error and returns false.
    bool CheckNetZip(AppState* state, const std::string& zip);

    // The name of a recurring net other than `except_net_id` whose name is
    // the same as `name` (NetNamesAreTheSame), read fresh from the
    // database so a net someone else just made counts; empty if there's
    // none. No two recurring nets may share a name. Ad hoc nets don't
    // count, and may share names freely.
    std::string ExistingNetNamed(const AppState* state, const std::string& name, std::int64_t except_net_id = 0);

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

    // Leaving the Saved Station window's callsign (Enter, Tab, F2/F3):
    // copies the highlighted entry of AppState::saved_station_suggestions
    // into AppState::saved_station and clears the suggestion list, the same
    // as ApplySelectedCallsignSuggestion. With no suggestions, what's known
    // about the callsign typed (FillSavedStationFromKnownStation).
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
    void BackfillCountyFromZip(const AppState* state, Station* station);

    // Fills a blank Grid Square with the 4-character grid of the station's
    // ZIP centroid, so a picked or looked-up station comes with an
    // approximate grid. US 5-digit ZIPs only; a typed grid is never touched.
    void BackfillGridFromZip(const AppState* state, Station* station);

    // Asks, in the background, for a picked US station's exact 6-character
    // grid, if its grid is blank or the 4-character one BackfillGridFromZip
    // just gave it. Returns at once; nothing happens without a lookup or a
    // street address (see CanLookUpGrid).
    void RequestPreciseGrid(AppState* state, const Station& station);

    // The answer: puts `grid` in the New Station form or the saved-station
    // form if it is open for `callsign` and its grid is blank or the same
    // square (see ShouldTakePreciseGrid). Run on the UI thread.
    void ApplyPreciseGrid(AppState* state, const std::string& callsign, const std::string& grid);

}  // namespace ql
