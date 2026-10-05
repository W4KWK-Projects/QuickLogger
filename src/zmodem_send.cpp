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

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <deque>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

#include "file_export.hpp"
#include "sftp_paths.hpp"
#include "ui/mouse.hpp"
#include "zmodem_protocol.hpp"
#include "zmodem_terminal.hpp"

namespace ql
{

    // How long to wait for a receiver to answer before giving up. Chosen to
    // comfortably cover a human noticing and accepting a "receive file?"
    // prompt in their terminal client, while still being a bounded wait.
    static constexpr int kZmodemTimeoutSeconds = 25;
    // How long to leave the terminal alone once a transfer has finished,
    // before QuickLogger redraws. The terminal program is still wrapping up
    // then: ZOC, for one, prints its transfer summary (and the "OO" that
    // ends a ZMODEM session) only after the transfer, and may ignore what
    // arrives before. Written onto the normal screen during this pause, it
    // disappears when QuickLogger returns to its own; any sooner, and it's
    // left over QuickLogger's screen until the next key.
    static constexpr useconds_t kTerminalSettleMicroseconds = 1000000;

    // ZMODEM is QuickLogger's own (see zmodem_protocol.hpp): nothing to
    // install, and it works wherever there is a terminal to talk to.
    bool ZmodemSendAvailable()
    {
        return true;
    }

    bool ZmodemReceiveAvailable()
    {
        return true;
    }

    bool NoZmodemOnThisSystem()
    {
        return false;
    }

    // A path's file name alone.
    static std::string FileNameOf(const std::string& path)
    {
        std::string::size_type slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // Runs inside ScreenInteractive::WithRestoredIO's closure: sends the
    // files to the terminal. Writes its outcome into `*ok_`/`*error_`,
    // since ftxui::Closure is a bare std::function<void()>.
    class RunZmodemSend
    {
    public:
        RunZmodemSend(std::vector<std::string> paths, bool* ok, std::string* error)
            : paths_(std::move(paths)), ok_(ok), error_(error)
        {
        }

        void operator()() const
        {
            *ok_ = false;
            error_->clear();
            std::vector<ZmodemFile> files;
            for (const std::string& path : paths_)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    *error_ = "Couldn't read " + path + " to send it.";
                    return;
                }
                ZmodemFile file;
                file.name = FileNameOf(path);
                file.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                struct stat info{};
                file.mtime = ::stat(path.c_str(), &info) == 0 ? static_cast<std::int64_t>(info.st_mtime) : 0;
                files.push_back(std::move(file));
            }
            TerminalChannel terminal;
            bool answered = false;
            std::string failure;
            if (ZmodemSend(&terminal, files, kZmodemTimeoutSeconds, &answered, &failure))
            {
                *ok_ = true;
                return;
            }
            *error_ = answered ? "ZMODEM transfer failed or was cancelled: " + failure
                               : "ZMODEM transfer timed out -- no receiver responded.";
        }

    private:
        std::vector<std::string> paths_;
        bool* ok_;
        std::string* error_;
    };

    // Runs `transfer`, then waits kTerminalSettleMicroseconds before
    // QuickLogger takes the terminal back.
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
        bool ok = false;
        ftxui::Closure run = screen->WithRestoredIO(ThenLetTerminalSettle(RunZmodemSend(paths, &ok, error)));
        run();
        // FTXUI took the terminal back, turning movement reports on again.
        RequestMouseMovementReportsOff();
        return ok;
    }

    // `name` in `dir`, or the first of "name (2)", "name (3)"... that
    // isn't there already: a received file never replaces another.
    static std::string FreeFileName(const std::string& dir, const std::string& name)
    {
        std::string::size_type dot = name.find_last_of('.');
        std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
        std::string extension = dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
        std::string candidate = name;
        for (int n = 2; ::access((dir + "/" + candidate).c_str(), F_OK) == 0 && n < 1000; ++n)
        {
            candidate = stem + " (" + std::to_string(n) + ")" + extension;
        }
        return candidate;
    }

    // Receives what the terminal sends into `dest_dir_`.
    class RunZmodemReceive
    {
    public:
        RunZmodemReceive(std::string dest_dir, bool* ok, std::string* error)
            : dest_dir_(std::move(dest_dir)), ok_(ok), error_(error)
        {
        }

        void operator()() const
        {
            *ok_ = false;
            error_->clear();
            std::vector<ZmodemFile> received;
            std::string failure;
            bool transferred = false;
            {
                TerminalChannel terminal;
                transferred = ZmodemReceive(&terminal, &received, static_cast<std::size_t>(kSftpMaxUploadBytes),
                                            kZmodemTimeoutSeconds, &failure);
            }
            // What arrived whole is kept even if the batch didn't finish.
            for (const ZmodemFile& file : received)
            {
                std::string path = dest_dir_ + "/" + FreeFileName(dest_dir_, file.name);
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(file.data.data(), static_cast<std::streamsize>(file.data.size()));
                if (!out)
                {
                    *error_ = "Couldn't save " + file.name + ".";
                    return;
                }
            }
            if (transferred)
            {
                *ok_ = true;
                return;
            }
            *error_ = failure == "The sender stopped answering." && received.empty()
                          ? "ZMODEM receive timed out -- no sender responded."
                          : "ZMODEM receive failed or was cancelled: " + failure;
        }

    private:
        std::string dest_dir_;
        bool* ok_;
        std::string* error_;
    };

    bool ReceiveFileViaZmodem(ftxui::ScreenInteractive* screen, const std::string& dest_dir, std::string* error)
    {
        EnsureDirectory(dest_dir);

        bool ok = false;
        ftxui::Closure run = screen->WithRestoredIO(ThenLetTerminalSettle(RunZmodemReceive(dest_dir, &ok, error)));
        run();
        // FTXUI took the terminal back, turning movement reports on again.
        RequestMouseMovementReportsOff();
        return ok;
    }

}  // namespace ql

#endif  // _WIN32
