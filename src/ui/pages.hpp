#pragma once

#include <ftxui/component/component.hpp>

#include "app_state.hpp"

namespace ql
{

    ftxui::Component BuildNetListPage(AppState* state);
    ftxui::Component BuildCreateNetPage(AppState* state);
    ftxui::Component BuildSelectRolePage(AppState* state);
    ftxui::Component BuildEnterCallsignPage(AppState* state);
    ftxui::Component BuildActiveNetPage(AppState* state);
    ftxui::Component BuildSettingsPage(AppState* state);
    ftxui::Component BuildAdHocNetPage(AppState* state);
    ftxui::Component BuildNetHistoryPage(AppState* state);
    ftxui::Component BuildEditNetPage(AppState* state);
    ftxui::Component BuildImportNetPage(AppState* state);
    ftxui::Component BuildManageUsersPage(AppState* state);

    // The window for AppState::info_window (Help and the seldom-used
    // windows), shown over whichever page is up.
    ftxui::Component BuildInfoWindow(AppState* state);

}  // namespace ql
