// Pushing sessions upstream, as the screens do it (app_state.hpp): the
// Upstream Server window, what a finished push leaves behind, and History's
// F3. Nothing here runs ssh: pushes are finished by hand with FinishPush.

#include <cstdint>
#include <string>

#include "../src/db/database.hpp"
#include "../src/settings.hpp"
#include "../src/ui/app_state.hpp"
#include "../src/ui/push_runner.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // A console session's AppState on a temporary database, with a
    // PushRunner that's never started.
    class PushFixture
    {
    public:
        PushFixture() : db_(dir_.File("quicklogger.db")), runner_(nullptr, &state)
        {
            state.db = &db_;
            state.db_path = dir_.File("quicklogger.db");
            state.settings_path = dir_.File("settings.txt");
            state.settings.callsign = "W4KWK";
            state.settings.location = "37415";
            state.is_console_session = true;
            state.push_runner = &runner_;
        }

        Database* db()
        {
            return &db_;
        }
        const TempDir& dir() const
        {
            return dir_;
        }

        void SetUpstream()
        {
            state.settings.upstream_host = "upstream.example.org";
            state.settings.upstream_user = "W4KWK";
        }

        // A net "TAG Skywarn" with one session, closed unless `open`;
        // History showing it.
        std::int64_t AddSession(bool open = false)
        {
            std::int64_t net_id = AddTestNet(&db_, "TAG Skywarn");
            std::int64_t instance_id = AddTestInstance(&db_, net_id, "2026-09-14", 1789428600, "W4KWK");
            if (!open)
            {
                db_.CloseNetInstance(instance_id, 1789430400);
            }
            RefreshNets(&state);
            state.selected_net_index = 0;
            state.page = kPageNetHistory;
            RefreshNetHistory(&state);
            return instance_id;
        }

        AppState state;

    private:
        TempDir dir_;
        Database db_;
        PushRunner runner_;
    };

    QL_TEST(TheUpstreamWindowSavesTheUpstream)
    {
        PushFixture f;
        OpenUpstreamWindow(&f.state);
        REQUIRE(f.state.show_upstream_window);
        CHECK_EQ(f.state.upstream_port_text, std::string("22"));

        f.state.upstream_host_text = " upstream.example.org ";
        f.state.upstream_user_text = "W4KWK";
        f.state.upstream_port_text = "2222";
        SaveUpstreamWindow(&f.state);
        CHECK(!f.state.show_upstream_window);
        CHECK_EQ(f.state.settings.upstream_host, std::string("upstream.example.org"));
        CHECK_EQ(f.state.settings_form.upstream_host, std::string("upstream.example.org"));
        CHECK_EQ(f.state.settings.upstream_port, 2222);
        AppSettings saved = LoadSettings(f.state.settings_path);
        CHECK_EQ(saved.upstream_host, std::string("upstream.example.org"));
        CHECK_EQ(saved.upstream_user, std::string("W4KWK"));
        CHECK_EQ(saved.upstream_port, 2222);
        CHECK(CanPushUpstream(&f.state));

        // A blank host is no upstream.
        OpenUpstreamWindow(&f.state);
        f.state.upstream_host_text = "";
        SaveUpstreamWindow(&f.state);
        CHECK(f.state.settings.upstream_host.empty());
        CHECK(!CanPushUpstream(&f.state));
    }

    QL_TEST(TheUpstreamWindowRefusesWhatSshWouldMisread)
    {
        PushFixture f;
        OpenUpstreamWindow(&f.state);
        f.state.upstream_host_text = "-oProxyCommand=sh";
        f.state.upstream_user_text = "W4KWK";
        SaveUpstreamWindow(&f.state);
        CHECK(f.state.show_upstream_window);
        CHECK(!f.state.form_error.empty());
        CHECK(f.state.settings.upstream_host.empty());

        f.state.upstream_host_text = "upstream.example.org";
        f.state.upstream_user_text = "";
        SaveUpstreamWindow(&f.state);
        CHECK(f.state.show_upstream_window);

        f.state.upstream_user_text = "W4KWK";
        f.state.upstream_port_text = "70000";
        SaveUpstreamWindow(&f.state);
        CHECK(f.state.show_upstream_window);
        CHECK_EQ(f.state.form_error, std::string("The port must be 1 to 65535."));
    }

    QL_TEST(OnlyTheConsoleWithAnUpstreamCanPush)
    {
        PushFixture f;
        CHECK(!CanPushUpstream(&f.state));
        f.SetUpstream();
        CHECK(CanPushUpstream(&f.state));
        f.state.is_console_session = false;
        CHECK(!CanPushUpstream(&f.state));
        f.state.is_console_session = true;
        f.state.push_runner = nullptr;
        CHECK(!CanPushUpstream(&f.state));
    }

    QL_TEST(APushThatWorkedMarksTheSessionPushed)
    {
        PushFixture f;
        f.SetUpstream();
        std::int64_t instance_id = f.AddSession();
        CHECK_EQ(f.state.history_instance_cells[0].back(), std::string("closed"));

        std::string file = f.dir().File("push.qlsession");
        WriteTextFile(file, "x");
        f.state.push_running = true;
        f.state.push_instance_id = instance_id;
        f.state.push_local_path = file;
        PushResult result;
        result.kind = PushResultKind::kPushed;
        result.message = "Pushed to TAG Skywarn on upstream.example.org.";
        FinishPush(&f.state, result);

        CHECK(!f.state.push_running);
        CHECK(!FileExists(file));
        CHECK(f.db()->GetNetInstanceById(instance_id)->pushed_at > 0);
        CHECK_EQ(f.state.status_message, result.message);
        CHECK_EQ(f.state.history_instance_cells[0].back(), std::string("pushed"));

        // Not again from History.
        PushSelectedHistorySession(&f.state);
        CHECK_EQ(f.state.form_error, std::string("That session has been pushed already."));
        CHECK(!f.state.push_running);
    }

    QL_TEST(APushThatFailedSaysWhyAndLeavesTheSessionClosed)
    {
        PushFixture f;
        f.SetUpstream();
        std::int64_t instance_id = f.AddSession();
        std::string file = f.dir().File("push.qlsession");
        WriteTextFile(file, "x");
        f.state.push_running = true;
        f.state.push_instance_id = instance_id;
        f.state.push_local_path = file;
        PushResult result;
        result.message = "Couldn't reach upstream.example.org.";
        FinishPush(&f.state, result);

        CHECK(!f.state.push_running);
        CHECK(!FileExists(file));
        CHECK_EQ(f.state.form_error, result.message);
        std::optional<NetInstance> session = f.db()->GetNetInstanceById(instance_id);
        CHECK(session->status == NetInstanceStatus::kClosed);
        CHECK_EQ(session->pushed_at, std::int64_t{0});
    }

    QL_TEST(APushToALookAlikeNetIsAskedAbout)
    {
        PushFixture f;
        f.SetUpstream();
        std::int64_t instance_id = f.AddSession();
        f.state.push_running = true;
        f.state.push_instance_id = instance_id;
        f.state.push_session_net = "TAG Skywarn";
        PushResult result;
        result.kind = PushResultKind::kNeedsConfirmation;
        result.upstream_net = "Sky Warn";
        FinishPush(&f.state, result);

        CHECK(!f.state.push_running);
        REQUIRE(f.state.show_confirm_prompt);
        CHECK(f.state.confirm_prompt == ConfirmPrompt::kPushToNet);
        CHECK_EQ(f.state.push_upstream_net, std::string("Sky Warn"));
        CHECK(f.state.confirm_prompt_lines[0].find("upstream.example.org has no net named TAG Skywarn") == 0);

        DeclinePushToNet(&f.state);
        CHECK(!f.state.show_confirm_prompt);
        CHECK_EQ(f.state.status_message, std::string("Not pushed. Push it from History (F3) later."));
        CHECK_EQ(f.db()->GetNetInstanceById(instance_id)->pushed_at, std::int64_t{0});
    }

    QL_TEST(OnlyAClosedSessionIsPushedFromHistory)
    {
        PushFixture f;
        f.SetUpstream();
        f.AddSession(/*open=*/true);
        PushSelectedHistorySession(&f.state);
        CHECK_EQ(f.state.form_error, std::string("Only a closed session can be pushed."));
        CHECK(!f.state.push_running);

        // A second push waits for the first.
        f.state.push_running = true;
        f.state.form_error.clear();
        CHECK(!StartPush(&f.state, f.state.history_instances[0].id, ""));
        CHECK_EQ(f.state.form_error, std::string("A push is already running."));
    }

    QL_TEST(ExportsCarryNoPushTime)
    {
        PushFixture f;
        std::int64_t instance_id = f.AddSession();
        f.db()->SetNetInstancePushedAt(instance_id, 1800000000);
        CHECK_EQ(f.db()->GetNetInstanceById(instance_id)->pushed_at, std::int64_t{1800000000});
        std::string file = f.dir().File("Sky.qlsession");
        std::string error;
        REQUIRE(WriteNetSliceFile(file, GatherSessionSlice(f.db(), instance_id), &error));
        std::optional<NetSlice> slice = ReadSessionSliceFile(file, &error);
        REQUIRE(slice.has_value());
        CHECK_EQ(slice->instances[0].pushed_at, std::int64_t{0});
    }

}  // namespace ql
