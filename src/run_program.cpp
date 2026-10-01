#include "run_program.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace ql
{

    // How much of each output is kept; the rest is read and dropped.
    static const std::size_t kMaxOutputBytes = 64 * 1024;

    static void AppendCapped(std::string* text, const char* data, std::size_t size)
    {
        if (text->size() < kMaxOutputBytes)
        {
            text->append(data, std::min(size, kMaxOutputBytes - text->size()));
        }
    }

    std::string WindowsCommandLine(const std::string& program, const std::vector<std::string>& arguments)
    {
        std::string line;
        std::vector<const std::string*> all;
        all.reserve(arguments.size() + 1);
        all.emplace_back(&program);
        for (const std::string& argument : arguments)
        {
            all.emplace_back(&argument);
        }
        for (std::size_t index = 0; index < all.size(); ++index)
        {
            const std::string& argument = *all[index];
            if (index > 0)
            {
                line += ' ';
            }
            if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos)
            {
                line += argument;
                continue;
            }
            line += '"';
            std::size_t backslashes = 0;
            for (char c : argument)
            {
                if (c == '\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == '"')
                {
                    // Backslashes before a quote are doubled, and the quote
                    // escaped.
                    line.append(backslashes * 2 + 1, '\\');
                }
                else
                {
                    line.append(backslashes, '\\');
                }
                backslashes = 0;
                line += c;
            }
            // And doubled before the closing quote.
            line.append(backslashes * 2, '\\');
            line += '"';
        }
        return line;
    }

#if defined(_WIN32)

    static std::wstring Wide(const std::string& text)
    {
        if (text.empty())
        {
            return std::wstring();
        }
        int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], size);
        return wide;
    }

    static std::string Narrow(const std::wstring& text)
    {
        if (text.empty())
        {
            return std::string();
        }
        int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                       nullptr);
        std::string narrow(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &narrow[0], size, nullptr,
                            nullptr);
        return narrow;
    }

    static bool IsFile(const std::string& path)
    {
        DWORD attributes = GetFileAttributesW(Wide(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::string FindProgramOnPath(const std::string& name)
    {
        std::string file = name + ".exe";
        std::vector<std::string> directories;
        DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (size > 0)
        {
            std::wstring path(size, L'\0');
            DWORD length = GetEnvironmentVariableW(L"PATH", &path[0], size);
            path.resize(length);
            std::string list = Narrow(path);
            std::string::size_type start = 0;
            while (start <= list.size())
            {
                std::string::size_type semicolon = list.find(';', start);
                if (semicolon == std::string::npos)
                {
                    semicolon = list.size();
                }
                std::string directory = list.substr(start, semicolon - start);
                if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"')
                {
                    directory = directory.substr(1, directory.size() - 2);
                }
                if (!directory.empty())
                {
                    directories.emplace_back(std::move(directory));
                }
                start = semicolon + 1;
            }
        }
        wchar_t windows[MAX_PATH];
        UINT windows_length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
        if (windows_length > 0 && windows_length < MAX_PATH)
        {
            directories.emplace_back(Narrow(std::wstring(windows, windows_length)) + "\\System32\\OpenSSH");
        }
        for (const std::string& directory : directories)
        {
            std::string candidate = directory + "\\" + file;
            if (IsFile(candidate))
            {
                return candidate;
            }
        }
        return "";
    }

    // Reads a pipe to its end on a thread of its own, so a program that
    // fills one output while RunProgram waits on the other never stalls.
    class PipeReader
    {
    public:
        PipeReader(HANDLE pipe, std::string* text) : pipe_(pipe), text_(text) {}

        void operator()() const
        {
            char buffer[4096];
            DWORD count = 0;
            while (ReadFile(pipe_, buffer, sizeof(buffer), &count, nullptr) && count > 0)
            {
                AppendCapped(text_, buffer, count);
            }
        }

    private:
        HANDLE pipe_;
        std::string* text_;
    };

    ProgramResult RunProgram(const std::string& path, const std::vector<std::string>& arguments, int timeout_seconds,
                             const std::atomic<bool>* cancel)
    {
        ProgramResult result;
        SECURITY_ATTRIBUTES inherit{};
        inherit.nLength = sizeof(inherit);
        inherit.bInheritHandle = TRUE;

        HANDLE out_read = nullptr;
        HANDLE out_write = nullptr;
        HANDLE err_read = nullptr;
        HANDLE err_write = nullptr;
        if (!CreatePipe(&out_read, &out_write, &inherit, 0) || !CreatePipe(&err_read, &err_write, &inherit, 0))
        {
            result.error = "Couldn't start " + path + ".";
            return result;
        }
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
        HANDLE nothing = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nothing;
        startup.hStdOutput = out_write;
        startup.hStdError = err_write;
        PROCESS_INFORMATION process{};
        std::wstring command_line = Wide(WindowsCommandLine(path, arguments));
        std::wstring application = Wide(path);
        BOOL created = CreateProcessW(application.c_str(), &command_line[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                      nullptr, nullptr, &startup, &process);
        // Only the program holds the write ends now, so the pipes end when
        // it does.
        CloseHandle(out_write);
        CloseHandle(err_write);
        if (nothing != INVALID_HANDLE_VALUE)
        {
            CloseHandle(nothing);
        }
        if (!created)
        {
            CloseHandle(out_read);
            CloseHandle(err_read);
            result.error = "Couldn't start " + path + ".";
            return result;
        }
        result.started = true;

        std::thread out_thread(PipeReader(out_read, &result.output));
        std::thread err_thread(PipeReader(err_read, &result.errors));
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        bool ended = false;
        while (!ended)
        {
            ended = WaitForSingleObject(process.hProcess, 200) == WAIT_OBJECT_0;
            if (!ended && (std::chrono::steady_clock::now() >= deadline || (cancel != nullptr && cancel->load())))
            {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, INFINITE);
                result.stopped = true;
                break;
            }
        }
        DWORD exit_code = 0;
        if (!result.stopped && GetExitCodeProcess(process.hProcess, &exit_code))
        {
            result.exit_status = static_cast<int>(exit_code);
        }
        out_thread.join();
        err_thread.join();
        CloseHandle(out_read);
        CloseHandle(err_read);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return result;
    }

#else

    // Where systems keep ssh when PATH doesn't name them (a service started
    // at boot gets a short PATH).
    static const char* const kSystemDirectories[] = {
        "/usr/bin", "/bin", "/usr/local/bin", "/opt/homebrew/bin",
    };

    std::string FindProgramOnPath(const std::string& name)
    {
        std::vector<std::string> directories;
        const char* path = std::getenv("PATH");
        if (path != nullptr)
        {
            std::string list = path;
            std::string::size_type start = 0;
            while (start <= list.size())
            {
                std::string::size_type colon = list.find(':', start);
                if (colon == std::string::npos)
                {
                    colon = list.size();
                }
                // An empty entry (the current directory) is skipped: never
                // run a program that happens to be lying where we started.
                if (colon > start)
                {
                    directories.emplace_back(list.substr(start, colon - start));
                }
                start = colon + 1;
            }
        }
        for (const char* directory : kSystemDirectories)
        {
            directories.emplace_back(directory);
        }
        for (const std::string& directory : directories)
        {
            std::string candidate = directory + "/" + name;
            if (::access(candidate.c_str(), X_OK) == 0)
            {
                return candidate;
            }
        }
        return "";
    }

    static void CloseOnExec(int fd)
    {
        ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }

    // Reads what's waiting on `fd` into `text`; false once it has ended.
    static bool Drain(int fd, std::string* text)
    {
        char buffer[4096];
        ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0)
        {
            AppendCapped(text, buffer, static_cast<std::size_t>(count));
            return true;
        }
        return count < 0 && (errno == EINTR || errno == EAGAIN);
    }

    ProgramResult RunProgram(const std::string& path, const std::vector<std::string>& arguments, int timeout_seconds,
                             const std::atomic<bool>* cancel)
    {
        ProgramResult result;
        int out_pipe[2] = {-1, -1};
        int err_pipe[2] = {-1, -1};
        if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0)
        {
            result.error = "Couldn't start " + path + ".";
            return result;
        }
        CloseOnExec(out_pipe[0]);
        CloseOnExec(out_pipe[1]);
        CloseOnExec(err_pipe[0]);
        CloseOnExec(err_pipe[1]);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.emplace_back(const_cast<char*>(path.c_str()));
        for (const std::string& argument : arguments)
        {
            argv.emplace_back(const_cast<char*>(argument.c_str()));
        }
        argv.emplace_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
        posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
        // Its own process group, so a terminal signal (Ctrl-C) for
        // QuickLogger doesn't reach it, and stopping it stops what it ran.
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);
        pid_t pid = -1;
        int spawned = posix_spawn(&pid, path.c_str(), &actions, &attributes, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        if (spawned != 0)
        {
            ::close(out_pipe[0]);
            ::close(err_pipe[0]);
            result.error = "Couldn't start " + path + ".";
            return result;
        }
        result.started = true;

        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        bool out_open = true;
        bool err_open = true;
        while (out_open || err_open)
        {
            if (std::chrono::steady_clock::now() >= deadline || (cancel != nullptr && cancel->load()))
            {
                result.stopped = true;
                break;
            }
            struct pollfd fds[2];
            fds[0].fd = out_open ? out_pipe[0] : -1;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            fds[1].fd = err_open ? err_pipe[0] : -1;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            int ready = ::poll(fds, 2, 200);
            if (ready <= 0)
            {
                continue;
            }
            if (out_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            {
                out_open = Drain(out_pipe[0], &result.output);
            }
            if (err_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            {
                err_open = Drain(err_pipe[0], &result.errors);
            }
        }
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);

        int status = 0;
        if (result.stopped)
        {
            ::kill(-pid, SIGTERM);
            for (int i = 0; i < 20 && ::waitpid(pid, &status, WNOHANG) == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ::kill(-pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return result;
        }
        // Its outputs have ended; it's ending too. Waited for no longer than
        // the time it had left.
        while (true)
        {
            pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid)
            {
                result.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                break;
            }
            if (waited < 0 && errno != EINTR)
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline || (cancel != nullptr && cancel->load()))
            {
                result.stopped = true;
                ::kill(-pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return result;
    }

#endif

}  // namespace ql
