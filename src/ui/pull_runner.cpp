#include "pull_runner.hpp"

#include <utility>

namespace ql
{

    // Posted to the UI thread when the list of nets has arrived.
    class PullListFinishedTask
    {
    public:
        PullListFinishedTask(AppState* state, PullResult result, int generation)
            : state_(state), result_(std::move(result)), generation_(generation)
        {
        }

        void operator()() const
        {
            FinishPullList(state_, result_, generation_);
            if (state_->screen != nullptr)
            {
                state_->screen->PostEvent(ftxui::Event::Custom);
            }
        }

    private:
        AppState* state_;
        PullResult result_;
        int generation_;
    };

    // Posted to the UI thread when the files have been fetched.
    class PullFilesFinishedTask
    {
    public:
        PullFilesFinishedTask(AppState* state, PullResult result, int generation)
            : state_(state), result_(std::move(result)), generation_(generation)
        {
        }

        void operator()() const
        {
            FinishPullFiles(state_, result_, generation_);
            if (state_->screen != nullptr)
            {
                state_->screen->PostEvent(ftxui::Event::Custom);
            }
        }

    private:
        AppState* state_;
        PullResult result_;
        int generation_;
    };

    PullRunner::PullRunner(ftxui::ScreenInteractive* screen, AppState* state) : screen_(screen), state_(state) {}

    PullRunner::~PullRunner()
    {
        Cancel();
    }

    void PullRunner::Cancel()
    {
        cancel_ = true;
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    void PullRunner::Prepare()
    {
        Cancel();
        cancel_ = false;
    }

    void PullRunner::ListNets(const Upstream& upstream, int generation)
    {
        Prepare();
        thread_ = std::thread(&PullRunner::RunListNets, this, upstream, generation);
    }

    void PullRunner::Fetch(const Upstream& upstream, const std::string& net_name, bool sessions,
                           const std::string& local_dir, int generation)
    {
        Prepare();
        thread_ = std::thread(&PullRunner::RunFetch, this, upstream, net_name, sessions, local_dir, generation);
    }

    void PullRunner::RunListNets(Upstream upstream, int generation)
    {
        PullResult result = PullNetList(upstream, &cancel_);
        if (!cancel_)
        {
            screen_->Post(PullListFinishedTask(state_, std::move(result), generation));
        }
    }

    void PullRunner::RunFetch(Upstream upstream, std::string net_name, bool sessions, std::string local_dir,
                              int generation)
    {
        PullResult result = PullNetFiles(upstream, net_name, sessions, local_dir, &cancel_);
        if (!cancel_)
        {
            screen_->Post(PullFilesFinishedTask(state_, std::move(result), generation));
        }
    }

}  // namespace ql
