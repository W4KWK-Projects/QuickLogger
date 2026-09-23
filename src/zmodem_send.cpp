#include "zmodem_send.hpp"

#include <csignal>
#include <cstdlib>
#include <ctime>

#include <sys/wait.h>
#include <unistd.h>

#include <ftxui/component/screen_interactive.hpp>

namespace ql
{

    // How long to wait for a receiver to respond before giving up. Chosen to
    // comfortably cover a human noticing and accepting a "receive file?"
    // prompt in their terminal client, while still being a bounded wait
    // rather than the indefinite one `sz` defaults to against an
    // unresponsive receiver (confirmed empirically: with nothing on the
    // other end, `sz` keeps retrying its init frame well past a minute).
    static constexpr int kZmodemTimeoutSeconds = 25;
    // Grace period after SIGTERM before escalating to SIGKILL, in case `sz`
    // doesn't react to the first signal immediately.
    static constexpr int kZmodemKillGraceSeconds = 2;

    bool ZmodemSendAvailable()
    {
        return std::system("command -v sz > /dev/null 2>&1") == 0;
    }

    // Waits up to `timeout_seconds` for `pid` to exit on its own, polling
    // rather than blocking so a stuck child doesn't hang this call forever.
    // Sends SIGTERM (then SIGKILL if it's still alive after a short grace
    // period) if the timeout elapses, then reaps it either way. Returns the
    // exit status wait() would have given, or -1 if the process had to be
    // killed.
    static int WaitWithTimeout(pid_t pid, int timeout_seconds)
    {
        std::time_t deadline = std::time(nullptr) + timeout_seconds;
        int status = 0;
        while (std::time(nullptr) < deadline)
        {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid)
            {
                return status;
            }
            usleep(200000);
        }

        kill(pid, SIGTERM);
        std::time_t kill_deadline = std::time(nullptr) + kZmodemKillGraceSeconds;
        while (std::time(nullptr) < kill_deadline)
        {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid)
            {
                return -1;
            }
            usleep(200000);
        }

        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }

    // Runs inside ScreenInteractive::WithRestoredIO's closure: forks and
    // execs `sz path_` with the real terminal's stdin/stdout inherited
    // (never redirected -- a ZMODEM-aware client needs to see the raw
    // protocol bytes in the same stream it's already reading), then waits
    // for it with a bounded timeout. Writes its outcome into `*ok_`/`*error_`
    // rather than returning one, since ftxui::Closure is a bare
    // std::function<void()>.
    class RunSzProcess
    {
    public:
        RunSzProcess(std::string path, bool* ok, std::string* error)
            : path_(std::move(path)), ok_(ok), error_(error)
        {
        }

        void operator()() const
        {
            *ok_ = false;
            error_->clear();

            pid_t pid = fork();
            if (pid < 0)
            {
                *error_ = "Could not start the ZMODEM sender (fork failed).";
                return;
            }
            if (pid == 0)
            {
                execlp("sz", "sz", path_.c_str(), static_cast<char*>(nullptr));
                _exit(127);
            }

            int status = WaitWithTimeout(pid, kZmodemTimeoutSeconds);
            if (status == -1)
            {
                *error_ = "ZMODEM transfer timed out -- no receiver responded.";
                return;
            }
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            {
                *ok_ = true;
                return;
            }
            *error_ = "ZMODEM transfer failed or was cancelled.";
        }

    private:
        std::string path_;
        bool* ok_;
        std::string* error_;
    };

    bool SendFileViaZmodem(ftxui::ScreenInteractive* screen, const std::string& path,
                           std::string* error)
    {
        if (!ZmodemSendAvailable())
        {
            *error = "The 'sz' command (lrzsz) isn't installed; skipped ZMODEM send.";
            return false;
        }

        bool ok = false;
        ftxui::Closure run = screen->WithRestoredIO(RunSzProcess(path, &ok, error));
        run();
        return ok;
    }

}  // namespace ql
