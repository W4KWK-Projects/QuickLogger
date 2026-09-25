#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

#include "app_state.hpp"
#include "escape_splitter.hpp"

namespace ql
{

    // An Input's on_change for a callsign field: normalizes `*field` in
    // place as the operator types (see NormalizeCallsign), e.g. entering
    // "aa4fa" reads back as "AA4FA" immediately rather than only once saved,
    // and a stray space never makes it into a saved callsign. Deliberately
    // takes the field directly instead of an AppState* (unlike every other
    // handler here) since it's reused across unrelated forms (Enter
    // Callsign, Settings, edit-net's saved-station form) that share nothing
    // but "this Input holds a callsign".
    class UppercaseFieldHandler
    {
    public:
        explicit UppercaseFieldHandler(std::string* field) : field_(field) {}

        void operator()() const;

    private:
        std::string* field_;
    };

    // An Input's on_change for a ZIP code field: strips any non-digit
    // character and truncates to 5 digits as the operator types, so the
    // field can never hold anything but a plain 5-digit ZIP (or a shorter
    // in-progress prefix of one) -- e.g. pasting "27601-1234" reads back as
    // "27601" immediately. Same rationale as UppercaseFieldHandler for
    // taking the field directly rather than an AppState*.
    class ZipCodeFieldHandler
    {
    public:
        explicit ZipCodeFieldHandler(std::string* field) : field_(field) {}

        void operator()() const;

    private:
        std::string* field_;
    };

    // An Input's on_change for a frequency field (MHz): keeps only digits
    // and the first decimal point as the operator types, so "146.94 MHz"
    // reads back as "146.94". Whether it's an amateur frequency is checked
    // on save (see CheckNetFrequency).
    class FrequencyFieldHandler
    {
    public:
        explicit FrequencyFieldHandler(std::string* field) : field_(field) {}

        void operator()() const;

    private:
        std::string* field_;
    };

    // An Input's on_change for a repeater offset field: keeps only a
    // leading + or -, digits and one decimal point as the operator types.
    // Checked on save (see OffsetProblem in frequency_rules.hpp).
    class OffsetFieldHandler
    {
    public:
        explicit OffsetFieldHandler(std::string* field) : field_(field) {}

        void operator()() const;

    private:
        std::string* field_;
    };

    // An Input's on_change for a whole-number field: strips any non-digit
    // character and truncates to `max_digits` as the operator types, like
    // ZipCodeFieldHandler.
    class DigitsFieldHandler
    {
    public:
        DigitsFieldHandler(std::string* field, std::size_t max_digits)
            : field_(field), max_digits_(max_digits)
        {
        }

        void operator()() const;

    private:
        std::string* field_;
        std::size_t max_digits_;
    };

    // F2 on the net list page: clears the create-net form and switches to it.
    class ShowCreateNetPageHandler
    {
    public:
        explicit ShowCreateNetPageHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 on the create-net page: validates the form, persists the new Net,
    // and returns to the net list.
    class CreateNetSubmitHandler
    {
    public:
        explicit CreateNetSubmitHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape on the create-net page: discards the form and returns to the net list.
    class CreateNetCancelHandler
    {
    public:
        explicit CreateNetCancelHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F3 on the net list page: moves to role selection, unless there are no
    // recurring nets to start yet -- or asks about resuming first, if the
    // net has a session open (see StartSelectedNet).
    class StartSelectedNetHandler
    {
    public:
        explicit StartSelectedNetHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 on the select-role page: moves to the callsign entry page.
    class RoleContinueHandler
    {
    public:
        explicit RoleContinueHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape on the select-role page: returns to the net list.
    class RoleBackHandler
    {
    public:
        explicit RoleBackHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 (and Enter in the callsign field) on the callsign entry page: creates
    // the NetInstance and moves to the active-net page.
    class OperatorCallsignSubmitHandler
    {
    public:
        explicit OperatorCallsignSubmitHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape on the callsign entry page: returns to role selection.
    class OperatorCallsignBackHandler
    {
    public:
        explicit OperatorCallsignBackHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 on the active-net page: clears the New Station modal's fields and
    // shows it, focused on the callsign field.
    class OpenNewStationModalHandler
    {
    public:
        explicit OpenNewStationModalHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Enter key on the modal's callsign field: looks up a previously known
    // station by exact callsign and prefills Name/Member ID from it, if found.
    // Independent of the live suggestion list below -- a fallback for typing
    // the full correct callsign without picking from suggestions.
    class CallsignLookupHandler
    {
    public:
        explicit CallsignLookupHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Fires on every keystroke in the modal's callsign field: refreshes the
    // live callsign-autocomplete suggestion list (see RefreshCallsignSuggestions).
    class CallsignSuggestHandler
    {
    public:
        explicit CallsignSuggestHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 inside the New Station modal: logs the current entry, then clears the
    // fields and refocuses the callsign field for the next one.
    class LogAndContinueHandler
    {
    public:
        explicit LogAndContinueHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F3 inside the New Station modal: logs the current entry if a
    // callsign was typed, then closes the modal.
    class LogAndCloseHandler
    {
    public:
        explicit LogAndCloseHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape inside the New Station modal: closes it without logging,
    // discarding whatever was typed.
    class CancelNewStationHandler
    {
    public:
        explicit CancelNewStationHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2/Enter on the active-net page's Close Net confirmation: closes out
    // the net instance and returns to the net list (see CloseActiveNet).
    class CloseNetInstanceHandler
    {
    public:
        explicit CloseNetInstanceHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F3 on the active-net page (and Enter on the check-in list): opens the
    // Edit Check-in modal for the selected row, unless there are no check-ins yet.
    class EditSelectedCheckInHandler
    {
    public:
        explicit EditSelectedCheckInHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F7 on the active-net page (no modal open): writes the currently open
    // instance's check-ins to a plain text file under exports/ (see
    // ExportNetLog). F7 is the shared "Export" key across every page that
    // offers one (active-net, net-history, edit-net) -- keep new export
    // entry points on F7 too, rather than picking whatever's free on that
    // page, so the shortcut doesn't shift around from screen to screen.
    class ExportActiveNetLogHandler
    {
    public:
        explicit ExportActiveNetLogHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 inside the Edit Check-in modal: persists the edits and closes it.
    class SaveEditCheckInHandler
    {
    public:
        explicit SaveEditCheckInHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape inside the Edit Check-in modal: discards the edits and closes it.
    class CancelEditCheckInHandler
    {
    public:
        explicit CancelEditCheckInHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F5 on the net list page: clears the ad hoc net form and switches to it.
    class ShowAdHocNetPageHandler
    {
    public:
        explicit ShowAdHocNetPageHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 on the ad hoc net page: creates a one-off Net (no recurrence, unlike
    // the create-net page), selects it, and jumps straight to role selection
    // -- the same downstream flow as starting an existing recurring net.
    class AdHocNetSubmitHandler
    {
    public:
        explicit AdHocNetSubmitHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the ad hoc net page: F2 Save, Escape Cancel.
    class AdHocNetKeyHandler
    {
    public:
        explicit AdHocNetKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // F6 on the net list page: loads past instances of the selected net and
    // switches to the net history page, unless there are no recurring nets yet.
    class ViewNetHistoryHandler
    {
    public:
        explicit ViewNetHistoryHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // on_change for the net history page's instance Menu: keeps the
    // check-in detail pane (and its scroll position) in sync with whichever
    // instance is now highlighted.
    class HistoryInstanceChangedHandler
    {
    public:
        explicit HistoryInstanceChangedHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape on the net history page: returns to the net list.
    class NetHistoryBackHandler
    {
    public:
        explicit NetHistoryBackHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F7 on the net history page: writes the highlighted net instance's
    // check-ins to a plain text file under exports/ (see ExportNetLog) --
    // F7 is the shared "Export" key across every page that offers one, see
    // ExportActiveNetLogHandler's comment.
    class ExportNetHistoryLogHandler
    {
    public:
        explicit ExportNetHistoryLogHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the net history page: Escape Back, F7 Export
    // Log, F5 Delete.
    class NetHistoryKeyHandler
    {
    public:
        explicit NetHistoryKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // F2 on the edit-net page: saves the net's fields and returns to the net
    // list (see SaveEditNetForm).
    class SaveEditNetHandler
    {
    public:
        explicit SaveEditNetHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 (Save & Continue) and F3 (Save & Close) in the Saved Station
    // window: saves the station to this net (see SaveNetStationForm), then
    // either clears the window for the next one or closes it. With nothing
    // typed at all, F3 just closes the window.
    class SaveNetStationFormHandler
    {
    public:
        SaveNetStationFormHandler(AppState* state, bool close_after)
            : state_(state), close_after_(close_after)
        {
        }

        void operator()() const;

    private:
        AppState* state_;
        bool close_after_;
    };

    // F7 on the edit-net page: writes this net's whole saved-station list to
    // a plain text file under exports/ (see ExportSavedStations). F7 is the
    // shared "Export" key across every page that offers one, see
    // ExportActiveNetLogHandler's comment.
    class ExportSavedStationsHandler
    {
    public:
        explicit ExportSavedStationsHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Fires on every keystroke in the saved-station mini-form's callsign
    // field: uppercases it and refreshes the autocomplete suggestion list
    // (see RefreshSavedStationSuggestions -- known-station tiers plus a
    // nearby-ULS tier). Combines both since FTXUI only allows one on_change
    // per Input, mirroring how CallsignSuggestHandler folds uppercasing into
    // the New Station modal's callsign on_change.
    class SavedStationCallsignChangeHandler
    {
    public:
        explicit SavedStationCallsignChangeHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Enter on the Saved Station window's callsign field: if suggestions are
    // showing, accepts the one marked ">"; otherwise does nothing, since
    // there's no exact-match DB lookup for this form the way
    // CallsignLookupHandler does for the New Station modal.
    class SavedStationCallsignEnterHandler
    {
    public:
        explicit SavedStationCallsignEnterHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Enter on the edit-net page's saved-stations list: opens the highlighted
    // station in the Saved Station window (see LoadSavedStationIntoForm).
    class LoadSavedStationHandler
    {
    public:
        explicit LoadSavedStationHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F6 on the edit-net page: opens the Saved Station window, empty.
    class AddNewSavedStationHandler
    {
    public:
        explicit AddNewSavedStationHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Each keystroke in the Find a Station window's callsign field: runs the
    // search again (see RefreshStationSearch).
    class InfoQueryChangeHandler
    {
    public:
        explicit InfoQueryChangeHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape on the edit-net page: returns to the net list.
    class EditNetBackHandler
    {
    public:
        explicit EditNetBackHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the edit-net page: F2 Save & Close, F4 Remove
    // Station (unsave from this net), F6 Add Station, F7 Export Stations, F8
    // Delete Net (this whole net and its history, via a confirmation modal),
    // F9 Edit Station, Escape Back. While the Saved Station window is open:
    // F2 Save & Continue, F3 Save & Close, Escape Cancel.
    class EditNetKeyHandler
    {
    public:
        explicit EditNetKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // F8 on the net list page: writes the highlighted net's whole slice
    // (definition, stations, instances, check-ins) to a .qlnet file under
    // exports/ (see ExportNetSlice), for handing off to a new user of the
    // software.
    class ExportNetSliceHandler
    {
    public:
        explicit ExportNetSliceHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F9 on the net list page: refreshes the list of *.qlnet files sitting
    // under imports/ and switches to the import-net page.
    class ShowImportNetPageHandler
    {
    public:
        explicit ShowImportNetPageHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the net list page: F2 New Recurring Net, F3
    // Start Selected Net, F4 Settings, F5 Ad Hoc Net, F6 View History, F7 Edit
    // Net, F8 Export Net, F9 Import Net, F10 Quit.
    class NetListKeyHandler
    {
    public:
        explicit NetListKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // F2 on the import-net page: imports the highlighted file (see
    // ImportSelectedNetSlice).
    class ImportSelectedNetSliceHandler
    {
    public:
        explicit ImportSelectedNetSliceHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F3 on the import-net page: opens the ZMODEM confirmation modal set up
    // to receive a file into imports/ (see StartZmodemReceive).
    class StartZmodemReceiveHandler
    {
    public:
        explicit StartZmodemReceiveHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape on the import-net page: returns to the net list.
    class ImportNetBackHandler
    {
    public:
        explicit ImportNetBackHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the import-net page: F2 Import, F3 Receive via
    // ZMODEM, Escape Back.
    class ImportNetKeyHandler
    {
    public:
        explicit ImportNetKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the create-net page: F2 Save, Escape Cancel.
    class CreateNetKeyHandler
    {
    public:
        explicit CreateNetKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the select-role page: F2 Continue, Escape Back.
    class SelectRoleKeyHandler
    {
    public:
        explicit SelectRoleKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the callsign entry page: F2 Start Net, Escape Back.
    class EnterCallsignKeyHandler
    {
    public:
        explicit EnterCallsignKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the active-net page. With no modal open: F2 New
    // Station, F3 Edit Check-In, F4 Close/Save Net, F5 Remove Check-In, F7
    // Export Log. With the New Station modal open: F2 logs and continues,
    // Escape closes. With the Edit Check-in modal open: F2 saves, Escape
    // cancels.
    class ActiveNetKeyHandler
    {
    public:
        explicit ActiveNetKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // F4 on the net list page: loads a working copy of the saved settings and
    // switches to the settings page.
    class ShowSettingsPageHandler
    {
    public:
        explicit ShowSettingsPageHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 on the settings page: persists the working copy to disk and to
    // AppState::settings, then returns to the net list.
    class SaveSettingsHandler
    {
    public:
        explicit SaveSettingsHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape on the settings page: discards the working copy and returns to
    // the net list, leaving AppState::settings untouched.
    class CancelSettingsHandler
    {
    public:
        explicit CancelSettingsHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F3 on the settings page, local console only: asks the data updater (see
    // data_updater.hpp) to refresh the shared station data now rather than
    // waiting for its weekly schedule. Only the console gets this -- the data
    // is shared, so one SSH user shouldn't be able to set off a download for
    // everyone. A no-op anywhere else.
    class RequestStationDataRefreshHandler
    {
    public:
        explicit RequestStationDataRefreshHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F4 on the settings page, console sessions only (see
    // AppState::is_console_session -- Manage Users is never reachable over
    // SSH, deliberately, to avoid needing an admin/permission concept):
    // loads the user roster and switches to the Manage Users page.
    class ShowManageUsersPageHandler
    {
    public:
        explicit ShowManageUsersPageHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 on the Manage Users page: adds/updates a user from the working
    // form fields.
    class AddUserHandler
    {
    public:
        explicit AddUserHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Escape on the Manage Users page: returns to the settings page.
    class ManageUsersBackHandler
    {
    public:
        explicit ManageUsersBackHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the Manage Users page: F2 Add, F3 Remove,
    // Escape Back.
    class ManageUsersKeyHandler
    {
    public:
        explicit ManageUsersKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the settings page: F2 Save, F3 Import ULS
    // database, F4 Manage Users (console sessions only), Escape Cancel.
    class SettingsKeyHandler
    {
    public:
        explicit SettingsKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // F10 on the net list page.
    class QuitHandler
    {
    public:
        explicit QuitHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2/Enter on the ZMODEM confirmation modal (see
    // AppState::show_zmodem_confirm_modal): runs whichever operation
    // AppState::zmodem_action names -- a send or a receive. Shared by every
    // page's key handler that can show this modal (active-net, net-history,
    // edit-net, import-net), checked before that page's own F-key handling
    // the same way each already checks its other modal flags.
    class ConfirmZmodemActionHandler
    {
    public:
        explicit ConfirmZmodemActionHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Esc on the ZMODEM confirmation modal: declines whichever operation was
    // pending -- a send leaves the file wherever it was already written; a
    // receive just never happens.
    class CancelZmodemActionHandler
    {
    public:
        explicit CancelZmodemActionHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F8 on the edit-net page: opens the delete-net confirmation modal (see
    // AppState::show_delete_net_confirm_modal). Nothing is deleted yet.
    class RequestDeleteNetHandler
    {
    public:
        explicit RequestDeleteNetHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2/Enter on the delete-net confirmation modal: permanently deletes the
    // net being edited and returns to the net list.
    class ConfirmDeleteNetHandler
    {
    public:
        explicit ConfirmDeleteNetHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Esc on the delete-net confirmation modal: closes it, deleting nothing.
    class CancelDeleteNetHandler
    {
    public:
        explicit CancelDeleteNetHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the whole app: dispatches by AppState::page to
    // whichever per-page key handler applies. This wraps the outermost
    // Container::Tab in main.cpp, rather than each page individually, because
    // Container::Tab only forwards keyboard events to its active child when
    // that child's subtree reports itself focusable -- which fails when a
    // page's only widget is an empty list (e.g. no recurring nets yet, or no
    // check-ins yet), silently swallowing every keystroke including the F-keys.
    // CatchEvent's handler runs unconditionally regardless of focus state, so
    // wrapping the whole Tab with it sidesteps that entirely.
    class AppKeyHandler
    {
    public:
        explicit AppKeyHandler(AppState* state) : state_(state) {}

        bool operator()(const ftxui::Event& event) const;

    private:
        AppState* state_;
    };

    // Wraps the whole app the same way ftxui::CatchEvent(tab, AppKeyHandler)
    // does internally (call AppKeyHandler first; if unhandled, forward to
    // the wrapped Tab) but adds a try/catch around both steps. Covers every
    // source of a Database call an event can trigger -- not just an F-key
    // action dispatched by AppKeyHandler, but also a child Input/Menu's own
    // on_change/on_enter (e.g. a live autocomplete query on every keystroke)
    // that ftxui::CatchEvent's plain fallthrough would otherwise send
    // straight into the component tree with no safety net at all. This
    // matters once quicklogger.db can be written by more than one
    // connection at a time (see Database's sqlite3_busy_timeout comment):
    // even with a generous busy timeout, sustained contention can still
    // make a write time out and throw, and a net-logging tool crashing
    // outright mid-net over a few milliseconds of write contention is far
    // worse than one action failing with a visible error, so any such
    // exception is caught here and surfaced as AppState::form_error instead
    // of propagating out of main().
    //
    // Every event first goes through an EscapeSplitter, so an Esc merged
    // with the key after it is handled as the two keys really pressed (see
    // THE NO-ALT-KEYS RULE in escape_splitter.hpp).
    class SafeAppEventDispatcher : public ftxui::ComponentBase
    {
    public:
        SafeAppEventDispatcher(ftxui::Component child, AppState* state);

        bool OnEvent(ftxui::Event event) override;

        // Lays the lists out for the terminal's current width first (see
        // UpdateListWidths), so a resize takes effect on the next redraw.
        ftxui::Element Render() override;

    private:
        bool Dispatch(const ftxui::Event& event);

        AppState* state_;
        EscapeSplitter escape_splitter_;
    };

}  // namespace ql
