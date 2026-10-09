// Access levels, net grants and restricted-nets mode: what a full user, a
// Net Admin and an Admin may do on a net (src/access.hpp), and that the
// actions behind the keys enforce it.

#include <cstdint>
#include <string>
#include <vector>

#include "../src/access.hpp"
#include "../src/db/database.hpp"
#include "../src/ui/app_state.hpp"
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

}  // namespace ql
