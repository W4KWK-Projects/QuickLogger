// Pulling from the upstream, as the screens do it (app_state.hpp): the Pull
// window's list of nets, and what the files fetched leave behind. Nothing
// here runs ssh: the steps are finished by hand with FinishPullList and
// FinishPullFiles.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/file_export.hpp"
#include "../src/net_slice.hpp"
#include "../src/settings.hpp"
#include "../src/ui/app_state.hpp"
#include "../src/ui/pull_runner.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // A console session's AppState on a temporary database, with a
    // PullRunner that's never started.
    class PullFixture
    {
    public:
        PullFixture() : db_(dir_.File("quicklogger.db")), runner_(nullptr, &state)
        {
            state.db = &db_;
            state.db_path = dir_.File("quicklogger.db");
            state.settings_path = dir_.File("settings.txt");
            state.settings.callsign = "W4KWK";
            state.settings.location = "37415";
            state.settings.upstream_host = "upstream.example.org";
            state.settings.upstream_user = "W4KWK";
            state.is_console_session = true;
            state.pull_runner = &runner_;
        }

        Database* db()
        {
            return &db_;
        }
        const TempDir& dir() const
        {
            return dir_;
        }

        // The import page for the net called `name`'s History (a session pull).
        std::int64_t OpenSessionImportOf(const std::string& name)
        {
            std::int64_t net_id = AddTestNet(&db_, name);
            RefreshNets(&state);
            state.selected_net_index = 0;
            state.history_ad_hoc = false;
            state.page = kPageNetHistory;
            RefreshNetHistory(&state);
            ::ql::OpenSessionImport(&state);
            return net_id;
        }

        // A .qlsession of net `net_name` for `date`, made in a database of its
        // own, at `path`.
        void WriteSession(const std::string& path, const std::string& net_name, const std::string& date,
                          std::int64_t started_at)
        {
            TempDir source_dir;
            Database source(source_dir.File("source.db"));
            std::int64_t net_id = AddTestNet(&source, net_name);
            std::int64_t instance = AddTestInstance(&source, net_id, date, started_at, "K4ABC");
            AddTestCheckIn(&source, instance, "K4ABC", 1);
            source.CloseNetInstance(instance, started_at + 1800);
            std::string error;
            REQUIRE(WriteNetSliceFile(path, GatherSessionSlice(&source, instance), &error));
        }

        AppState state;

    private:
        TempDir dir_;
        Database db_;
        PullRunner runner_;
    };

    static PullResult NetsOf(const std::vector<UpstreamNet>& nets)
    {
        PullResult result;
        result.kind = PullResultKind::kNets;
        result.nets = nets;
        return result;
    }

    static UpstreamNet MakeNet(const std::string& name, int sessions, const std::string& service = "amateur")
    {
        UpstreamNet net;
        net.name = name;
        net.sessions = sessions;
        net.service = service;
        return net;
    }

    QL_TEST(OnlyTheConsoleWithAnUpstreamCanPull)
    {
        PullFixture f;
        CHECK(CanPullUpstream(&f.state));
        f.state.settings.upstream_host.clear();
        CHECK(!CanPullUpstream(&f.state));
        f.state.settings.upstream_host = "upstream.example.org";
        f.state.is_console_session = false;
        CHECK(!CanPullUpstream(&f.state));
        f.state.is_console_session = true;
        f.state.pull_runner = nullptr;
        CHECK(!CanPullUpstream(&f.state));
    }

    QL_TEST(PullingNeedsAnUpstreamAndAFullUser)
    {
        PullFixture f;
        f.state.settings.upstream_host.clear();
        OpenPullWindow(&f.state);
        CHECK(f.state.pull_stage == PullStage::kNone);
        CHECK_EQ(f.state.form_error, std::string("Set up an upstream server in Settings (F5) first."));

        // A view-only user here can't import anything, pulled or not.
        f.state.settings.upstream_host = "upstream.example.org";
        f.state.view_only_user = true;
        f.state.form_error.clear();
        OpenPullWindow(&f.state);
        CHECK(f.state.pull_stage == PullStage::kNone);
        CHECK(!f.state.form_error.empty());
    }

    QL_TEST(TheListOfNetsIsChosenFrom)
    {
        PullFixture f;
        f.state.pull_stage = PullStage::kListing;
        f.state.show_pull_modal = true;
        f.state.pull_generation = 3;
        f.state.pull_sessions = false;
        FinishPullList(&f.state, NetsOf({MakeNet("Dixie Traders", 1), MakeNet("TAG Skywarn", 12)}), 3);
        CHECK(f.state.pull_stage == PullStage::kChoosing);
        REQUIRE(f.state.pull_net_labels.size() == 2);
        CHECK_EQ(f.state.pull_net_labels[0], std::string("Dixie Traders  (1 session)"));
        CHECK_EQ(f.state.pull_net_labels[1], std::string("TAG Skywarn  (12 sessions)"));
        CHECK_EQ(f.state.selected_pull_net, 0);
        MovePullHighlight(&f.state, 1);
        CHECK_EQ(f.state.selected_pull_net, 1);
        MovePullHighlight(&f.state, 1);
        CHECK_EQ(f.state.selected_pull_net, 1);
        MovePullHighlight(&f.state, -5);
        CHECK_EQ(f.state.selected_pull_net, 0);
    }

    QL_TEST(AnAnswerToAWindowSinceClosedIsIgnored)
    {
        PullFixture f;
        f.state.pull_stage = PullStage::kListing;
        f.state.show_pull_modal = true;
        f.state.pull_generation = 4;
        FinishPullList(&f.state, NetsOf({MakeNet("TAG Skywarn", 1)}), 3);
        CHECK(f.state.pull_stage == PullStage::kListing);
        CHECK(f.state.pull_nets.empty());

        // Esc closes it, and what arrives after is ignored too.
        ClosePullWindow(&f.state);
        CHECK(f.state.pull_stage == PullStage::kNone);
        CHECK(!f.state.show_pull_modal);
        FinishPullList(&f.state, NetsOf({MakeNet("TAG Skywarn", 1)}), f.state.pull_generation);
        CHECK(f.state.pull_stage == PullStage::kNone);
    }

    QL_TEST(AListThatFailedOrIsEmptyClosesTheWindowWithWhy)
    {
        PullFixture f;
        f.state.pull_stage = PullStage::kListing;
        f.state.show_pull_modal = true;
        PullResult failed;
        failed.message = "Couldn't reach upstream.example.org.";
        FinishPullList(&f.state, failed, f.state.pull_generation);
        CHECK(f.state.pull_stage == PullStage::kNone);
        CHECK(!f.state.show_pull_modal);
        CHECK_EQ(f.state.form_error, failed.message);

        f.state.pull_stage = PullStage::kListing;
        f.state.show_pull_modal = true;
        FinishPullList(&f.state, NetsOf({}), f.state.pull_generation);
        CHECK(f.state.pull_stage == PullStage::kNone);
        CHECK_EQ(f.state.form_error, std::string("upstream.example.org has no nets."));
    }

    QL_TEST(ASessionPullHighlightsTheNetOfTheSameNameOnTheSameService)
    {
        PullFixture f;
        f.OpenSessionImportOf("TAG Skywarn");
        f.state.pull_stage = PullStage::kListing;
        f.state.show_pull_modal = true;
        f.state.pull_sessions = true;
        FinishPullList(&f.state,
                       NetsOf({MakeNet("Dixie Traders", 2), MakeNet("GMRS Chat", 5, "gmrs"), MakeNet("Sky Warn", 4),
                               MakeNet("tag  skywarn", 12)}),
                       f.state.pull_generation);
        // Not the GMRS net: its sessions couldn't go into this one.
        REQUIRE(f.state.pull_nets.size() == 3);
        CHECK_EQ(f.state.pull_nets[f.state.selected_pull_net].name, std::string("tag  skywarn"));

        // With no net of its name, the first that looks like it.
        f.state.pull_stage = PullStage::kListing;
        FinishPullList(&f.state, NetsOf({MakeNet("Dixie Traders", 2), MakeNet("Skywarn Weekly Net", 4)}),
                       f.state.pull_generation);
        CHECK_EQ(f.state.pull_nets[f.state.selected_pull_net].name, std::string("Skywarn Weekly Net"));
    }

    QL_TEST(ANetWithoutClosedSessionsIsNotPulledForItsSessions)
    {
        PullFixture f;
        f.OpenSessionImportOf("TAG Skywarn");
        f.state.pull_sessions = true;
        f.state.pull_stage = PullStage::kListing;
        f.state.show_pull_modal = true;
        FinishPullList(&f.state, NetsOf({MakeNet("TAG Skywarn", 0)}), f.state.pull_generation);
        ChoosePullNet(&f.state);
        CHECK(f.state.pull_stage == PullStage::kChoosing);
        CHECK_EQ(f.state.form_error, std::string("TAG Skywarn has no closed sessions to pull."));
    }

    QL_TEST(PulledSessionsAreAddedToTheNetsHistoryExceptTheOnesItHas)
    {
        PullFixture f;
        std::int64_t net_id = f.OpenSessionImportOf("TAG Skywarn");
        // One of the three is here already.
        std::int64_t have = AddTestInstance(f.db(), net_id, "2026-09-07", 1788823800, "W4KWK");
        f.db()->CloseNetInstance(have, 1788825600);

        std::string pull_dir = f.dir().File("imports/.pull");
        REQUIRE(EnsureDirectory(pull_dir));
        PullResult result;
        result.kind = PullResultKind::kFiles;
        result.net = "Skywarn Weekly Net";
        result.not_sent = 1;
        const char* const dates[] = {"2026-09-21", "2026-09-14", "2026-09-07"};
        std::int64_t started[] = {1790033400, 1789428600, 1788823800};
        for (int i = 0; i < 3; ++i)
        {
            std::string path = pull_dir + "/Skywarn_" + dates[i] + ".qlsession";
            f.WriteSession(path, "Skywarn Weekly Net", dates[i], started[i]);
            result.files.push_back(path);
        }

        f.state.pull_sessions = true;
        f.state.pull_dir = pull_dir;
        f.state.pull_stage = PullStage::kFetching;
        f.state.show_pull_modal = true;
        FinishPullFiles(&f.state, result, f.state.pull_generation);

        CHECK(f.state.pull_stage == PullStage::kNone);
        CHECK(!f.state.show_pull_modal);
        CHECK_EQ(f.db()->GetNetInstancesForNet(net_id).size(), std::size_t(3));
        CHECK_EQ(f.state.status_message,
                 std::string("Added 2 sessions of Skywarn Weekly Net from upstream.example.org; 1 here already. "
                             "1 not sent (still open, or over the limit)."));
        CHECK(f.state.page == kPageNetHistory);
        CHECK(!f.state.import_session);
        CHECK(!FileExists(pull_dir + "/Skywarn_2026-09-14.qlsession"));

        // Pulled again: nothing new.
        REQUIRE(EnsureDirectory(pull_dir));
        f.WriteSession(pull_dir + "/Skywarn_2026-09-14.qlsession", "Skywarn Weekly Net", "2026-09-14", 1789428600);
        result.files.clear();
        result.files.push_back(pull_dir + "/Skywarn_2026-09-14.qlsession");
        f.state.import_session = true;
        f.state.pull_dir = pull_dir;
        f.state.pull_stage = PullStage::kFetching;
        FinishPullFiles(&f.state, result, f.state.pull_generation);
        CHECK_EQ(f.db()->GetNetInstancesForNet(net_id).size(), std::size_t(3));
        CHECK_EQ(f.state.status_message, std::string("Nothing new: upstream.example.org's sessions of Skywarn Weekly "
                                                     "Net are here already. 1 not sent (still open, or over the "
                                                     "limit)."));
    }

    QL_TEST(APulledNetIsImportedLikeAReceivedFile)
    {
        PullFixture f;
        // The file as the fetch leaves it: in the imports folder, a net of its own.
        std::string imports = SessionImportsDir(f.state.db_path, f.state.ssh_username);
        REQUIRE(EnsureDirectory(imports));
        TempDir source_dir;
        Database source(source_dir.File("source.db"));
        std::int64_t net_id = AddTestNet(&source, "TAG Skywarn");
        std::int64_t instance = AddTestInstance(&source, net_id, "2026-09-14", 1789428600, "K4ABC");
        AddTestCheckIn(&source, instance, "K4ABC", 1);
        source.CloseNetInstance(instance, 1789430400);
        std::string error;
        REQUIRE(WriteNetSliceFile(imports + "/TAG_Skywarn.qlnet", GatherNetSlice(&source, net_id), &error));

        PullResult result;
        result.kind = PullResultKind::kFiles;
        result.net = "TAG Skywarn";
        result.files.push_back(imports + "/TAG_Skywarn.qlnet");
        f.state.import_session = false;
        f.state.pull_sessions = false;
        f.state.page = kPageImportNet;
        f.state.pull_stage = PullStage::kFetching;
        f.state.show_pull_modal = true;
        FinishPullFiles(&f.state, result, f.state.pull_generation);

        CHECK(f.state.pull_stage == PullStage::kNone);
        REQUIRE(f.db()->GetAllNets().size() == 1);
        CHECK_EQ(f.db()->GetAllNets()[0].name, std::string("TAG Skywarn"));
        CHECK(f.state.page == kPageNetList);
    }

    QL_TEST(AFetchThatFailedClosesTheWindowWithWhy)
    {
        PullFixture f;
        f.state.pull_stage = PullStage::kFetching;
        f.state.show_pull_modal = true;
        PullResult failed;
        failed.message = "upstream.example.org has no net named Sky any more.";
        FinishPullFiles(&f.state, failed, f.state.pull_generation);
        CHECK(f.state.pull_stage == PullStage::kNone);
        CHECK_EQ(f.state.form_error, failed.message);

        // A net with no closed sessions to send: none to import either.
        f.state.pull_stage = PullStage::kFetching;
        PullResult none;
        none.kind = PullResultKind::kFiles;
        none.net = "Sky";
        f.state.pull_sessions = true;
        FinishPullFiles(&f.state, none, f.state.pull_generation);
        CHECK_EQ(f.state.form_error, std::string("upstream.example.org has no closed sessions of Sky to send."));
    }

}  // namespace ql
