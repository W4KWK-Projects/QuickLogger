#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ql
{

    // The old SCP protocol (`scp -t` to receive, `scp -f` to send) on the
    // built-in SSH server, for scp clients that don't speak SFTP: the
    // OpenSSH that comes with Windows 10 and 11, and anything older than
    // OpenSSH 9.0 (or newer with -O). The client asks for it as an exec
    // request ("scp -t /imports/"); ssh_server.cpp runs it through
    // RunScpCommand instead of any program.
    //
    // It sees the same files as SFTP (sftp_paths.hpp) under the same rules:
    // single files only, /exports read-only, /imports read-only for a
    // view-only user, uploads only of .qlnet and .qlsession files within
    // kSftpMaxUploadBytes and kSftpMaxImportsBytes, each written to a
    // hidden temporary file and moved into place once whole.
    //
    // Kept apart from libssh, behind ScpChannel, so it can be tested in a
    // build without the SSH server.

    // The byte stream a transfer runs over: the SSH channel, or a buffer in
    // tests.
    class ScpChannel
    {
    public:
        virtual ~ScpChannel() = default;
        // Reads exactly `size` bytes. False at the end of the stream or on
        // an error.
        virtual bool Read(char* data, std::size_t size) = 0;
        virtual bool Write(const char* data, std::size_t size) = 0;
    };

    struct ScpCommand
    {
        // -t: the client sends files here. Otherwise -f: it fetches one.
        bool receive = false;
        // -p: the client sends (or wants) modification times. Accepted;
        // QuickLogger sets the times of what it writes itself.
        bool preserve = false;
        // -d: the target has to be a folder (several files sent).
        bool target_is_folder = false;
        std::string path;
    };

    // True if `line` asks for scp: its first word is "scp".
    bool IsScpCommandLine(std::string_view line);

    // Parses the command an scp client sends ("scp -v -p -t -- /imports/").
    // Returns false, with `error` set, for anything but -t or -f with one
    // path and the options -p, -d, -v and -q; -r (folders) is refused.
    bool ParseScpCommand(std::string_view line, ScpCommand* command, std::string* error);

    // Parses a file line ("C0644 1234 Net.qlnet", without its newline).
    bool ParseScpFileLine(std::string_view line, std::uint64_t* size, std::string* name, std::string* error);

    // Runs `command` for SSH user `username` (whose folders are next to the
    // database at `db_path`) over `channel`. Returns the exit status: 0 if
    // every file was copied, 1 otherwise.
    int RunScpCommand(const ScpCommand& command, ScpChannel* channel, const std::string& db_path,
                      const std::string& username, bool view_only);

}  // namespace ql
