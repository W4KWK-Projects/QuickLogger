#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

#include "app_state.hpp"

namespace ql
{

    // An Input's on_change: uppercases `*field` in place as the operator
    // types, e.g. entering "aa4fa" reads back as "AA4FA" immediately rather
    // than only once saved. Deliberately takes the field directly instead of
    // an AppState* (unlike every other handler here) since it's reused across
    // unrelated forms (Enter Callsign, Settings, edit-net's saved-station
    // form) that share nothing but "this Input holds a callsign".
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
    // recurring nets to start yet.
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

    // Enter on the callsign-suggestions menu: copies the highlighted
    // suggestion's callsign into the modal, prefills Name/Member ID from it,
    // and clears the suggestion list.
    class SelectCallsignSuggestionHandler
    {
    public:
        explicit SelectCallsignSuggestionHandler(AppState* state) : state_(state) {}

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

    // Escape inside the New Station modal: logs the current entry if a
    // callsign was typed, then closes the modal.
    class LogAndCloseHandler
    {
    public:
        explicit LogAndCloseHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F4 on the active-net page: closes out the net instance and returns to
    // the net list.
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

    // F5 on the active-net page (no modal open): removes the highlighted
    // check-in entirely.
    class RemoveSelectedCheckInHandler
    {
    public:
        explicit RemoveSelectedCheckInHandler(AppState* state) : state_(state) {}

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

        bool operator()(ftxui::Event event) const;

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

    // F5 on the net history page: permanently deletes the highlighted net
    // instance and its check-ins.
    class DeleteSelectedNetInstanceHandler
    {
    public:
        explicit DeleteSelectedNetInstanceHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the net history page: Escape Back, F5 Delete.
    class NetHistoryKeyHandler
    {
    public:
        explicit NetHistoryKeyHandler(AppState* state) : state_(state) {}

        bool operator()(ftxui::Event event) const;

    private:
        AppState* state_;
    };

    // F7 on the net list page: loads the selected net's fields and saved
    // stations into the edit-net form and switches to it, unless there are no
    // recurring nets yet.
    class ShowEditNetPageHandler
    {
    public:
        explicit ShowEditNetPageHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F2 on the edit-net page: saves the net's fields. Stays on the page
    // (rather than returning to the net list) so saved stations can still be
    // managed.
    class SaveEditNetHandler
    {
    public:
        explicit SaveEditNetHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F3 on the edit-net page: adds the saved-station mini-form's callsign as a
    // known station for this net.
    class SaveNetStationFormHandler
    {
    public:
        explicit SaveNetStationFormHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F4 on the edit-net page: removes the highlighted saved station.
    class RemoveSavedNetStationHandler
    {
    public:
        explicit RemoveSavedNetStationHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F5 on the edit-net page: deletes the highlighted saved station
    // entirely (its Station record, not just its association with this
    // net) -- refuses if it has real check-in history. See
    // Database::DeleteStationCompletely.
    class DeleteSelectedSavedStationHandler
    {
    public:
        explicit DeleteSelectedSavedStationHandler(AppState* state) : state_(state) {}

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

    // Enter on the saved-station mini-form's callsign field: if suggestions
    // are showing, accepts the highlighted one (same as Enter on the
    // suggestion menu itself); otherwise does nothing, since there's no
    // exact-match DB lookup for this form the way CallsignLookupHandler does
    // for the New Station modal.
    class SavedStationCallsignEnterHandler
    {
    public:
        explicit SavedStationCallsignEnterHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Enter on the saved-station mini-form's suggestions menu: copies the
    // highlighted suggestion into the mini-form and clears the suggestion list.
    class SelectSavedStationSuggestionHandler
    {
    public:
        explicit SelectSavedStationSuggestionHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Enter on the edit-net page's saved-stations list: loads the highlighted
    // station's fields into the mini-form and moves focus there, so more
    // details can be added (or existing ones corrected) and saved back via
    // SaveNetStationFormHandler.
    class LoadSavedStationHandler
    {
    public:
        explicit LoadSavedStationHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // F6 on the edit-net page: clears the saved-station mini-form (so it's
    // ready for a brand new callsign rather than whatever was last loaded or
    // just saved) and moves focus straight to its callsign field, skipping
    // the six Tab-stops (Name/Mode/Frequency/Location/Recurrence/the
    // saved-station list) that otherwise sit between wherever focus is and
    // the field an operator visiting this page is usually here for.
    class AddNewSavedStationHandler
    {
    public:
        explicit AddNewSavedStationHandler(AppState* state) : state_(state) {}

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

    // Global key handling for the edit-net page: F2 Save Net, F3 Save Station,
    // F4 Remove Station (unsave from this net), F5 Delete Station (purge
    // entirely), Escape Back.
    class EditNetKeyHandler
    {
    public:
        explicit EditNetKeyHandler(AppState* state) : state_(state) {}

        bool operator()(ftxui::Event event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the net list page: F2 New Recurring Net, F3
    // Start Selected Net, F4 Settings, F5 Ad Hoc Net, F6 View History, F7 Edit
    // Net, F10 Quit.
    class NetListKeyHandler
    {
    public:
        explicit NetListKeyHandler(AppState* state) : state_(state) {}

        bool operator()(ftxui::Event event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the create-net page: F2 Save, Escape Cancel.
    class CreateNetKeyHandler
    {
    public:
        explicit CreateNetKeyHandler(AppState* state) : state_(state) {}

        bool operator()(ftxui::Event event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the select-role page: F2 Continue, Escape Back.
    class SelectRoleKeyHandler
    {
    public:
        explicit SelectRoleKeyHandler(AppState* state) : state_(state) {}

        bool operator()(ftxui::Event event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the callsign entry page: F2 Start Net, Escape Back.
    class EnterCallsignKeyHandler
    {
    public:
        explicit EnterCallsignKeyHandler(AppState* state) : state_(state) {}

        bool operator()(ftxui::Event event) const;

    private:
        AppState* state_;
    };

    // Global key handling for the active-net page. With no modal open: F2 New
    // Station, F3 Edit Check-In, F4 Close/Save Net, F5 Remove Check-In. With
    // the New Station modal open: F2 logs and continues, Escape closes. With
    // the Edit Check-in modal open: F2 saves, Escape cancels.
    class ActiveNetKeyHandler
    {
    public:
        explicit ActiveNetKeyHandler(AppState* state) : state_(state) {}

        bool operator()(ftxui::Event event) const;

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

    // F3 on the settings page: starts the background FCC ULS import (see
    // uls_import.hpp) if one isn't already running; no-op otherwise. Used for
    // both the very first import and any forced re-run.
    class StartUlsImportHandler
    {
    public:
        explicit StartUlsImportHandler(AppState* state) : state_(state) {}

        void operator()() const;

    private:
        AppState* state_;
    };

    // Global key handling for the settings page: F2 Save, F3 Import ULS
    // database, Escape Cancel.
    class SettingsKeyHandler
    {
    public:
        explicit SettingsKeyHandler(AppState* state) : state_(state) {}

        bool operator()(ftxui::Event event) const;

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

        bool operator()(ftxui::Event event) const;

    private:
        AppState* state_;
    };

}  // namespace ql
