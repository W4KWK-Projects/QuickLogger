#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "run_program.hpp"
#include "upstream_push.hpp"

namespace ql
{

    // Federated Logging's other direction: pulling a net, or the sessions of
    // one, from the upstream QuickLogger into this one. The system's own ssh
    // asks the upstream for its nets (list-nets) and has it write the one
    // chosen into its /exports (export-net, export-sessions); the system's own
    // scp then fetches the files (docs/IMPORT_SESSION.md). What was fetched
    // goes through the same import as a file received any other way.
    //
    // As with pushing (upstream_push.hpp), both run with BatchMode and the
    // local account's ~/.ssh, and the decisions are plain functions, testable
    // without running ssh; the Pull* functions run it.

    // One recurring net the upstream offers (list-nets).
    struct UpstreamNet
    {
        std::string name;
        // "amateur" or "gmrs".
        std::string service;
        // Its closed sessions.
        int sessions = 0;
    };

    // How many files one scp fetches; more go in further runs.
    constexpr std::size_t kPullFilesPerCopy = 25;

    // What a pull step came to.
    enum class PullResultKind
    {
        kFailed,
        kNets,   // list-nets worked: `nets`.
        kFiles,  // The files were fetched: `files`.
    };

    struct PullResult
    {
        PullResultKind kind = PullResultKind::kFailed;
        // kFailed: one sentence for the status line.
        std::string message;
        std::vector<UpstreamNet> nets;
        // The upstream's own name for the net asked for (kFiles).
        std::string net;
        // The files fetched, as paths under the folder given, in the order
        // the upstream named them (newest session first).
        std::vector<std::string> files;
        // Sessions the upstream left out (still open, or past its limit).
        int not_sent = 0;
    };

    // ssh's arguments to run list-nets.
    std::vector<std::string> UpstreamListNetsArguments(const Upstream& upstream);

    // ssh's arguments to run export-net (`sessions` false) or
    // export-sessions on the net called `net_name`.
    std::vector<std::string> UpstreamExportArguments(const Upstream& upstream, const std::string& net_name,
                                                     bool sessions);

    // scp's arguments to copy the files named `remote_names` from the
    // upstream user's /exports into the folder `local_dir`.
    std::vector<std::string> UpstreamFetchArguments(const Upstream& upstream,
                                                    const std::vector<std::string>& remote_names,
                                                    const std::string& local_dir);

    // The answer to list-nets once ssh has run (`run`): the nets (kNets, in
    // the upstream's order), or why not.
    PullResult DecideListNetsResult(const Upstream& upstream, const ProgramResult& run);

    // The answer to export-net or export-sessions once ssh has run: kFiles
    // with the names the upstream gave, still to be fetched (`files` holds
    // names only here), or why not. Only names a /exports file of the right
    // kind (`extension`, ".qlnet" or ".qlsession") can have are taken; the
    // upstream can't make the client fetch anything else.
    PullResult DecideExportResult(const Upstream& upstream, const ProgramResult& run, const std::string& net_name,
                                  const std::string& extension);

    // Asks the upstream for its nets. Runs ssh for up to
    // kUpstreamRunTimeoutSeconds, stopping as soon as `*cancel` becomes true.
    PullResult PullNetList(const Upstream& upstream, const std::atomic<bool>* cancel);

    // Has the upstream export net `net_name` (sessions: its closed
    // sessions, one file each; otherwise the net as one .qlnet) and fetches
    // the files into `local_dir`, which it creates. `files` are their paths
    // there. Runs ssh and scp, each for up to kUpstreamRunTimeoutSeconds, and
    // stops as soon as `*cancel` becomes true. Changes nothing else here.
    PullResult PullNetFiles(const Upstream& upstream, const std::string& net_name, bool sessions,
                            const std::string& local_dir, const std::atomic<bool>* cancel);

}  // namespace ql
