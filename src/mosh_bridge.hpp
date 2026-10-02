#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ql
{

    // Mosh on the built-in SSH server. `mosh user@host` logs in with ssh and
    // asks to run `mosh-server new ...`; mosh-server prints "MOSH CONNECT
    // <port> <key>", detaches, and from then on the session runs over UDP,
    // surviving a dropped connection or a change of network. ssh_server.cpp
    // runs the real mosh-server, but never with the client's command: the
    // program it starts is always this QuickLogger, `--mosh-session`, which
    // logs in as the user who authenticated over SSH by way of a one-time
    // token (CreateMoshToken/ConsumeMoshToken).
    //
    // Kept apart from libssh and from anything POSIX-only, so it can be
    // tested in every build.

    // What the client asked mosh-server for.
    struct MoshServerRequest
    {
        // The words after "new", as mosh-server takes them, with anything
        // from "--" on (the client's own command) left out.
        std::vector<std::string> options;
        // Run with --experimental-remote-ip=remote, the client first asks
        // for a "MOSH SSH_CONNECTION ..." line.
        bool report_ssh_connection = false;
    };

    // True if an exec request is a mosh client's (its program is
    // mosh-server, by any path: --server can name one).
    bool IsMoshServerCommandLine(std::string_view line);

    // Reads a mosh client's exec request: shell words in single quotes,
    // the way the client quotes them, after an optional SSH_CONNECTION
    // probe. Only mosh-server's own options are taken (-s, -v, -i ADDRESS,
    // -p PORT[:PORT2], -c COLORS, -l NAME=VALUE for a locale variable);
    // anything else, or anything a shell would act on, is refused with
    // `error` set.
    bool ParseMoshServerCommand(std::string_view line, MoshServerRequest* request, std::string* error);

    // The command line ssh_server.cpp runs: `mosh_server_path` new
    // <options> -- `self_path` --mosh-session.
    std::vector<std::string> MoshServerArgv(const std::string& mosh_server_path, const MoshServerRequest& request,
                                            const std::string& self_path);

    // Writes a one-time token for `username` to `dir`/mosh/<token> (only its
    // owner can read it) and returns the token, or "" with `error` set.
    // Tokens left over (never used) are removed while there.
    std::string CreateMoshToken(const std::string& dir, const std::string& username, std::int64_t now,
                                std::string* error);

    // Reads and deletes `token`'s file in `dir`/mosh. True, with `username`
    // set, if it's there and was made within kMoshTokenSeconds of `now`.
    bool ConsumeMoshToken(const std::string& dir, const std::string& token, std::int64_t now, std::string* username);

    // How long a token is good for: mosh-server starts its program as
    // soon as it has a client, a second or two after it's created.
    constexpr std::int64_t kMoshTokenSeconds = 120;

    // A Mosh session whose client hasn't been heard from in this long ends
    // (mosh-server's MOSH_SERVER_NETWORK_TMOUT): a day.
    constexpr std::int64_t kMoshIdleSeconds = 24 * 60 * 60;

}  // namespace ql
