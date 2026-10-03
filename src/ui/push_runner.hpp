#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <ftxui/component/screen_interactive.hpp>

#include "../upstream_push.hpp"
#include "app_state.hpp"

namespace ql
{

    // Runs one push at a time (PushSessionFile, which waits on scp and ssh
    // for up to a few minutes) on a thread of its own, so the screen keeps
    // working meanwhile, then posts FinishPush to the UI thread, which
    // redraws once. The console session owns one (AppState::push_runner),
    // made after the screen; destroying it stops a push that's still
    // running and waits for its thread.
    class PushRunner
    {
    public:
        PushRunner(ftxui::ScreenInteractive* screen, AppState* state);
        ~PushRunner();

        PushRunner(const PushRunner&) = delete;
        PushRunner& operator=(const PushRunner&) = delete;

        // Starts pushing. The caller makes sure no push is running
        // (AppState::push_running).
        // `net`: the file is a .qlnet (import-net), not a .qlsession.
        void Start(const Upstream& upstream, const std::string& local_path, const std::string& remote_name,
                   const std::string& confirm_net, const std::string& session_net, bool net = false);

        // Has the upstream delete the upload it kept when it asked about a
        // look-alike net, now declined. Done on a thread of its own and
        // without a word to the person: if it doesn't work, the upstream's
        // sweep of old uploads gets it.
        void Discard(const Upstream& upstream, const std::string& remote_name);

    private:
        void Run(Upstream upstream, std::string local_path, std::string remote_name, std::string confirm_net,
                 std::string session_net, bool net);

        ftxui::ScreenInteractive* screen_;
        AppState* state_;
        std::atomic<bool> cancel_{false};
        std::thread thread_;
        std::thread discard_thread_;
    };

}  // namespace ql
