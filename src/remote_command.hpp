#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "db/database.hpp"

namespace ql
{

    // The commands the SSH server runs for an exec request (`ssh user@host
    // <command>`): QuickLogger's own, never a shell or another program. A
    // public, versioned interface for other QuickLoggers and apps pushing
    // sessions upstream and pulling nets and sessions back (Federated
    // Logging); docs/IMPORT_SESSION.md is its documentation.
    //
    //   version
    //   import-session [--confirm-net "<net name>"] <file>
    //   discard-upload <file>
    //   list-nets
    //   export-net "<net name>"
    //   export-sessions "<net name>"
    //
    // Every command answers in the same form: "QUICKLOGGER-RESULT 1", then
    // "key: value" lines (see RemoteCommandResult), and an exit status.
    //
    // Kept apart from the libssh code (ssh_server.cpp only passes the
    // command line in and the result out) so it can be tested in a build
    // without the SSH server.

    // The version of this interface: the number after QUICKLOGGER-RESULT,
    // and `version`'s "interface:".
    constexpr int kRemoteCommandInterface = 1;

    enum class RemoteCommandKind
    {
        kVersion,
        kImportSession,
        kDiscardUpload,
        kListNets,
        kExportNet,
        kExportSessions,
    };

    struct RemoteCommand
    {
        RemoteCommandKind kind = RemoteCommandKind::kVersion;
        // import-session and discard-upload: the file as given, and --confirm-net's net name.
        // export-net and export-sessions: the net's name, in `file`.
        std::string file;
        bool has_confirm_net = false;
        std::string confirm_net;
    };

    // The most sessions export-sessions writes at once (the newest): each is
    // a file of its own, and the caller fetches them one by one.
    constexpr int kRemoteMaxExportedSessions = 300;

    // Exit statuses, as documented.
    constexpr int kRemoteExitOk = 0;
    constexpr int kRemoteExitError = 1;
    constexpr int kRemoteExitNeedsConfirmation = 2;
    constexpr int kRemoteExitNoMatch = 3;
    constexpr int kRemoteExitRefused = 4;

    struct RemoteCommandResult
    {
        std::string output;  // Every line ends in '\n'.
        int exit_status = kRemoteExitOk;
    };

    // Splits `line` into words without a shell: words are separated by
    // spaces or tabs, and a "double-quoted" string is one word (\" and \\
    // inside it stand for " and \). Outside quotes only letters, digits and
    // . _ - / + = : , @ % are allowed, so anything a shell would act on (;
    // | & $ ` < > ' and the rest) is refused, not passed along. Returns
    // false, with `error` set to a sentence, for anything else: an unclosed
    // quote, a control character, a quote in the middle of a word.
    bool SplitCommandLine(std::string_view line, std::vector<std::string>* words, std::string* error);

    // Parses a whole command line into `command`. Returns false, with
    // `error` set, for an unknown command, a missing or extra argument, or
    // anything SplitCommandLine refuses.
    bool ParseRemoteCommand(std::string_view line, RemoteCommand* command, std::string* error);

    // The answer to a command line that couldn't be parsed: status error,
    // exit 1, `error` as the message.
    RemoteCommandResult RemoteCommandParseError(const std::string& error);

    // Runs `command` for SSH user `username` against `db` (the database at
    // `db_path`, whose ssh-users/<username> import folder holds the files).
    // A view-only user is refused import-session and discard-upload (they
    // change things here); version, list-nets, export-net and
    // export-sessions only read, so they may run them, as they may take an
    // export over ZMODEM. `now` is the import time.
    //
    // import-session imports a .qlsession from the user's /imports (the same
    // name rules as SFTP: sftp_paths.hpp) as History's F6 does. A recurring
    // net's session goes to the net with the same name (NetNamesAreTheSame);
    // failing that the first look-alike, alphabetically, is offered back
    // (needs-confirmation) and only imported into once named with
    // --confirm-net. An ad hoc net's becomes a new ad hoc net. A session
    // already there is "already-imported", not an error. The file is deleted
    // once imported (or found already imported), and kept otherwise.
    //
    // list-nets answers with one `net:` / `service:` / `sessions:` group per
    // recurring net. export-net writes the net (as a .qlnet) and
    // export-sessions each of its closed sessions (as a .qlsession) into the
    // user's /exports, answering with the file names (`file:` lines) for the
    // client to fetch; the net is named exactly (NetNamesAreTheSame), which
    // the client had from list-nets. At most kRemoteMaxExportedSessions
    // sessions go, the newest.
    RemoteCommandResult RunRemoteCommand(const RemoteCommand& command, Database* db, const std::string& db_path,
                                         const std::string& username, bool view_only, std::int64_t now);

}  // namespace ql
