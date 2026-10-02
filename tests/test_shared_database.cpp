// Two people using one database at once, as two SSH sessions (or an SSH
// session and the console) do: each with its own connection and its own
// screens, one acting while the other is looking.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/ui/app_state.hpp"
#include "../src/ui/handlers.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // One person: their own connection to the shared database, and their
    // own AppState.
    class Operator
    {
    public:
        Operator(const std::string& db_path, const std::string& callsign) : db_(db_path)
        {
            state.db = &db_;
            state.db_path = db_path;
            state.settings.callsign = callsign;
            state.settings.location = "37415";
            state.operator_callsign = callsign;
        }

        Database* db()
        {
            return &db_;
        }

        // Starts a session of `net_id` as Net Control, logging themselves.
        void StartSession(std::int64_t net_id, const std::string& net_name)
        {
            NetInstance instance;
            instance.net_id = net_id;
            instance.instance_date = "2026-10-02";
            instance.started_at = 1000;
            instance.net_control_callsign = state.operator_callsign;
            instance.operator_role = kRoleNetControl;
            instance.id = db_.CreateNetInstance(instance);
            state.active_instance = instance;
            state.active_net_name = net_name;
            state.selected_role_index = kRoleNetControl;
            state.page = kPageActiveNet;
            LogOperatorCheckIn(&state);
            ClearModalFields(&state);
        }

        // Joins the session `instance_id` someone else started, as Logger.
        void JoinSession(std::int64_t instance_id, const std::string& net_name)
        {
            std::optional<NetInstance> instance = db_.GetNetInstanceById(instance_id);
            REQUIRE(instance.has_value());
            state.active_instance = *instance;
            state.active_net_name = net_name;
            state.selected_role_index = kRoleLogger;
            state.page = kPageActiveNet;
            RefreshActiveCheckIns(&state);
        }

        // Logs `callsign` through the New Station modal.
        bool Log(const std::string& callsign)
        {
            ClearModalFields(&state);
            state.modal_station.callsign = callsign;
            return LogStationCheckIn(&state);
        }

        // Opens History on the net `net_id`, highlighting the session
        // `instance_id`.
        void ShowHistory(std::int64_t net_id, std::int64_t instance_id)
        {
            RefreshNets(&state);
            for (std::size_t i = 0; i < state.nets.size(); ++i)
            {
                if (state.nets[i].id == net_id)
                {
                    state.selected_net_index = static_cast<int>(i);
                }
            }
            ViewNetHistoryHandler view_history(&state);
            view_history();
            Highlight(instance_id);
        }

        // Moves History's highlight to the session `instance_id`, as the
        // arrow keys do.
        void Highlight(std::int64_t instance_id)
        {
            for (std::size_t i = 0; i < state.history_instances.size(); ++i)
            {
                if (state.history_instances[i].id == instance_id)
                {
                    state.selected_history_index = static_cast<int>(i);
                }
            }
            HistoryInstanceChangedHandler changed(&state);
            changed();
        }

        // The call signs History shows for the highlighted session, in
        // order, with the name shown for each.
        std::string HistoryShown() const
        {
            std::string shown;
            for (std::size_t i = 0; i < state.history_check_ins.size(); ++i)
            {
                shown += state.history_check_ins[i].callsign;
                const std::vector<std::string>& cells = state.history_check_in_cells[i];
                shown += " (" + (cells.size() > 3 ? cells[3] : std::string()) + ") ";
            }
            return shown;
        }

        AppState state;

    private:
        Database db_;
    };

    static std::string ActiveCallsigns(const AppState& state)
    {
        std::string shown;
        for (const CheckIn& check_in : state.active_check_ins)
        {
            shown += "#" + std::to_string(check_in.sequence_number) + " " + check_in.callsign + " ";
        }
        return shown;
    }

    // A net with a closed session of three check-ins (W4KWK, K4AAA, K4BBB)
    // and an older one of one.
    static void AddClosedSessions(Database* db, std::int64_t* net_id, std::int64_t* newer, std::int64_t* older)
    {
        *net_id = AddTestNet(db, "Skywarn");
        *older = AddTestInstance(db, *net_id, "2026-09-22", 500, "W4KWK");
        AddTestCheckIn(db, *older, "W4KWK", 1);
        db->CloseNetInstance(*older, 900);
        *newer = AddTestInstance(db, *net_id, "2026-09-29", 1000, "W4KWK");
        AddTestCheckIn(db, *newer, "W4KWK", 1);
        AddTestCheckIn(db, *newer, "K4AAA", 2);
        AddTestCheckIn(db, *newer, "K4BBB", 3);
        db->CloseNetInstance(*newer, 1500);
    }

    QL_TEST(TwoLoggersInOneSessionShareItsNumbers)
    {
        TempDir dir;
        Operator net_control(dir.File("quicklogger.db"), "W4KWK");
        Operator logger(dir.File("quicklogger.db"), "K4LOG");
        std::int64_t net_id = AddTestNet(net_control.db(), "Skywarn");
        net_control.StartSession(net_id, "Skywarn");
        logger.JoinSession(net_control.state.active_instance.id, "Skywarn");

        // Each logs without having seen the other's latest: numbers still
        // follow on, never repeat.
        CHECK(net_control.Log("K4AAA"));
        CHECK(logger.Log("K4BBB"));
        CHECK(net_control.Log("K4CCC"));
        CHECK(logger.Log("K4DDD"));

        // Someone the other already logged is refused, even unseen.
        CHECK(!logger.Log("K4CCC"));
        CHECK_EQ(logger.state.form_error, std::string("K4CCC is already in this session's log, as #4."));
        CHECK(!net_control.Log("K4DDD/M"));

        // Each sees everyone once the other's check-ins are picked up (as
        // the session page's poll does).
        RefreshActiveCheckInsFromOthers(&net_control.state, net_control.state.active_instance.id);
        RefreshActiveCheckInsFromOthers(&logger.state, logger.state.active_instance.id);
        std::string everyone = "#1 W4KWK #2 K4AAA #3 K4BBB #4 K4CCC #5 K4DDD ";
        CHECK_EQ(ActiveCallsigns(net_control.state), everyone);
        CHECK_EQ(ActiveCallsigns(logger.state), everyone);
    }

    QL_TEST(HistoryShowsAnotherSessionsDeleteWhenComingBack)
    {
        TempDir dir;
        Operator viewer(dir.File("quicklogger.db"), "W4KWK");
        Operator editor(dir.File("quicklogger.db"), "K4EDT");
        std::int64_t net_id = 0;
        std::int64_t newer = 0;
        std::int64_t older = 0;
        AddClosedSessions(viewer.db(), &net_id, &newer, &older);

        viewer.ShowHistory(net_id, newer);
        CHECK_EQ(viewer.state.history_check_ins.size(), std::size_t{3});
        viewer.Highlight(older);

        // Meanwhile someone else deletes K4AAA from the newer session.
        editor.ShowHistory(net_id, newer);
        editor.state.selected_history_check_in_index = 1;
        DeleteSelectedHistoryCheckIn(&editor.state);
        REQUIRE(editor.state.form_error.empty());

        // Coming back to it shows it as it is now.
        viewer.Highlight(newer);
        REQUIRE(viewer.state.history_check_ins.size() == 2);
        CHECK_EQ(viewer.state.history_check_ins[1].callsign, std::string("K4BBB"));
    }

    QL_TEST(HistoryShowsAnotherSessionsStationEditWhenComingBack)
    {
        TempDir dir;
        Operator viewer(dir.File("quicklogger.db"), "W4KWK");
        Operator editor(dir.File("quicklogger.db"), "K4EDT");
        std::int64_t net_id = 0;
        std::int64_t newer = 0;
        std::int64_t older = 0;
        AddClosedSessions(viewer.db(), &net_id, &newer, &older);

        viewer.ShowHistory(net_id, newer);
        viewer.Highlight(older);
        Station renamed = MakeStation("K4AAA", "Adams, Alice");
        editor.db()->UpdateStationFields(renamed, 2000);

        viewer.Highlight(newer);
        CHECK(viewer.HistoryShown().find("K4AAA (Adams, Alice)") != std::string::npos);
    }

    QL_TEST(HistoryKeepsAClosedSessionWhileNoOneElseChangesAnything)
    {
        TempDir dir;
        Operator viewer(dir.File("quicklogger.db"), "W4KWK");
        std::int64_t net_id = 0;
        std::int64_t newer = 0;
        std::int64_t older = 0;
        AddClosedSessions(viewer.db(), &net_id, &newer, &older);
        viewer.ShowHistory(net_id, newer);
        viewer.Highlight(older);

        // A change through the viewer's own connection, behind History's
        // back (its own screens reread what they change): coming back shows
        // what was kept, so it wasn't read again.
        std::vector<CheckIn> check_ins = viewer.db()->GetCheckInsForNetInstance(newer);
        REQUIRE(check_ins.size() == 3);
        viewer.db()->DeleteCheckIn(check_ins[2].id);
        viewer.Highlight(newer);
        CHECK_EQ(viewer.state.history_check_ins.size(), std::size_t{3});
    }

    QL_TEST(DeletingFromAnOutOfDateHistoryDeletesWhatWasShown)
    {
        TempDir dir;
        Operator viewer(dir.File("quicklogger.db"), "W4KWK");
        Operator editor(dir.File("quicklogger.db"), "K4EDT");
        std::int64_t net_id = 0;
        std::int64_t newer = 0;
        std::int64_t older = 0;
        AddClosedSessions(viewer.db(), &net_id, &newer, &older);
        viewer.ShowHistory(net_id, newer);

        // Someone else deletes K4AAA, which the viewer still shows.
        editor.ShowHistory(net_id, newer);
        editor.state.selected_history_check_in_index = 1;
        DeleteSelectedHistoryCheckIn(&editor.state);

        // Deleting K4BBB from the out-of-date list deletes K4BBB, not
        // whatever now sits where it's shown.
        viewer.state.selected_history_check_in_index = 2;
        DeleteSelectedHistoryCheckIn(&viewer.state);
        std::vector<CheckIn> left = viewer.db()->GetCheckInsForNetInstance(newer);
        REQUIRE(left.size() == 1);
        CHECK_EQ(left[0].callsign, std::string("W4KWK"));

        // And deleting the one already gone deletes nothing else.
        viewer.ShowHistory(net_id, newer);
        editor.ShowHistory(net_id, newer);
        editor.state.selected_history_check_in_index = 0;
        std::int64_t gone = editor.state.history_check_ins[0].id;
        editor.db()->DeleteCheckIn(gone);
        viewer.state.selected_history_check_in_index = 0;
        DeleteSelectedHistoryCheckIn(&viewer.state);
        CHECK(viewer.db()->GetCheckInsForNetInstance(newer).empty());
        CHECK_EQ(viewer.db()->GetCheckInsForNetInstance(older).size(), std::size_t{1});
    }

}  // namespace ql
