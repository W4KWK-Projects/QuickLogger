#include "push_runner.hpp"

#include <utility>

namespace ql
{

    // Posted to the UI thread when a push has ended.
    class PushFinishedTask
    {
    public:
        PushFinishedTask(AppState* state, PushResult result) : state_(state), result_(std::move(result)) {}

        void operator()() const
        {
            FinishPush(state_, result_);
            if (state_->screen != nullptr)
            {
                state_->screen->PostEvent(ftxui::Event::Custom);
            }
        }

    private:
        AppState* state_;
        PushResult result_;
    };

    PushRunner::PushRunner(ftxui::ScreenInteractive* screen, AppState* state) : screen_(screen), state_(state) {}

    PushRunner::~PushRunner()
    {
        cancel_ = true;
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    void PushRunner::Start(const Upstream& upstream, const std::string& local_path, const std::string& remote_name,
                           const std::string& confirm_net, const std::string& session_net)
    {
        // The last push's thread has posted its result by now; it's ending.
        if (thread_.joinable())
        {
            thread_.join();
        }
        thread_ = std::thread(&PushRunner::Run, this, upstream, local_path, remote_name, confirm_net, session_net);
    }

    void PushRunner::Run(Upstream upstream, std::string local_path, std::string remote_name, std::string confirm_net,
                         std::string session_net)
    {
        PushResult result = PushSessionFile(upstream, local_path, remote_name, confirm_net, session_net, &cancel_);
        if (!cancel_)
        {
            screen_->Post(PushFinishedTask(state_, std::move(result)));
        }
    }

}  // namespace ql
