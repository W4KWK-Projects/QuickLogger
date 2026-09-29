#include "show_folder.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ql
{

    static bool EnvironmentHas(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0';
    }

    bool IsLocalTerminal(bool is_console_session)
    {
        return is_console_session && !EnvironmentHas("SSH_CONNECTION") &&
               !EnvironmentHas("SSH_CLIENT") && !EnvironmentHas("SSH_TTY");
    }

    bool CanShowInFileManager()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return true;
#else
        return EnvironmentHas("DISPLAY") || EnvironmentHas("WAYLAND_DISPLAY");
#endif
    }

    // `path` made absolute, since the file manager doesn't start in our
    // working directory.
    static std::filesystem::path AbsolutePath(const std::string& path)
    {
        std::error_code ignored;
        std::filesystem::path absolute =
            std::filesystem::absolute(std::filesystem::u8path(path), ignored);
        return absolute.empty() ? std::filesystem::u8path(path) : absolute.lexically_normal();
    }

#if defined(_WIN32)

    static std::wstring Widen(const std::string& text)
    {
        int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (length <= 0)
        {
            return std::wstring();
        }
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], length);
        wide.resize(static_cast<std::size_t>(length - 1));
        return wide;
    }

    // Selects every one of `files` (all in one folder) in a File Explorer
    // window on that folder.
    static bool SelectInExplorer(const std::vector<std::wstring>& files)
    {
        std::wstring folder = std::filesystem::path(files.front()).parent_path().wstring();
        PIDLIST_ABSOLUTE folder_item = ILCreateFromPathW(folder.c_str());
        if (folder_item == nullptr)
        {
            return false;
        }
        std::vector<PIDLIST_ABSOLUTE> file_items;
        std::vector<PCUITEMID_CHILD> children;
        for (const std::wstring& file : files)
        {
            PIDLIST_ABSOLUTE item = ILCreateFromPathW(file.c_str());
            if (item != nullptr)
            {
                file_items.push_back(item);
                children.push_back(reinterpret_cast<PCUITEMID_CHILD>(ILFindLastID(item)));
            }
        }
        HRESULT result = E_FAIL;
        if (!children.empty())
        {
            result = SHOpenFolderAndSelectItems(folder_item, static_cast<UINT>(children.size()),
                                                children.data(), 0);
        }
        for (PIDLIST_ABSOLUTE item : file_items)
        {
            ILFree(item);
        }
        ILFree(folder_item);
        return SUCCEEDED(result);
    }

    bool ShowInFileManager(const std::vector<std::string>& paths, std::string* error)
    {
        if (paths.empty())
        {
            return true;
        }
        std::vector<std::wstring> files;
        for (const std::string& path : paths)
        {
            files.push_back(Widen(AbsolutePath(path).make_preferred().u8string()));
        }

        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool selected = SelectInExplorer(files);
        if (SUCCEEDED(com))
        {
            CoUninitialize();
        }
        if (selected)
        {
            return true;
        }

        // Explorer can still select one of them.
        std::wstring command = L"explorer.exe /select,\"" + files.front() + L"\"";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &startup, &process))
        {
            *error = "couldn't start File Explorer";
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

#else

#if !defined(__APPLE__)
    // `path` as a file:// URI, every byte but unreserved characters and '/'
    // percent-encoded (dbus-send would also split an unencoded ',').
    static std::string FileUri(const std::string& path)
    {
        static const char kHex[] = "0123456789ABCDEF";
        std::string uri = "file://";
        for (char c : path)
        {
            unsigned char byte = static_cast<unsigned char>(c);
            bool plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                         (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                         byte == '.' || byte == '~' || byte == '/';
            if (plain)
            {
                uri += c;
            }
            else
            {
                uri += '%';
                uri += kHex[byte >> 4];
                uri += kHex[byte & 0x0F];
            }
        }
        return uri;
    }
#endif

    // Runs `args` without waiting for it: a child forks the real process
    // and exits at once, so it's never left a zombie and the file manager
    // outlives nothing of ours. Its stdin/stdout/stderr go to /dev/null.
    static bool SpawnDetached(const std::vector<std::string>& args)
    {
        std::vector<char*> argv;
        for (const std::string& arg : args)
        {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        pid_t child = fork();
        if (child < 0)
        {
            return false;
        }
        if (child == 0)
        {
            setsid();
            if (fork() == 0)
            {
                int null_fd = open("/dev/null", O_RDWR);
                if (null_fd >= 0)
                {
                    dup2(null_fd, STDIN_FILENO);
                    dup2(null_fd, STDOUT_FILENO);
                    dup2(null_fd, STDERR_FILENO);
                }
                execvp(argv[0], argv.data());
                _exit(127);
            }
            _exit(0);
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        {
        }
        return true;
    }

    bool ShowInFileManager(const std::vector<std::string>& paths, std::string* error)
    {
        if (paths.empty())
        {
            return true;
        }
        std::vector<std::string> files;
        for (const std::string& path : paths)
        {
            files.push_back(AbsolutePath(path).string());
        }
#if defined(__APPLE__)
        // Finder's own "reveal" selects every file; `open -R` selects only
        // one, so it's the fallback if macOS's permission for the terminal
        // to control Finder is declined.
        std::vector<std::string> args = {
            "sh",
            "-c",
            "osascript -e 'on run argv' -e 'set found to {}' -e 'repeat with p in argv' "
            "-e 'set end of found to (POSIX file (contents of p)) as alias' -e 'end repeat' "
            "-e 'tell application \"Finder\"' -e 'reveal found' -e 'activate' -e 'end tell' "
            "-e 'end run' \"$@\" || open -R \"$1\"",
            "sh",
        };
        args.insert(args.end(), files.begin(), files.end());
#else
        // The freedesktop.org file-manager request selects the files
        // (Nautilus, Dolphin, Nemo, Thunar, Caja...); without a file
        // manager that answers it, xdg-open just opens the folder.
        std::string uris;
        for (const std::string& file : files)
        {
            uris += (uris.empty() ? "" : ",") + FileUri(file);
        }
        std::string folder = AbsolutePath(paths.front()).parent_path().string();
        std::vector<std::string> args = {
            "sh",
            "-c",
            "dbus-send --session --print-reply --dest=org.freedesktop.FileManager1 "
            "--type=method_call /org/freedesktop/FileManager1 "
            "org.freedesktop.FileManager1.ShowItems \"array:string:$1\" string: "
            "|| xdg-open \"$2\"",
            "sh",
            uris,
            folder,
        };
#endif
        if (!SpawnDetached(args))
        {
            *error = "couldn't start the file manager";
            return false;
        }
        return true;
    }

#endif

}  // namespace ql
