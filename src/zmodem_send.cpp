#include "zmodem_send.hpp"

// ZMODEM support is a thin wrapper around forking and exec'ing the external
// `sz`/`rz` programs (lrzsz), which is POSIX-only (fork/waitpid/kill). On
// Windows there's nothing equivalent to run -- and no remote-terminal use
// case either, since there's no SSH server on Windows to hand files over --
// so the same API simply reports itself unavailable.
#if defined(_WIN32)

namespace ql
{

    bool ZmodemSendAvailable()
    {
        return false;
    }

    bool ZmodemReceiveAvailable()
    {
        return false;
    }

    bool NoZmodemOnThisSystem()
    {
        return true;
    }

    bool SendFilesViaZmodem(ftxui::ScreenInteractive* screen, const std::vector<std::string>& paths, std::string* error)
    {
        (void)screen;
        (void)paths;
        *error = "ZMODEM transfers aren't supported on Windows.";
        return false;
    }

    bool ReceiveFileViaZmodem(ftxui::ScreenInteractive* screen, const std::string& dest_dir, std::string* error)
    {
        (void)screen;
        (void)dest_dir;
        *error = "ZMODEM transfers aren't supported on Windows.";
        return false;
    }

}  // namespace ql

#else  // POSIX

#include <csignal>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <ftxui/component/screen_interactive.hpp>

#include "file_export.hpp"
#include "ui/mouse.hpp"

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
    // How long to leave the terminal alone once sz or rz has finished,
    // before QuickLogger redraws. The terminal program is still wrapping up
    // then: ZOC, for one, prints its transfer summary (and the "OO" that
    // ends a ZMODEM session) only after the transfer, and may ignore what
    // arrives before. Written onto the normal screen during this pause, it
    // disappears when QuickLogger returns to its own; any sooner, and it's
    // left over QuickLogger's screen until the next key.
    static constexpr useconds_t kTerminalSettleMicroseconds = 1000000;

    // Where package managers put `sz` and `rz` (lrzsz): FreeBSD's pkg and
    // Homebrew on Intel, Homebrew on Apple Silicon, MacPorts, then the
    // system's own. Checked after PATH, which may not name them -- a service
    // started at boot on FreeBSD gets only /sbin:/bin:/usr/sbin:/usr/bin.
    static const char* const kFallbackDirectories[] = {
        "/usr/local/bin", "/opt/homebrew/bin", "/opt/local/bin", "/usr/bin", "/bin",
    };

    // The full path of the executable named `program`: the first found in
    // the directories on PATH (the same lookup execvp() does), then in
    // kFallbackDirectories. Empty if there's none. Done directly rather than
    // via `std::system("command -v ...")`, which would start a shell just to
    // ask.
    static std::string FindProgram(const std::string& program)
    {
        std::vector<std::string> directories;
        const char* path = std::getenv("PATH");
        std::string path_list = path == nullptr ? std::string() : std::string(path);
        std::string::size_type start = 0;
        while (path != nullptr && start <= path_list.size())
        {
            std::string::size_type colon = path_list.find(':', start);
            if (colon == std::string::npos)
            {
                colon = path_list.size();
            }
            // An empty PATH entry means the current directory.
            std::string directory = path_list.substr(start, colon - start);
            directories.push_back(directory.empty() ? std::string(".") : directory);
            start = colon + 1;
        }
        for (const char* directory : kFallbackDirectories)
        {
            directories.push_back(directory);
        }
        for (const std::string& directory : directories)
        {
            std::string candidate = directory + "/" + program;
            if (::access(candidate.c_str(), X_OK) == 0)
            {
                return candidate;
            }
        }
        return "";
    }

    // FreeBSD's lrzsz package installs sz and rz as lsz and lrz.
    static std::string FindSz()
    {
        std::string sz = FindProgram("sz");
        return sz.empty() ? FindProgram("lsz") : sz;
    }

    static std::string FindRz()
    {
        std::string rz = FindProgram("rz");
        return rz.empty() ? FindProgram("lrz") : rz;
    }

    bool ZmodemSendAvailable()
    {
        return !FindSz().empty();
    }

    bool ZmodemReceiveAvailable()
    {
        return !FindRz().empty();
    }

    bool NoZmodemOnThisSystem()
    {
        return ::access("/etc/alpine-release", F_OK) == 0;
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
    // execs `sz` with `paths_` (one batch) and the real terminal's stdin/stdout inherited
    // (never redirected -- a ZMODEM-aware client needs to see the raw
    // protocol bytes in the same stream it's already reading), then waits
    // for it with a bounded timeout. Writes its outcome into `*ok_`/`*error_`
    // rather than returning one, since ftxui::Closure is a bare
    // std::function<void()>.
    //
    // Deliberately NOT passing `-e`/`--escape` here, unlike RunRzProcess: a
    // real download regressed (CRC errors, "Garbage count exceeded", ZCAN)
    // through the user's real terminal client (ZOC) after `-e` was added to
    // both directions to fix a real upload-corruption bug -- even though
    // sends had verified clean with that same real client both before that
    // change and on files exported before it.
    //
    // Note this is the OPPOSITE of what a local test harness shows: bridging
    // this process to a real standalone `rz` over a pair of local PTYs
    // reliably FAILS without `-e` (rz's own XON/XOFF pacing bytes seem to
    // desync the transfer without it) and reliably SUCCEEDS with `-e` added.
    // That local result doesn't generalize here -- a python relay loop
    // between two local ptys doesn't reproduce a real SSH channel's timing,
    // and the one client that actually matters (ZOC) showed the reverse.
    // Trust the real client's result over the local one if this needs
    // revisiting: whatever ZOC's zmodem receiver does with a fully-escaped
    // stream, it isn't the same as how it handles `sz`'s own normally-
    // negotiated escaping.
    class RunSzProcess
    {
    public:
        RunSzProcess(std::vector<std::string> paths, bool* ok, std::string* error)
            : paths_(std::move(paths)), ok_(ok), error_(error)
        {
        }

        void operator()() const
        {
            *ok_ = false;
            error_->clear();

            // Built before fork(): the child only execs.
            std::string sz = FindSz();
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>("sz"));
            for (const std::string& path : paths_)
            {
                argv.push_back(const_cast<char*>(path.c_str()));
            }
            argv.push_back(nullptr);

            pid_t pid = fork();
            if (pid < 0)
            {
                *error_ = "Could not start the ZMODEM sender (fork failed).";
                return;
            }
            if (pid == 0)
            {
                execv(sz.c_str(), argv.data());
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
        std::vector<std::string> paths_;
        bool* ok_;
        std::string* error_;
    };

    // Runs `transfer` (a RunSzProcess or RunRzProcess), then waits
    // kTerminalSettleMicroseconds before QuickLogger takes the terminal back.
    class ThenLetTerminalSettle
    {
    public:
        explicit ThenLetTerminalSettle(ftxui::Closure transfer) : transfer_(std::move(transfer)) {}

        void operator()() const
        {
            transfer_();
            usleep(kTerminalSettleMicroseconds);
        }

    private:
        ftxui::Closure transfer_;
    };

    bool SendFilesViaZmodem(ftxui::ScreenInteractive* screen, const std::vector<std::string>& paths, std::string* error)
    {
        if (!ZmodemSendAvailable())
        {
            *error = "The 'sz' command (lrzsz) isn't installed; skipped ZMODEM send.";
            return false;
        }

        bool ok = false;
        ftxui::Closure run = screen->WithRestoredIO(ThenLetTerminalSettle(RunSzProcess(paths, &ok, error)));
        run();
        // FTXUI took the terminal back, turning movement reports on again.
        RequestMouseMovementReportsOff();
        return ok;
    }

    // The receiving mirror of RunSzProcess: chdir()s into `dest_dir_` before
    // exec-ing `rz -e` (in the *child*, after fork -- never in this process,
    // since every other export/import path still needs to resolve paths
    // relative to wherever QuickLogger itself was launched from) so
    // whatever file arrives lands there under the name the sending client
    // gives it, then waits for it the same bounded way RunSzProcess does.
    // `-e`/`--escape` forces every control character (XON/XOFF, DLE, etc.)
    // to be escaped rather than sent raw -- fixes a real report where an
    // upload through an SSH pty racked up repeated "Transmission error
    // corrected" retries and finally cancelled (ZCAN) on an 84KB file, the
    // well-known "zmodem over ssh" failure mode caused by an unescaped
    // 0x11/0x13 landing in the file's byte stream and getting intercepted by
    // flow control before `rz` ever saw it. Kept receive-only -- see
    // RunSzProcess's comment for why the send side deliberately doesn't get
    // the same flag.
    class RunRzProcess
    {
    public:
        RunRzProcess(std::string dest_dir, bool* ok, std::string* error)
            : dest_dir_(std::move(dest_dir)), ok_(ok), error_(error)
        {
        }

        void operator()() const
        {
            *ok_ = false;
            error_->clear();

            // Found before fork(): the child only execs.
            std::string rz = FindRz();
            pid_t pid = fork();
            if (pid < 0)
            {
                *error_ = "Could not start the ZMODEM receiver (fork failed).";
                return;
            }
            if (pid == 0)
            {
                if (chdir(dest_dir_.c_str()) != 0)
                {
                    _exit(127);
                }
                execl(rz.c_str(), "rz", "-e", static_cast<char*>(nullptr));
                _exit(127);
            }

            int status = WaitWithTimeout(pid, kZmodemTimeoutSeconds);
            if (status == -1)
            {
                *error_ = "ZMODEM receive timed out -- no sender responded.";
                return;
            }
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            {
                *ok_ = true;
                return;
            }
            *error_ = "ZMODEM receive failed or was cancelled.";
        }

    private:
        std::string dest_dir_;
        bool* ok_;
        std::string* error_;
    };

    bool ReceiveFileViaZmodem(ftxui::ScreenInteractive* screen, const std::string& dest_dir, std::string* error)
    {
        if (!ZmodemReceiveAvailable())
        {
            *error = "The 'rz' command (lrzsz) isn't installed; can't receive via ZMODEM.";
            return false;
        }

        EnsureDirectory(dest_dir);

        bool ok = false;
        ftxui::Closure run = screen->WithRestoredIO(ThenLetTerminalSettle(RunRzProcess(dest_dir, &ok, error)));
        run();
        // FTXUI took the terminal back, turning movement reports on again.
        RequestMouseMovementReportsOff();
        return ok;
    }

}  // namespace ql

#endif  // _WIN32
