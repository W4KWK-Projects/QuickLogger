// Access levels, net grants and restricted-nets mode: what a full user, a
// Net Admin and an Admin may do on a net (src/access.hpp), and that the
// actions behind the keys enforce it.

#include <cstdint>
#include <string>
#include <vector>

#include <ftxui/screen/screen.hpp>

#include "../src/access.hpp"
#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/net_slice.hpp"
#include "../src/ui/app_state.hpp"
#include "../src/ui/chrome.hpp"
#include "../src/ui/handlers.hpp"
#include "../src/ui/pages.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // An AppState on a temporary database with three SSH users and two nets:
    // "Tuesday Net" and "Friday Net". N4FUL is a full user, N4NET a Net Admin
    // and N4ADM an Admin; restricted-nets mode starts off.
    class AccessFixture
    {
    public:
        AccessFixture() : db_(dir_.File("quicklogger.db"))
        {
            state.db = &db_;
            state.db_path = dir_.File("quicklogger.db");
            state.settings.callsign = "W4KWK";
            state.settings.location = "37415";
            AddUser("N4FUL", kAccessUser);
            AddUser("N4NET", kAccessNetAdmin);
            AddUser("N4ADM", kAccessAdmin);
            tuesday = AddTestNet(&db_, "Tuesday Net");
            friday = AddTestNet(&db_, "Friday Net");
            RefreshNets(&state);
        }

        Database* db()
        {
            return &db_;
        }

        void AddUser(const std::string& username, int level)
        {
            User user;
            user.username = username;
            user.public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI" + username;
            user.amateur_callsign = username;
            user.access_level = level;
            db_.CreateUser(user);
        }

        // Logs in as `username` over SSH.
        void LogInAs(const std::string& username)
        {
            state.is_console_session = false;
            state.ssh_username = username;
            state.view_only_user = db_.IsUserViewOnly(username);
            RefreshNets(&state);
        }

        void Restrict()
        {
            db_.SetServerOption(kOptionRestrictedNets, true);
            RefreshAccess(&state);
        }

        Net NetById(std::int64_t id)
        {
            return *db_.GetNetById(id);
        }

        AppState state;
        std::int64_t tuesday = 0;
        std::int64_t friday = 0;

    private:
        TempDir dir_;
        Database db_;
    };

    QL_TEST(AccessPowerOnANetFollowsTheLevelAndTheGrants)
    {
        Net recurring;
        recurring.id = 7;
        Net ad_hoc;
        ad_hoc.id = 8;
        ad_hoc.is_ad_hoc = true;

        UserAccess full;
        full.restricted = true;
        // Not given: watch. Ad hoc nets are open to any full user.
        CHECK(full.PowerOn(recurring) == NetPower::kWatch);
        CHECK(full.PowerOn(ad_hoc) == NetPower::kManage);
        full.grants = {7};
        CHECK(full.PowerOn(recurring) == NetPower::kLog);

        UserAccess net_admin;
        net_admin.restricted = true;
        net_admin.level = kAccessNetAdmin;
        CHECK(net_admin.PowerOn(recurring) == NetPower::kWatch);
        net_admin.grants = {7};
        CHECK(net_admin.PowerOn(recurring) == NetPower::kManage);

        UserAccess admin;
        admin.restricted = true;
        admin.level = kAccessAdmin;
        CHECK(admin.PowerOn(recurring) == NetPower::kManage);

        // Off: every full user does everything, as before.
        UserAccess open;
        CHECK(open.PowerOn(recurring) == NetPower::kManage);

        // View-only never goes past watching.
        UserAccess viewer;
        viewer.view_only = true;
        CHECK(viewer.PowerOn(recurring) == NetPower::kWatch);
        CHECK(viewer.PowerOn(ad_hoc) == NetPower::kWatch);

        // The console is an Admin.
        UserAccess console;
        console.console = true;
        console.restricted = true;
        CHECK(console.PowerOn(recurring) == NetPower::kManage);
    }

    QL_TEST(OnlyNetAdminsAndAdminsCreateNetsWhenRestricted)
    {
        UserAccess full;
        CHECK(full.CanCreateNets());
        full.restricted = true;
        CHECK(!full.CanCreateNets());
        full.level = kAccessNetAdmin;
        CHECK(full.CanCreateNets());
        full.view_only = true;
        CHECK(!full.CanCreateNets());
    }

    QL_TEST(WhoMayGiveANetToWhom)
    {
        Net net;
        net.id = 7;
        UserAccess admin;
        admin.level = kAccessAdmin;
        CHECK(admin.CanGrant(net, kAccessUser));
        CHECK(admin.CanGrant(net, kAccessNetAdmin));
        CHECK(!admin.CanGrant(net, kAccessAdmin));

        // A Net Admin gives full users the nets they have themself.
        UserAccess net_admin;
        net_admin.level = kAccessNetAdmin;
        CHECK(!net_admin.CanGrant(net, kAccessUser));
        net_admin.grants = {7};
        CHECK(net_admin.CanGrant(net, kAccessUser));
        CHECK(!net_admin.CanGrant(net, kAccessNetAdmin));

        UserAccess full;
        full.grants = {7};
        CHECK(!full.CanGrant(net, kAccessUser));

        // Ad hoc nets are nobody's to hand out.
        net.is_ad_hoc = true;
        CHECK(!admin.CanGrant(net, kAccessUser));
    }

    QL_TEST(NetGrantsFollowTheirNetAndTheirUser)
    {
        AccessFixture f;
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        f.db()->GrantNet("n4ful", f.tuesday, "console");
        f.db()->GrantNet("N4FUL", f.friday, "console");
        CHECK_EQ(f.db()->GetNetGrants("N4FUL").size(), std::size_t{2});
        CHECK_EQ(f.db()->GetNetGrantees(f.tuesday).size(), std::size_t{1});

        f.db()->RevokeNet("n4ful", f.friday);
        CHECK_EQ(f.db()->GetNetGrants("N4FUL").size(), std::size_t{1});

        // Renamed with the user; gone with the user and with the net.
        CHECK(f.db()->RenameUser("N4FUL", "N4NEW"));
        CHECK(f.db()->GetNetGrants("N4FUL").empty());
        CHECK_EQ(f.db()->GetNetGrants("N4NEW").size(), std::size_t{1});
        f.db()->DeleteNetCompletely(f.tuesday);
        CHECK(f.db()->GetNetGrants("N4NEW").empty());
        f.db()->GrantNet("N4NEW", f.friday, "console");
        f.db()->DeleteUser("N4NEW");
        CHECK(f.db()->GetNetGrantees(f.friday).empty());
    }

    QL_TEST(ANetListUserOnlyLogsTheirNetsWhenRestricted)
    {
        AccessFixture f;
        f.LogInAs("N4FUL");
        f.state.selected_net_index = 0;

        // Off: any net starts as before.
        for (std::size_t i = 0; i < f.state.nets.size(); ++i)
        {
            CHECK(PowerOnNet(&f.state, f.state.nets[i]) == NetPower::kManage);
        }

        f.Restrict();
        RefreshNets(&f.state);
        CHECK_EQ(f.state.nets.size(), std::size_t{2});
        for (std::size_t i = 0; i < f.state.nets.size(); ++i)
        {
            CHECK(PowerOnNet(&f.state, f.state.nets[i]) == NetPower::kWatch);
        }
        CHECK(!CanCreateNets(&f.state));

        // Enter on a net that isn't theirs only watches: nothing open, so
        // it says so and stays on the list.
        f.state.selected_net_index = 0;
        StartSelectedNet(&f.state);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK(f.state.form_error.find("isn't one of your nets") != std::string::npos);

        // Given the net, they log it.
        f.db()->GrantNet("N4FUL", f.state.nets[0].id, "console");
        f.state.form_error.clear();
        StartSelectedNet(&f.state);
        CHECK_EQ(f.state.page, kPageSelectRole);
    }

    QL_TEST(AFullUserWithANetEditsItsStationsButNotItsDetails)
    {
        AccessFixture f;
        f.Restrict();
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        f.LogInAs("N4FUL");

        OpenEditNetForm(&f.state, f.NetById(f.tuesday));
        CHECK_EQ(f.state.edit_net_id, f.tuesday);
        CHECK(!f.state.edit_net_editable);

        // Not the details, not deleting the net.
        f.state.edit_net_name = "Renamed";
        CHECK(!SaveEditNetForm(&f.state));
        CHECK_EQ(f.state.form_error, std::string("Only Net Admins can edit nets."));
        CHECK_EQ(f.NetById(f.tuesday).name, std::string("Tuesday Net"));
        RequestDeleteNet(&f.state);
        CHECK(!f.state.show_delete_net_confirm_modal);
        f.state.show_delete_net_confirm_modal = true;
        ConfirmDeleteNet(&f.state);
        CHECK(f.db()->GetNetById(f.tuesday).has_value());

        // A saved station can be added, then edited, but not removed.
        f.state.form_error.clear();
        OpenNewSavedStationForm(&f.state);
        CHECK(f.state.show_saved_station_modal);
        f.state.saved_station.callsign = "K4ABC";
        CHECK(SaveNetStationForm(&f.state));
        CHECK_EQ(f.db()->GetSavedNetEntries(f.tuesday).size(), std::size_t{1});
        RefreshEditNetSavedStations(&f.state);
        f.state.selected_saved_station_index = 0;
        RemoveSelectedSavedNetStation(&f.state);
        CHECK_EQ(f.state.form_error, std::string("Only Net Admins can remove saved stations."));
        CHECK_EQ(f.db()->GetSavedNetEntries(f.tuesday).size(), std::size_t{1});

        // A net that isn't theirs can't be opened at all.
        f.state.form_error.clear();
        f.state.edit_net_id = 0;
        OpenEditNetForm(&f.state, f.NetById(f.friday));
        CHECK_EQ(f.state.edit_net_id, std::int64_t{0});
        CHECK(f.state.form_error.find("isn't one of your nets") != std::string::npos);
        CloseSavedStationForm(&f.state);
        f.state.edit_net_id = f.friday;
        OpenNewSavedStationForm(&f.state);
        CHECK(!f.state.show_saved_station_modal);
    }

    QL_TEST(ANetAdminLooksAfterTheirNetsOnly)
    {
        AccessFixture f;
        f.Restrict();
        f.db()->GrantNet("N4NET", f.tuesday, "console");
        f.LogInAs("N4NET");

        OpenEditNetForm(&f.state, f.NetById(f.tuesday));
        CHECK(f.state.edit_net_editable);
        f.state.edit_net_name = "Tuesday Evening Net";
        CHECK(SaveEditNetForm(&f.state));
        CHECK_EQ(f.NetById(f.tuesday).name, std::string("Tuesday Evening Net"));

        // Friday isn't theirs.
        f.state.form_error.clear();
        f.state.edit_net_id = 0;
        OpenEditNetForm(&f.state, f.NetById(f.friday));
        CHECK_EQ(f.state.edit_net_id, std::int64_t{0});

        // They can add a net, and are given it.
        CHECK(CanCreateNets(&f.state));
        GiveNewNetToCreator(&f.state, f.friday);
        CHECK(f.state.access.IsGranted(f.friday));

        // And delete what they look after.
        OpenEditNetForm(&f.state, f.NetById(f.friday));
        f.state.show_delete_net_confirm_modal = true;
        ConfirmDeleteNet(&f.state);
        CHECK(!f.db()->GetNetById(f.friday).has_value());
    }

    QL_TEST(OnlyANetAdminDeletesHistoryWhenRestricted)
    {
        AccessFixture f;
        std::int64_t instance = AddTestInstance(f.db(), f.tuesday, "2026-09-14", 1789428600, "K4ABC");
        AddTestCheckIn(f.db(), instance, "K4ABC", 1);
        AddTestCheckIn(f.db(), instance, "W4XYZ", 2);
        f.db()->CloseNetInstance(instance, 1789428600 + 1800);
        f.Restrict();
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        f.db()->GrantNet("N4NET", f.tuesday, "console");
        f.LogInAs("N4FUL");
        f.state.selected_net_index = 0;
        for (std::size_t i = 0; i < f.state.nets.size(); ++i)
        {
            if (f.state.nets[i].id == f.tuesday)
            {
                f.state.selected_net_index = static_cast<int>(i);
            }
        }
        f.state.history_ad_hoc = false;
        RefreshNetHistory(&f.state);
        CHECK_EQ(f.state.history_instances.size(), std::size_t{1});
        CHECK(HistoryPower(&f.state) == NetPower::kLog);

        DeleteSelectedHistoryCheckIn(&f.state);
        CHECK_EQ(f.state.form_error, std::string("Only Net Admins can delete check-ins."));
        DeleteSelectedNetInstance(&f.state);
        CHECK_EQ(f.db()->GetNetInstancesForNet(f.tuesday).size(), std::size_t{1});
        f.state.form_error.clear();
        StartRowPick(&f.state, RowPickAction::kDeleteNetInstance);
        CHECK(f.state.row_pick_action == RowPickAction::kNone);
        CHECK(!f.state.form_error.empty());

        // Their Net Admin may.
        f.LogInAs("N4NET");
        f.state.selected_net_index = 0;
        for (std::size_t i = 0; i < f.state.nets.size(); ++i)
        {
            if (f.state.nets[i].id == f.tuesday)
            {
                f.state.selected_net_index = static_cast<int>(i);
            }
        }
        RefreshNetHistory(&f.state);
        CHECK(HistoryPower(&f.state) == NetPower::kManage);
        DeleteSelectedNetInstance(&f.state);
        CHECK(f.db()->GetNetInstancesForNet(f.tuesday).empty());
    }

    QL_TEST(AdHocNetsStayOpenToEveryFullUser)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4FUL");
        f.state.new_net_name = "Field Day";
        f.state.new_net_location = "37415";
        StartAdHocNet(&f.state);
        CHECK_EQ(f.state.page, kPageSelectRole);
        CHECK(f.state.start_net.is_ad_hoc);
        CHECK(RefuseNetPower(&f.state, f.state.start_net.id, NetPower::kManage, "do anything") == false);
    }

    QL_TEST(AViewOnlyUserNeverGetsMoreFromAGrant)
    {
        AccessFixture f;
        f.AddUser("N4VIEW", kAccessUser);
        f.db()->SetUserViewOnly("N4VIEW", true);
        f.Restrict();
        f.db()->GrantNet("N4VIEW", f.tuesday, "console");
        f.LogInAs("N4VIEW");
        CHECK(RefuseNetPower(&f.state, f.tuesday, NetPower::kLog, "log check-ins"));
        CHECK_EQ(f.state.form_error, std::string("View-only users can't log check-ins."));
    }

    QL_TEST(AdminsAndTheConsoleAreNeverRestricted)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4ADM");
        CHECK(!RefuseNetPower(&f.state, f.tuesday, NetPower::kManage, "edit nets"));
        CHECK(CanCreateNets(&f.state));
        f.state.is_console_session = true;
        f.state.ssh_username.clear();
        CHECK(!RefuseNetPower(&f.state, f.friday, NetPower::kManage, "edit nets"));
    }

    QL_TEST(NetAccessGivesAndTakesNetsWithinTheirLimits)
    {
        AccessFixture f;
        f.AddUser("N4TWO", kAccessUser);
        f.db()->GrantNet("N4NET", f.tuesday, "console");

        // A Net Admin hands out only their own nets, to full users.
        f.LogInAs("N4NET");
        f.state.page = kPageSettings;
        CHECK(CanOpenNetAccess(&f.state));
        OpenNetAccess(&f.state, kPageSettings);
        CHECK_EQ(f.state.page, kPageNetAccess);
        CHECK_EQ(f.state.net_access_nets.size(), std::size_t{1});
        CHECK_EQ(f.state.net_access_nets[0].id, f.tuesday);
        ChooseNetAccessNet(&f.state);
        CHECK_EQ(f.state.net_access_stage, 1);
        // N4FUL and N4TWO; not themself, and not the Admin.
        CHECK_EQ(f.state.net_access_users.size(), std::size_t{2});
        f.state.selected_net_access_user = 0;
        ToggleNetAccessUser(&f.state);
        CHECK_EQ(f.db()->GetNetGrants(f.state.net_access_users[0].username).size(), std::size_t{1});
        CHECK(f.state.net_access_user_labels[0].find("[x]") == 0);
        CHECK_EQ(f.db()->RecentKeyEvents(1)[0].action, std::string("net given"));
        ToggleNetAccessUser(&f.state);
        CHECK(f.db()->GetNetGrants(f.state.net_access_users[0].username).empty());
        BackOutOfNetAccess(&f.state);
        CHECK_EQ(f.state.net_access_stage, 0);
        BackOutOfNetAccess(&f.state);
        CHECK_EQ(f.state.page, kPageSettings);

        // Taken away from them mid-session: the next change is refused.
        f.state.selected_net_access_user = 0;
        OpenNetAccess(&f.state, kPageSettings);
        ChooseNetAccessNet(&f.state);
        f.db()->RevokeNet("N4NET", f.tuesday);
        ToggleNetAccessUser(&f.state);
        CHECK(f.db()->GetNetGrants("N4FUL").empty());
        CHECK(f.db()->GetNetGrants("N4TWO").empty());
        CHECK(!f.state.form_error.empty());

        // An Admin hands out any net, to full users and Net Admins.
        f.LogInAs("N4ADM");
        OpenNetAccess(&f.state, kPageManageUsers);
        CHECK_EQ(f.state.net_access_nets.size(), std::size_t{2});
        ChooseNetAccessNet(&f.state);
        CHECK_EQ(f.state.net_access_users.size(), std::size_t{3});
    }

    QL_TEST(OnlyAnAdminTurnsRestrictedOnAndItIsLogged)
    {
        AccessFixture f;
        f.LogInAs("N4NET");
        ToggleRestrictedNets(&f.state);
        CHECK(!f.db()->ServerOptionOn(kOptionRestrictedNets));
        CHECK_EQ(f.state.form_error, std::string("Only admins can manage users."));

        f.LogInAs("N4ADM");
        f.state.form_error.clear();
        ToggleRestrictedNets(&f.state);
        CHECK(f.db()->ServerOptionOn(kOptionRestrictedNets));
        CHECK_EQ(f.db()->RecentKeyEvents(1)[0].action, std::string("restricted on"));
        ToggleRestrictedNets(&f.state);
        CHECK(!f.db()->ServerOptionOn(kOptionRestrictedNets));
    }

    // ---- What the screens show, at 80x24 and 120x40 ---------------------------

    // The rows of `page` drawn as a `width` x `height` terminal does it.
    static std::vector<std::string> DrawRows(AppState* state, const ftxui::Component& page, int width, int height)
    {
        SetFrameTerminalSize(ftxui::Dimensions{width, height});
        UpdateListWidths(state, width);
        state->screen_height = height;
        ftxui::Screen drawn(width, height);
        ftxui::Render(drawn, page->Render());
        std::vector<std::string> rows;
        for (int y = 0; y < height; ++y)
        {
            std::string row;
            for (int x = 0; x < width; ++x)
            {
                row += drawn.PixelAt(x, y).character;
            }
            rows.push_back(row);
        }
        return rows;
    }

    static bool ShowsText(const std::vector<std::string>& rows, const std::string& text)
    {
        for (const std::string& row : rows)
        {
            if (row.find(text) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // Both sizes the screens are designed for.
    struct ScreenSize
    {
        int width;
        int height;
    };
    static const ScreenSize kScreenSizes[] = {{80, 24}, {120, 40}};

    static void SelectNet(AppState* state, std::int64_t net_id)
    {
        for (std::size_t i = 0; i < state->nets.size(); ++i)
        {
            if (state->nets[i].id == net_id)
            {
                state->selected_net_index = static_cast<int>(i);
            }
        }
    }

    QL_TEST(TheNetListKeysFollowWhatTheUserMayDoWithTheHighlightedNet)
    {
        for (const ScreenSize& size : kScreenSizes)
        {
            AccessFixture f;
            f.Restrict();
            f.db()->GrantNet("N4FUL", f.tuesday, "console");
            f.db()->GrantNet("N4NET", f.tuesday, "console");
            f.LogInAs("N4FUL");
            ftxui::Component page = BuildNetListPage(&f.state);

            // A net that isn't theirs: watch only, no New, Edit.
            SelectNet(&f.state, f.friday);
            std::vector<std::string> rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "Not one of your nets"));
            CHECK(ShowsText(rows, "F3/Enter  View"));
            CHECK(!ShowsText(rows, "F2  New"));
            CHECK(!ShowsText(rows, "F7"));
            CHECK(ShowsText(rows, "F8  Export"));
            CHECK(ShowsText(rows, "F10  Quit"));

            // A net they were given: log it, and its saved stations.
            SelectNet(&f.state, f.tuesday);
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "F3/Enter  Log Net"));
            CHECK(ShowsText(rows, "F7  Stations"));
            CHECK(!ShowsText(rows, "F2  New"));
            CHECK(ShowsText(rows, "F9  Import"));
            CHECK(!ShowsText(rows, "(Net Admin)"));

            // A Net Admin of it: everything, New included.
            f.LogInAs("N4NET");
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "F2  New"));
            CHECK(ShowsText(rows, "F7  Edit"));
            CHECK(ShowsText(rows, "(Net Admin)"));
            SelectNet(&f.state, f.friday);
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "F3/Enter  View"));
            CHECK(!ShowsText(rows, "F7"));

            // Restricted off: every full user has the whole bar.
            f.db()->SetServerOption(kOptionRestrictedNets, false);
            f.LogInAs("N4FUL");
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "F2  New"));
            CHECK(ShowsText(rows, "F7  Edit"));
            CHECK(ShowsText(rows, "F9  Import"));
            CHECK(!ShowsText(rows, "Not one of your nets"));
        }
    }

    QL_TEST(AUserWithNoNetsHasNoNewOrImportKey)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4FUL");
        ftxui::Component page = BuildNetListPage(&f.state);
        std::vector<std::string> rows = DrawRows(&f.state, page, 80, 24);
        CHECK(!ShowsText(rows, "F2  New"));
        CHECK(!ShowsText(rows, "F9  Import"));
        CHECK(ShowsText(rows, "F5  AdHoc"));
        CHECK(ShowsText(rows, "F6  History"));
    }

    QL_TEST(EditNetShowsOnlyTheStationsToAUserWhoMayLogTheNet)
    {
        for (const ScreenSize& size : kScreenSizes)
        {
            AccessFixture f;
            f.Restrict();
            f.db()->GrantNet("N4FUL", f.tuesday, "console");
            f.db()->GrantNet("N4NET", f.tuesday, "console");
            ftxui::Component page = BuildEditNetPage(&f.state);

            f.LogInAs("N4FUL");
            OpenEditNetForm(&f.state, f.NetById(f.tuesday));
            std::vector<std::string> rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "Tuesday Net"));
            CHECK(ShowsText(rows, "Only a Net Admin changes these"));
            CHECK(ShowsText(rows, "F6  Add Station"));
            CHECK(ShowsText(rows, "F3  Edit Station"));
            CHECK(!ShowsText(rows, "F2  Save"));
            CHECK(!ShowsText(rows, "F4  Remove"));
            CHECK(!ShowsText(rows, "F8  Del Net"));

            // The name can't be typed into.
            page->OnEvent(ftxui::Event::Character('X'));
            CHECK_EQ(f.state.edit_net_name, std::string("Tuesday Net"));

            f.LogInAs("N4NET");
            OpenEditNetForm(&f.state, f.NetById(f.tuesday));
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(!ShowsText(rows, "Only a Net Admin changes these"));
            CHECK(ShowsText(rows, "F2  Save"));
            CHECK(ShowsText(rows, "F8  Del Net"));
        }
    }

    // A closed two-check-in session of Tuesday Net.
    static void AddTuesdaySession(AccessFixture* f)
    {
        std::int64_t instance = AddTestInstance(f->db(), f->tuesday, "2026-09-14", 1789428600, "K4ABC");
        AddTestCheckIn(f->db(), instance, "K4ABC", 1);
        AddTestCheckIn(f->db(), instance, "W4XYZ", 2);
        f->db()->CloseNetInstance(instance, 1789428600 + 1800);
    }

    QL_TEST(HistoryKeysFollowWhatTheUserMayDoWithTheNet)
    {
        for (const ScreenSize& size : kScreenSizes)
        {
            AccessFixture f;
            AddTuesdaySession(&f);
            f.Restrict();
            f.db()->GrantNet("N4FUL", f.tuesday, "console");
            f.db()->GrantNet("N4NET", f.tuesday, "console");
            ftxui::Component page = BuildNetHistoryPage(&f.state);

            // Not theirs: only look and export.
            f.LogInAs("N4FUL");
            SelectNet(&f.state, f.friday);
            RefreshNetHistory(&f.state);
            std::vector<std::string> rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "F7  Export"));
            CHECK(!ShowsText(rows, "F6  Import"));
            CHECK(!ShowsText(rows, "Del Check-In"));
            AppKeyHandler keys(&f.state);
            f.state.page = kPageNetHistory;
            CHECK(keys(ftxui::Event::F6));
            CHECK(f.state.page == kPageNetHistory);
            CHECK(!f.state.show_zmodem_confirm_modal);
            CHECK(f.state.row_pick_action == RowPickAction::kNone);
            CHECK(keys(ftxui::Event::F5));
            CHECK(keys(ftxui::Event::F4));
            CHECK(f.state.row_pick_action == RowPickAction::kNone);

            // Theirs: import, but no deleting.
            SelectNet(&f.state, f.tuesday);
            RefreshNetHistory(&f.state);
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "F6  Import"));
            CHECK(!ShowsText(rows, "Del Check-In"));
            CHECK(keys(ftxui::Event::F5));
            CHECK(f.state.row_pick_action == RowPickAction::kNone);

            // Their Net Admin: everything.
            f.LogInAs("N4NET");
            SelectNet(&f.state, f.tuesday);
            RefreshNetHistory(&f.state);
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "F6  Import"));
            CHECK(ShowsText(rows, "F5  Del Check-In"));
            f.state.page = kPageNetHistory;
            CHECK(keys(ftxui::Event::F5));
            CHECK(f.state.row_pick_action == RowPickAction::kDeleteHistoryCheckIn);
        }
    }

    QL_TEST(ManageUsersShowsTheNewKeysAndLevelsAt80Columns)
    {
        for (const ScreenSize& size : kScreenSizes)
        {
            AccessFixture f;
            f.state.page = kPageSettings;
            ShowManageUsersPageHandler show(&f.state);
            show();
            CHECK_EQ(f.state.page, kPageManageUsers);
            ftxui::Component page = BuildManageUsersPage(&f.state);
            std::vector<std::string> rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "Admin"));
            CHECK(ShowsText(rows, "Net Admin"));
            CHECK(ShowsText(rows, "Full access"));
            CHECK(ShowsText(rows, "F6  Key Log"));
            CHECK(ShowsText(rows, "F7  Net Access"));
            CHECK(ShowsText(rows, "F8  Restricted: Off"));
            CHECK(ShowsText(rows, "Esc  Back"));

            AppKeyHandler keys(&f.state);
            CHECK(keys(ftxui::Event::F8));
            CHECK(f.db()->ServerOptionOn(kOptionRestrictedNets));
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "F8  Restricted: On"));
            CHECK(keys(ftxui::Event::F8));
            CHECK(!f.db()->ServerOptionOn(kOptionRestrictedNets));
        }
    }

    QL_TEST(TheEditUserWindowFitsItsFourAccessChoicesAt80Columns)
    {
        AccessFixture f;
        ShowManageUsersPageHandler show(&f.state);
        show();
        ftxui::Component page = BuildManageUsersPage(&f.state);
        for (std::size_t i = 0; i < f.state.manage_user_names.size(); ++i)
        {
            OpenUserKeys(&f.state, static_cast<int>(i));
            std::vector<std::string> rows = DrawRows(&f.state, page, 80, 24);
            CHECK(ShowsText(rows, "Full access"));
            CHECK(ShowsText(rows, "View-only"));
            CHECK(ShowsText(rows, "Net Admin"));
            CHECK(ShowsText(rows, "Admin"));
            CHECK(ShowsText(rows, "F2  Save"));
            CHECK(ShowsText(rows, "Esc  Cancel"));
            CloseUserKeys(&f.state);
        }
    }

    QL_TEST(NetAccessWorksFromTheKeyboardAndFitsBothSizes)
    {
        for (const ScreenSize& size : kScreenSizes)
        {
            AccessFixture f;
            f.state.page = kPageManageUsers;
            ftxui::Component page = BuildNetAccessPage(&f.state);
            AppKeyHandler keys(&f.state);
            OpenNetAccess(&f.state, kPageManageUsers);
            CHECK_EQ(f.state.page, kPageNetAccess);
            std::vector<std::string> rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "Choose a net"));
            CHECK(ShowsText(rows, "Friday Net"));
            CHECK(ShowsText(rows, "Tuesday Net"));
            CHECK(ShowsText(rows, "Restricted is off"));
            CHECK(ShowsText(rows, "F2/Enter  Choose"));

            // Down, then Enter, picks the second net (alphabetical: Tuesday).
            CHECK(!page->OnEvent(ftxui::Event::ArrowDown) || true);
            CHECK_EQ(f.state.selected_net_access_net, 1);
            page->OnEvent(ftxui::Event::Return);
            CHECK_EQ(f.state.net_access_stage, 1);
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "Who has Tuesday Net"));
            CHECK(ShowsText(rows, "[ ] N4FUL"));
            CHECK(ShowsText(rows, "F2/Enter  Give/Take"));

            // In the users list, Down moves the highlight and Enter (not
            // the hidden net list) gives the net.
            page->OnEvent(ftxui::Event::ArrowDown);
            CHECK_EQ(f.state.selected_net_access_user, 1);
            page->OnEvent(ftxui::Event::Return);
            CHECK_EQ(f.db()->GetNetGrants(f.state.net_access_users[1].username).size(), std::size_t{1});
            rows = DrawRows(&f.state, page, size.width, size.height);
            CHECK(ShowsText(rows, "[x] " + f.state.net_access_users[1].username));
            // F2 does the same; Esc goes back a step, then out.
            CHECK(keys(ftxui::Event::F2));
            CHECK(f.db()->GetNetGrants(f.state.net_access_users[1].username).empty());
            CHECK(keys(ftxui::Event::Escape));
            CHECK_EQ(f.state.net_access_stage, 0);
            CHECK(keys(ftxui::Event::Escape));
            CHECK_EQ(f.state.page, kPageManageUsers);
        }
    }

    QL_TEST(SettingsOffersNetAccessToANetAdminAndManageUsersToAnAdmin)
    {
        AccessFixture f;
        f.db()->GrantNet("N4NET", f.tuesday, "console");
        ftxui::Component page = BuildSettingsPage(&f.state);
        AppKeyHandler keys(&f.state);

        f.LogInAs("N4FUL");
        f.state.page = kPageSettings;
        std::vector<std::string> rows = DrawRows(&f.state, page, 80, 24);
        CHECK(!ShowsText(rows, "Net Access"));
        CHECK(!ShowsText(rows, "Manage Users"));
        keys(ftxui::Event::F5);
        CHECK_EQ(f.state.page, kPageSettings);

        f.LogInAs("N4NET");
        f.state.page = kPageSettings;
        rows = DrawRows(&f.state, page, 80, 24);
        CHECK(ShowsText(rows, "F5  Net Access"));
        CHECK(!ShowsText(rows, "Manage Users"));
        CHECK(keys(ftxui::Event::F5));
        CHECK_EQ(f.state.page, kPageNetAccess);
        CHECK_EQ(f.state.net_access_nets.size(), std::size_t{1});
        // Esc goes back to Settings.
        keys(ftxui::Event::Escape);
        CHECK_EQ(f.state.page, kPageSettings);

        f.LogInAs("N4ADM");
        f.state.is_admin_user = true;
        f.state.page = kPageSettings;
        rows = DrawRows(&f.state, page, 80, 24);
        CHECK(ShowsText(rows, "F5  Manage Users"));
        CHECK(!ShowsText(rows, "Net Access"));
    }

    QL_TEST(ANetAdminWithNoNetsIsToldSo)
    {
        AccessFixture f;
        f.LogInAs("N4NET");
        OpenNetAccess(&f.state, kPageSettings);
        CHECK(f.state.page != kPageNetAccess);
        CHECK_EQ(f.state.form_error, std::string("You haven't been given any nets to hand out."));
        f.LogInAs("N4FUL");
        f.state.form_error.clear();
        OpenNetAccess(&f.state, kPageSettings);
        CHECK_EQ(f.state.form_error, std::string("Only Net Admins and Admins hand out nets."));
    }

    // ---- The keys and flows behind them ------------------------------------------

    QL_TEST(NewAndImportKeysAreRefusedWhenRestrictedAndNothingGiven)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4FUL");
        f.state.page = kPageNetList;
        AppKeyHandler keys(&f.state);
        keys(ftxui::Event::F2);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK_EQ(f.state.form_error, std::string("Only Net Admins can add a new net."));
        f.state.form_error.clear();
        keys(ftxui::Event::F9);
        CHECK_EQ(f.state.page, kPageNetList);
        CHECK_EQ(f.state.form_error, std::string("You haven't been given any nets to import into."));

        // Something given: Import opens; New still doesn't.
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        f.state.form_error.clear();
        keys(ftxui::Event::F9);
        CHECK_EQ(f.state.page, kPageImportNet);
        f.state.page = kPageNetList;
        keys(ftxui::Event::F2);
        CHECK_EQ(f.state.page, kPageNetList);

        // The submit handler refuses too, whatever page led there.
        f.state.new_net_name = "Sneaky Net";
        CreateNetSubmitHandler create(&f.state);
        create();
        CHECK_EQ(f.db()->GetAllNets().size(), std::size_t{2});
    }

    QL_TEST(ANetAdminAddsANetWhenRestrictedAndIsGivenIt)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4NET");
        f.state.page = kPageNetList;
        AppKeyHandler keys(&f.state);
        keys(ftxui::Event::F2);
        CHECK_EQ(f.state.page, kPageCreateNet);
        ResetCreateNetForm(&f.state);
        f.state.new_net_name = "Saturday Net";
        CreateNetSubmitHandler create(&f.state);
        create();
        std::vector<Net> nets = f.db()->GetAllNets();
        REQUIRE(nets.size() == 3);
        std::int64_t added = 0;
        for (const Net& net : nets)
        {
            added = net.name == "Saturday Net" ? net.id : added;
        }
        CHECK(added != 0);
        CHECK_EQ(f.db()->GetNetGrants("N4NET").size(), std::size_t{1});
        CHECK_EQ(f.db()->GetNetGrants("N4NET")[0], added);
    }

    QL_TEST(StartingASessionOnANetThatIsNotYoursIsRefusedEvenFromTheCallsignPage)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4FUL");
        f.state.start_net = f.NetById(f.friday);
        f.state.operator_callsign = "N4FUL";
        f.state.selected_role_index = kRoleNetControl;
        OperatorCallsignSubmitHandler submit(&f.state);
        submit();
        CHECK(f.db()->GetNetInstancesForNet(f.friday).empty());
        CHECK(f.state.form_error.find("isn't one of your nets") != std::string::npos);
        CHECK(f.state.page != kPageActiveNet);
    }

    QL_TEST(LoggingAndClosingAreRefusedOnceTheNetIsTakenAway)
    {
        AccessFixture f;
        f.Restrict();
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        f.LogInAs("N4FUL");
        f.state.start_net = f.NetById(f.tuesday);
        f.state.operator_callsign = "N4FUL";
        f.state.selected_role_index = kRoleNetControl;
        OperatorCallsignSubmitHandler submit(&f.state);
        submit();
        CHECK(f.state.page == kPageActiveNet);
        std::int64_t instance = f.state.active_instance.id;
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(instance).size(), std::size_t{1});

        // Taken away mid-session: no more check-ins, no close.
        f.db()->RevokeNet("N4FUL", f.tuesday);
        f.state.modal_station = Station();
        f.state.modal_station.callsign = "W4XYZ";
        CHECK(!LogStationCheckIn(&f.state));
        CHECK_EQ(f.db()->GetCheckInsForNetInstance(instance).size(), std::size_t{1});
        CHECK(f.state.form_error.find("isn't one of your nets") != std::string::npos);
        f.state.selected_check_in_index = 0;
        RequestCloseActiveNet(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK(!CloseActiveNet(&f.state));
        CHECK(f.db()->GetNetInstanceById(instance)->status == NetInstanceStatus::kOpen);
    }

    // Writes the net `net_name` (with a session) as a .qlnet in the user's
    // imports folder, as another QuickLogger exported it.
    static void WriteNetFile(AccessFixture* f, const std::string& file, const std::string& net_name)
    {
        TempDir source_dir;
        Database source(source_dir.File("source.db"));
        std::int64_t net_id = AddTestNet(&source, net_name);
        std::int64_t instance = AddTestInstance(&source, net_id, "2026-09-14", 1789428600, "K4ABC");
        AddTestCheckIn(&source, instance, "K4ABC", 1);
        source.CloseNetInstance(instance, 1789428600 + 1800);
        std::string dir = SessionImportsDir(f->state.db_path, f->state.ssh_username);
        EnsureDirectory(dir);
        std::string error;
        REQUIRE(WriteNetSliceFile(dir + "/" + file, GatherNetSlice(&source, net_id), &error));
        f->state.page = kPageImportNet;
        f->state.import_session = false;
        RefreshImportNetFiles(&f->state);
        for (std::size_t i = 0; i < f->state.import_net_files.size(); ++i)
        {
            if (f->state.import_net_files[i] == file)
            {
                f->state.selected_import_file_index = static_cast<int>(i);
            }
        }
    }

    QL_TEST(ImportingANetWhenRestrictedFollowsWhatTheUserMayDo)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4FUL");

        // A net that is new: only a Net Admin adds it.
        WriteNetFile(&f, "Sat.qlnet", "Saturday Net");
        ImportSelectedNetSlice(&f.state);
        CHECK_EQ(f.state.form_error, std::string("Only Net Admins can add a new net."));
        CHECK_EQ(f.db()->GetAllNets().size(), std::size_t{2});

        // The same name as a net that isn't theirs: refused, not merged.
        WriteNetFile(&f, "Tue.qlnet", "Tuesday Net");
        f.state.form_error.clear();
        ImportSelectedNetSlice(&f.state);
        CHECK(f.state.form_error.find("isn't one of your nets") != std::string::npos);
        CHECK(!f.state.show_merge_modal);
        CHECK(f.db()->GetNetInstancesForNet(f.tuesday).empty());

        // A look-alike that isn't theirs isn't offered either; nothing new
        // can be added.
        WriteNetFile(&f, "Tue2.qlnet", "Tuesday Nite Net");
        f.state.form_error.clear();
        ImportSelectedNetSlice(&f.state);
        CHECK(!f.state.show_merge_modal);
        CHECK_EQ(f.state.form_error, std::string("Only Net Admins can add a new net."));

        // Given Tuesday, it merges into it, and the merge applies only to a net they have.
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        RefreshNets(&f.state);
        WriteNetFile(&f, "Tue.qlnet", "Tuesday Net");
        f.state.form_error.clear();
        ImportSelectedNetSlice(&f.state);
        CHECK(f.state.show_merge_modal);
        ChooseMergeTarget(&f.state);
        CHECK_EQ(f.state.merge_plan.target_net_id, f.tuesday);
        ConfirmNetMerge(&f.state);
        CHECK_EQ(f.db()->GetNetInstancesForNet(f.tuesday).size(), std::size_t{1});

        // Taken away between the summary and the confirm: refused.
        f.db()->DeleteNetCompletely(f.friday);
        std::int64_t monday = AddTestNet(f.db(), "Monday Net");
        f.db()->GrantNet("N4FUL", monday, "console");
        RefreshNets(&f.state);
        WriteNetFile(&f, "Mon.qlnet", "Monday Net");
        ImportSelectedNetSlice(&f.state);
        REQUIRE(f.state.show_merge_modal);
        ChooseMergeTarget(&f.state);
        f.db()->RevokeNet("N4FUL", monday);
        ConfirmNetMerge(&f.state);
        CHECK(f.db()->GetNetInstancesForNet(monday).empty());
        CHECK(f.state.form_error.find("isn't one of your nets") != std::string::npos);
    }

    QL_TEST(ANetAdminWhoImportsANewNetIsGivenIt)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4NET");
        WriteNetFile(&f, "Sat.qlnet", "Saturday Net");
        ImportSelectedNetSlice(&f.state);
        CHECK_EQ(f.db()->GetAllNets().size(), std::size_t{3});
        CHECK_EQ(f.db()->GetNetGrants("N4NET").size(), std::size_t{1});
    }

    QL_TEST(ImportingASessionNeedsTheNetWhenRestricted)
    {
        AccessFixture f;
        f.Restrict();
        f.LogInAs("N4FUL");
        // From the net list, into History's import.
        f.state.history_ad_hoc = false;
        SelectNet(&f.state, f.tuesday);
        OpenSessionImport(&f.state);
        CHECK(f.state.page != kPageImportNet);
        CHECK(f.state.form_error.find("isn't one of your nets") != std::string::npos);

        // Given the net, it opens and imports; taken away before the
        // import itself, it is refused.
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        TempDir source_dir;
        Database source(source_dir.File("source.db"));
        std::int64_t source_net = AddTestNet(&source, "Tuesday Net");
        std::int64_t instance = AddTestInstance(&source, source_net, "2026-09-14", 1789428600, "K4ABC");
        AddTestCheckIn(&source, instance, "K4ABC", 1);
        source.CloseNetInstance(instance, 1789428600 + 1800);
        std::string dir = SessionImportsDir(f.state.db_path, f.state.ssh_username);
        EnsureDirectory(dir);
        std::string error;
        REQUIRE(WriteNetSliceFile(dir + "/Tue.qlsession", GatherSessionSlice(&source, instance), &error));
        f.state.form_error.clear();
        OpenSessionImport(&f.state);
        CHECK_EQ(f.state.page, kPageImportNet);
        f.db()->RevokeNet("N4FUL", f.tuesday);
        ImportSelectedSession(&f.state);
        CHECK(f.db()->GetNetInstancesForNet(f.tuesday).empty());
        CHECK(f.state.form_error.find("isn't one of your nets") != std::string::npos);
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        f.state.form_error.clear();
        ImportSelectedSession(&f.state);
        CHECK_EQ(f.db()->GetNetInstancesForNet(f.tuesday).size(), std::size_t{1});
    }

    QL_TEST(SessionNotesAreReadOnlyOnANetThatIsNotYours)
    {
        AccessFixture f;
        AddTuesdaySession(&f);
        f.Restrict();
        f.LogInAs("N4FUL");
        SelectNet(&f.state, f.tuesday);
        f.state.history_ad_hoc = false;
        RefreshNetHistory(&f.state);
        OpenHistorySessionNotes(&f.state);
        CHECK(f.state.show_session_notes_modal);
        CHECK(f.state.session_notes_read_only);
        CloseSessionNotes(&f.state);

        // Given the net, they can write; taken away before saving, refused.
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        RefreshNets(&f.state);
        OpenHistorySessionNotes(&f.state);
        CHECK(!f.state.session_notes_read_only);
        f.state.session_notes_text = "Storm net.";
        f.db()->RevokeNet("N4FUL", f.tuesday);
        SaveSessionNotes(&f.state);
        CHECK_EQ(f.db()->GetNetInstancesForNet(f.tuesday)[0].notes, std::string());
        CHECK(!f.state.show_session_notes_modal);
    }

    // Whether F1 Help (already opened) has a row for `key`.
    static bool HelpShows(const AppState& state, const std::string& key)
    {
        for (const std::string& row : state.info_rows)
        {
            if (row.find(key) == 0)
            {
                return true;
            }
        }
        return false;
    }

    QL_TEST(HelpListsOnlyTheKeysTheUserHas)
    {
        AccessFixture f;
        AddTuesdaySession(&f);
        f.Restrict();
        f.db()->GrantNet("N4FUL", f.tuesday, "console");
        f.db()->GrantNet("N4NET", f.tuesday, "console");
        f.LogInAs("N4FUL");
        f.state.page = kPageNetList;
        OpenHelp(&f.state);
        CHECK(!HelpShows(f.state, "F2"));
        CHECK(HelpShows(f.state, "F9"));
        CloseInfoWindow(&f.state);

        f.state.page = kPageNetHistory;
        SelectNet(&f.state, f.tuesday);
        OpenHelp(&f.state);
        CHECK(HelpShows(f.state, "F6"));
        CHECK(!HelpShows(f.state, "F5"));
        CloseInfoWindow(&f.state);

        f.LogInAs("N4NET");
        f.state.page = kPageNetHistory;
        SelectNet(&f.state, f.tuesday);
        OpenHelp(&f.state);
        CHECK(HelpShows(f.state, "F5"));
        CloseInfoWindow(&f.state);

        // Edit Net, for a user who may only log the net.
        f.LogInAs("N4FUL");
        OpenEditNetForm(&f.state, f.NetById(f.tuesday));
        f.state.page = kPageEditNet;
        OpenHelp(&f.state);
        CHECK(HelpShows(f.state, "F6"));
        CHECK(!HelpShows(f.state, "F8"));
        CHECK(!HelpShows(f.state, "F2"));
    }

    QL_TEST(HelpCoversNetAccessAndTheNewManageUsersKeys)
    {
        AccessFixture f;
        f.db()->GrantNet("N4NET", f.tuesday, "console");
        f.LogInAs("N4NET");
        f.state.page = kPageNetAccess;
        OpenHelp(&f.state);
        CHECK(HelpShows(f.state, "F2/Enter"));
        CloseInfoWindow(&f.state);
        f.state.page = kPageSettings;
        OpenHelp(&f.state);
        CHECK(HelpShows(f.state, "F5"));
        CloseInfoWindow(&f.state);
        f.state.is_console_session = true;
        f.state.page = kPageManageUsers;
        OpenHelp(&f.state);
        CHECK(HelpShows(f.state, "F7"));
        CHECK(HelpShows(f.state, "F8"));
    }

    QL_TEST(AUserWithTwoKeysIsListedOnceInNetAccess)
    {
        AccessFixture f;
        User second;
        second.username = "N4FUL";
        second.public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIsecondkey";
        f.db()->CreateUser(second);
        f.state.is_console_session = true;
        OpenNetAccess(&f.state, kPageManageUsers);
        f.state.selected_net_access_user = 99;
        ChooseNetAccessNet(&f.state);
        CHECK_EQ(f.state.net_access_users.size(), std::size_t{2});
        CHECK_EQ(f.state.selected_net_access_user, 0);
    }

    QL_TEST(AViewOnlyUserCantHandOutNetsAndGetsOnlyWatchPower)
    {
        AccessFixture f;
        f.AddUser("N4VIEW", kAccessNetAdmin);
        f.db()->SetUserViewOnly("N4VIEW", true);
        f.db()->GrantNet("N4VIEW", f.tuesday, "console");
        f.Restrict();
        f.LogInAs("N4VIEW");
        CHECK(PowerOnNet(&f.state, f.state.nets[0]) == NetPower::kWatch);
        CHECK(!CanCreateNets(&f.state));
        CHECK(!CanOpenNetAccess(&f.state));
        f.state.net_access_nets.push_back(f.NetById(f.tuesday));
        ToggleNetAccessUser(&f.state);
        CHECK_EQ(f.state.form_error, std::string("View-only users can't hand out nets."));
        // A session with no user to look up is a plain full user.
        f.state.view_only_user = false;
        f.state.ssh_username.clear();
        RefreshAccess(&f.state);
        CHECK(f.state.access.restricted);
        CHECK_EQ(f.state.access.level, kAccessUser);
    }

}  // namespace ql
