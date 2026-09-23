#include <curl/curl.h>

#include <ctime>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "db/database.hpp"
#include "settings.hpp"
#include "uls_import.hpp"
#include "ui/app_state.hpp"
#include "ui/handlers.hpp"
#include "ui/pages.hpp"

int main()
{
    // Must happen before any thread (including the background ULS import
    // thread StartUlsImport may spawn below) could call into libcurl.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
    ql::Database db("quicklogger.db");

    ql::AppState state;
    state.db = &db;
    state.db_path = "quicklogger.db";
    state.screen = &screen;
    state.settings_path = "settings.txt";
    state.settings = ql::LoadSettings(state.settings_path);

    // First launch (or an upgrade from before the ZIP code became required):
    // force the operator through Settings before anything else. Mirrors
    // ShowSettingsPageHandler's own state setup since that handler isn't
    // reachable yet -- there's no net list to press F4 from until this is
    // done. CancelSettingsHandler refuses to leave kPageSettings while
    // SettingsAreComplete is still false, so Esc can't bypass this.
    if (!ql::SettingsAreComplete(state.settings))
    {
        state.settings_form = state.settings;
        state.page = ql::kPageSettings;
    }

    ql::RefreshNets(&state);

    // Kick off a fresh FCC ULS import automatically if one has never run,
    // previously failed, is stuck "running" (only possible if a prior run
    // crashed, since a clean quit is blocked while one is active), the last
    // completed run is more than a week old, or either auxiliary step (the
    // ZIP-centroid geocode or the ZIP-to-county lookup) specifically failed
    // last time (which a fresh, still-recent `uls` row would otherwise mask
    // for up to a week -- see ShouldRetryAuxiliaryImport).
    std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (ql::ShouldAutoStartUlsImport(db.GetImportRunStatus("uls"), now) ||
        ql::ShouldRetryAuxiliaryImport(db.GetImportRunStatus("zip_centroids")) ||
        ql::ShouldRetryAuxiliaryImport(db.GetImportRunStatus("zip_counties")))
    {
        ql::StartUlsImport(state.db_path, &state.uls_import_progress, &screen);
    }

    ftxui::Component net_list_page = ql::BuildNetListPage(&state);
    ftxui::Component create_net_page = ql::BuildCreateNetPage(&state);
    ftxui::Component select_role_page = ql::BuildSelectRolePage(&state);
    ftxui::Component enter_callsign_page = ql::BuildEnterCallsignPage(&state);
    ftxui::Component active_net_page = ql::BuildActiveNetPage(&state);
    ftxui::Component settings_page = ql::BuildSettingsPage(&state);
    ftxui::Component ad_hoc_net_page = ql::BuildAdHocNetPage(&state);
    ftxui::Component net_history_page = ql::BuildNetHistoryPage(&state);
    ftxui::Component edit_net_page = ql::BuildEditNetPage(&state);

    ftxui::Component tab = ftxui::Container::Tab(
        {
            net_list_page,
            create_net_page,
            select_role_page,
            enter_callsign_page,
            active_net_page,
            settings_page,
            ad_hoc_net_page,
            net_history_page,
            edit_net_page,
        },
        &state.page);

    // Wrapping the whole Tab (rather than each page individually) matters:
    // Container::Tab only forwards keyboard events to its active child when
    // that child's subtree reports itself focusable, which fails whenever a
    // page's only widget is an empty list (e.g. no recurring nets yet). See
    // ql::AppKeyHandler for the full explanation. SafeAppEventDispatcher
    // wraps AppKeyHandler the same way ftxui::CatchEvent would, but also
    // guards against a Database exception (e.g. a write that times out
    // because another connection -- a second instance of the app, or this
    // one's own background ULS import thread -- is mid-transaction) taking
    // down the whole app.
    ftxui::Component ui = ftxui::Make<ql::SafeAppEventDispatcher>(tab, &state);

    screen.Loop(ui);
    return 0;
}
