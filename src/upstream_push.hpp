#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "run_program.hpp"

namespace ql
{

    // Federated Logging's client side: pushing a closed session to an
    // upstream QuickLogger. The session's .qlsession goes up with the
    // system's own scp into the upstream user's /imports, then the system's
    // own ssh runs `import-session` there (docs/IMPORT_SESSION.md). Both use
    // the local account's ~/.ssh (keys, known_hosts, config, ssh-agent);
    // QuickLogger keeps no keys and checks no host keys itself.
    //
    // Both run with BatchMode, so ssh never asks for a password or
    // passphrase behind the screen: a key with a passphrase has to be in
    // ssh-agent, and an unknown host key fails. The user logs in once with
    // plain ssh first.
    //
    // The decisions (arguments, reading the reply, what to tell the
    // operator) are plain functions, testable without running ssh;
    // PushSessionFile is the part that runs it.

    struct Upstream
    {
        std::string host;
        std::string user;
        int port = 22;
    };

    // How long ssh may take to connect.
    constexpr int kUpstreamConnectTimeoutSeconds = 15;

    // How long each of scp and ssh may run in all before it's stopped.
    constexpr int kUpstreamRunTimeoutSeconds = 120;

    // A host name or address QuickLogger will pass to ssh: letters, digits,
    // '.', '-' and '_', not starting with '-'. (No IPv6 addresses: a Host
    // entry in ~/.ssh/config can name one.)
    bool IsValidUpstreamHost(std::string_view host);

    // A user name likewise: letters, digits, '.', '-' and '_', not starting
    // with '-'.
    bool IsValidUpstreamUser(std::string_view user);

    // scp's arguments (after the program itself) to copy `local_path` to
    // `remote_name` in the upstream user's /imports.
    std::vector<std::string> UpstreamScpArguments(const Upstream& upstream, const std::string& local_path,
                                                  const std::string& remote_name);

    // ssh's arguments (after the program itself) to run `command` on the
    // upstream, as one string for QuickLogger's own command line
    // (remote_command.hpp).
    std::vector<std::string> UpstreamSshArguments(const Upstream& upstream, const std::string& command);

    // `text` as one double-quoted word for that command line.
    std::string QuotedCommandWord(const std::string& text);

    // One sentence on why ssh or scp (`run`) failed, from what it printed:
    // no ssh, no reaching the host, an unknown host key, a refused key.
    // `fallback` ("Couldn't copy the session to") plus the host is the
    // answer when it can't tell; `uploading` also reads scp's complaints
    // about a view-only user, a file over the limit and a full /imports.
    std::string UpstreamFailureMessage(const Upstream& upstream, const ProgramResult& run, const char* fallback,
                                       bool uploading);

    // ssh's arguments to run import-session (import-net if `net`) on
    // `remote_name`, with --confirm-net `confirm_net` if it isn't empty. The command reaches the
    // upstream as one string; the net name in it is quoted as
    // import-session reads it (SplitCommandLine).
    std::vector<std::string> UpstreamImportArguments(const Upstream& upstream, const std::string& remote_name,
                                                     const std::string& confirm_net, bool net = false);

    // What import-session answered.
    enum class ImportReplyStatus
    {
        kImported,
        kMerged,  // import-net: merged into a net there.
        kAlreadyImported,
        kNeedsConfirmation,
        kNoMatch,
        kRefused,
        kError,
        // Not an import-session answer at all: no "QUICKLOGGER-RESULT 1"
        // first line, or a status it doesn't know.
        kUnreadable,
    };

    struct ImportReply
    {
        ImportReplyStatus status = ImportReplyStatus::kUnreadable;
        std::string net;
        std::int64_t net_id = 0;
        std::string session;
        std::string message;
        // import-net: what merging adds, and the sessions that differ.
        bool has_counts = false;
        int new_sessions = 0;
        int new_saved_stations = 0;
        int differing_sessions = 0;
    };

    // Reads import-session's (or import-net's) output.
    ImportReply ParseImportReply(std::string_view output);

    // True if the reply says the file isn't in the user's /imports (cleared
    // since it was uploaded): the one refusal worth uploading again for.
    bool ImportReplyMeansFileMissing(const ImportReply& reply);

    // How a push went, for the operator.
    enum class PushResultKind
    {
        kPushed,             // Imported, or already there: pushed_at is set.
        kNeedsConfirmation,  // Ask whether `upstream_net` is the right net, then push again with it.
        kFailed,
    };

    struct PushResult
    {
        PushResultKind kind = PushResultKind::kFailed;
        // One sentence for the status line.
        std::string message;
        // kNeedsConfirmation: the upstream's net to confirm, and for a net
        // pushed, what merging into it would do ("adds 2 sessions and 1 saved
        // station; 1 session differs and stays as it is there").
        std::string upstream_net;
        std::string merge_summary;
    };

    // ssh's arguments to run discard-upload on `remote_name`.
    std::vector<std::string> UpstreamDiscardArguments(const Upstream& upstream, const std::string& remote_name);

    // The push's result once scp has run (`copy`) and, if the copy worked,
    // ssh too (`import`, else ignored). `session_net` is the session's own
    // net name, for "no net like" messages.
    PushResult DecidePushResult(const Upstream& upstream, const ProgramResult& copy, const ProgramResult& import,
                                const std::string& session_net);

    // What merging a pushed net adds, as a sentence without its end:
    // "adds 2 sessions and 1 saved station; 1 session differs and stays as
    // it is there".
    std::string MergeSummary(const ImportReply& reply);

    // Pushes the .qlsession (or, if `net`, .qlnet) at `local_path` to `upstream` as `remote_name`,
    // confirming `confirm_net` if it isn't blank. A confirmation runs the
    // import on the upload the upstream kept when it asked, and copies the
    // file again only if the upstream no longer has it. Runs scp and ssh, each for
    // up to kUpstreamRunTimeoutSeconds, and stops as soon as `*cancel`
    // becomes true. Changes nothing here: the caller sets pushed_at.
    PushResult PushSessionFile(const Upstream& upstream, const std::string& local_path, const std::string& remote_name,
                               const std::string& confirm_net, const std::string& session_net,
                               const std::atomic<bool>* cancel, bool net = false);

    // Asks the upstream to delete the upload of `remote_name` it kept when it
    // asked about a look-alike net and the answer was no. Best effort and
    // silent: an upstream that can't (it's gone, or older and doesn't know
    // the command) sweeps old uploads itself after a week.
    void DiscardUpstreamUpload(const Upstream& upstream, const std::string& remote_name,
                               const std::atomic<bool>* cancel);

    // True if both ssh and scp can be found (FindProgramOnPath).
    bool UpstreamToolsAvailable();

    // The sentence for a push that can't happen because they can't be.
    extern const char* const kNoSshMessage;

}  // namespace ql
