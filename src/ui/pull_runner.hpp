#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <ftxui/component/screen_interactive.hpp>

#include "../upstream_pull.hpp"
#include "app_state.hpp"

namespace ql
{

    // Runs one pull step at a time (PullNetList, PullNetFiles, which wait on
    // ssh and scp for up to a few minutes) on a thread of its own, so the
    // screen keeps working meanwhile, then posts the result to the UI thread
    // (FinishPullList, FinishPullFiles), which redraws once. Like PushRunner,
    // the console session owns one (AppState::pull_runner), made after the
    // screen; destroying it stops a step that's still running.
    //
    // Each step carries the AppState::pull_generation it was started under, so
    // an answer to a pull the person has since cancelled is ignored.
    class PullRunner
    {
    public:
        PullRunner(ftxui::ScreenInteractive* screen, AppState* state);
        ~PullRunner();

        PullRunner(const PullRunner&) = delete;
        PullRunner& operator=(const PullRunner&) = delete;

        // Asks the upstream for its nets.
        void ListNets(const Upstream& upstream, int generation);

        // Has the upstream export net `net_name` (sessions, or the net as a
        // whole) and fetches the files into `local_dir`.
        void Fetch(const Upstream& upstream, const std::string& net_name, bool sessions, const std::string& local_dir,
                   int generation);

        // Stops the step that's running, if any, and waits for it to end;
        // it posts nothing.
        void Cancel();

    private:
        void RunListNets(Upstream upstream, int generation);
        void RunFetch(Upstream upstream, std::string net_name, bool sessions, std::string local_dir, int generation);

        // Waits for the last step's thread, then clears the stop flag.
        void Prepare();

        ftxui::ScreenInteractive* screen_;
        AppState* state_;
        std::atomic<bool> cancel_{false};
        std::thread thread_;
    };

}  // namespace ql
