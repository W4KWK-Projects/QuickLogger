#include "upstream_pull.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "file_export.hpp"
#include "sftp_paths.hpp"

namespace ql
{

    static PullResult Failed(std::string message)
    {
        PullResult result;
        result.kind = PullResultKind::kFailed;
        result.message = std::move(message);
        return result;
    }

    // One `key: value` line of a command's answer.
    struct ResultLine
    {
        std::string key;
        std::string value;
    };

    // Reads a command's output into its `key: value` lines. False if it
    // doesn't begin with "QUICKLOGGER-RESULT 1": not an answer at all.
    static bool ReadResultLines(std::string_view output, std::vector<ResultLine>* lines)
    {
        bool first = true;
        std::string_view::size_type start = 0;
        while (start < output.size())
        {
            std::string_view::size_type end = output.find('\n', start);
            if (end == std::string_view::npos)
            {
                end = output.size();
            }
            std::string_view line = output.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (first)
            {
                if (line != "QUICKLOGGER-RESULT 1")
                {
                    return false;
                }
                first = false;
                continue;
            }
            std::string_view::size_type colon = line.find(": ");
            if (colon != std::string_view::npos)
            {
                ResultLine& added = lines->emplace_back();
                added.key = std::string(line.substr(0, colon));
                added.value = std::string(line.substr(colon + 2));
            }
        }
        return !first;
    }

    static const std::string* FindValue(const std::vector<ResultLine>& lines, std::string_view key)
    {
        for (const ResultLine& line : lines)
        {
            if (line.key == key)
            {
                return &line.value;
            }
        }
        return nullptr;
    }

    static int ParseCount(const std::string& text)
    {
        if (text.empty() || text.size() > 6 || text.find_first_not_of("0123456789") != std::string::npos)
        {
            return 0;
        }
        return std::atoi(text.c_str());
    }

    std::vector<std::string> UpstreamListNetsArguments(const Upstream& upstream)
    {
        return UpstreamSshArguments(upstream, "list-nets");
    }

    std::vector<std::string> UpstreamExportArguments(const Upstream& upstream, const std::string& net_name,
                                                     bool sessions)
    {
        return UpstreamSshArguments(
            upstream, std::string(sessions ? "export-sessions " : "export-net ") + QuotedCommandWord(net_name));
    }

    std::vector<std::string> UpstreamFetchArguments(const Upstream& upstream,
                                                    const std::vector<std::string>& remote_names,
                                                    const std::string& local_dir)
    {
        std::vector<std::string> arguments;
        arguments.reserve(remote_names.size() + 8);
        arguments.emplace_back("-o");
        arguments.emplace_back("BatchMode=yes");
        arguments.emplace_back("-o");
        arguments.emplace_back("ConnectTimeout=" + std::to_string(kUpstreamConnectTimeoutSeconds));
        arguments.emplace_back("-P");
        arguments.emplace_back(std::to_string(upstream.port));
        arguments.emplace_back("--");
        std::string prefix = upstream.user + "@" + upstream.host + ":/exports/";
        for (const std::string& name : remote_names)
        {
            arguments.emplace_back(prefix + name);
        }
        arguments.emplace_back(local_dir);
        return arguments;
    }

    // The sentence for an upstream that answered, but not with what was asked.
    static PullResult NotAsExpected(const Upstream& upstream, const std::vector<ResultLine>& lines, const char* what)
    {
        const std::string* status = FindValue(lines, "status");
        const std::string* message = FindValue(lines, "message");
        // An upstream that doesn't know the command says so as an error.
        if (status != nullptr && *status == "error" && message != nullptr &&
            message->find("Unknown command") != std::string::npos)
        {
            return Failed(upstream.host + " is a QuickLogger that can't " + what + " (it needs a newer version).");
        }
        if (status != nullptr && *status == "refused")
        {
            return Failed(upstream.host + " refused: " + (message == nullptr ? "no reason given." : *message));
        }
        return Failed(upstream.host + " couldn't " + what + ".");
    }

    PullResult DecideListNetsResult(const Upstream& upstream, const ProgramResult& run)
    {
        if (!run.started || run.stopped || run.exit_status == 255)
        {
            return Failed(UpstreamFailureMessage(upstream, run, "Couldn't ask", false));
        }
        std::vector<ResultLine> lines;
        if (!ReadResultLines(run.output, &lines))
        {
            return Failed("The upstream isn't a QuickLogger, or is a different version.");
        }
        const std::string* status = FindValue(lines, "status");
        if (status == nullptr || *status != "ok")
        {
            return NotAsExpected(upstream, lines, "list its nets");
        }
        PullResult result;
        result.kind = PullResultKind::kNets;
        // A `net:` line starts a net; `service:` and `sessions:` follow it.
        for (const ResultLine& line : lines)
        {
            if (line.key == "net")
            {
                UpstreamNet& net = result.nets.emplace_back();
                net.name = line.value;
            }
            else if (!result.nets.empty() && line.key == "service")
            {
                result.nets.back().service = line.value;
            }
            else if (!result.nets.empty() && line.key == "sessions")
            {
                result.nets.back().sessions = ParseCount(line.value);
            }
        }
        return result;
    }

    PullResult DecideExportResult(const Upstream& upstream, const ProgramResult& run, const std::string& net_name,
                                  const std::string& extension)
    {
        if (!run.started || run.stopped || run.exit_status == 255)
        {
            return Failed(UpstreamFailureMessage(upstream, run, "Couldn't ask", false));
        }
        std::vector<ResultLine> lines;
        if (!ReadResultLines(run.output, &lines))
        {
            return Failed("The upstream isn't a QuickLogger, or is a different version.");
        }
        const std::string* status = FindValue(lines, "status");
        if (status != nullptr && *status == "no-match")
        {
            return Failed(upstream.host + " has no net named " + net_name + " any more.");
        }
        if (status == nullptr || *status != "ok")
        {
            return NotAsExpected(upstream, lines, "export that net");
        }
        PullResult result;
        result.kind = PullResultKind::kFiles;
        const std::string* net = FindValue(lines, "net");
        result.net = net == nullptr ? net_name : *net;
        const std::string* not_sent = FindValue(lines, "not-sent");
        result.not_sent = not_sent == nullptr ? 0 : ParseCount(*not_sent);
        for (const ResultLine& line : lines)
        {
            if (line.key != "file")
            {
                continue;
            }
            const std::string& name = line.value;
            if (!IsAllowedImportName(name) || name.size() <= extension.size() ||
                name.compare(name.size() - extension.size(), extension.size(), extension) != 0)
            {
                return Failed(upstream.host + " named a file that isn't one QuickLogger exports.");
            }
            result.files.push_back(name);
        }
        return result;
    }

    PullResult PullNetList(const Upstream& upstream, const std::atomic<bool>* cancel)
    {
        std::string ssh = FindProgramOnPath("ssh");
        if (ssh.empty())
        {
            return Failed(kNoSshMessage);
        }
        ProgramResult run = RunProgram(ssh, UpstreamListNetsArguments(upstream), kUpstreamRunTimeoutSeconds, cancel);
        return DecideListNetsResult(upstream, run);
    }

    PullResult PullNetFiles(const Upstream& upstream, const std::string& net_name, bool sessions,
                            const std::string& local_dir, const std::atomic<bool>* cancel)
    {
        std::string scp = FindProgramOnPath("scp");
        std::string ssh = FindProgramOnPath("ssh");
        if (scp.empty() || ssh.empty())
        {
            return Failed(kNoSshMessage);
        }
        ProgramResult export_run =
            RunProgram(ssh, UpstreamExportArguments(upstream, net_name, sessions), kUpstreamRunTimeoutSeconds, cancel);
        PullResult exported = DecideExportResult(upstream, export_run, net_name, sessions ? ".qlsession" : ".qlnet");
        if (exported.kind != PullResultKind::kFiles || exported.files.empty())
        {
            return exported;
        }
        if (!EnsureDirectory(local_dir))
        {
            return Failed("Couldn't make the folder " + local_dir + ".");
        }

        std::vector<std::string> remote_names = std::move(exported.files);
        exported.files.clear();
        for (std::size_t start = 0; start < remote_names.size(); start += kPullFilesPerCopy)
        {
            std::size_t end = std::min(remote_names.size(), start + kPullFilesPerCopy);
            std::vector<std::string> chunk(remote_names.begin() + static_cast<std::ptrdiff_t>(start),
                                           remote_names.begin() + static_cast<std::ptrdiff_t>(end));
            ProgramResult copy =
                RunProgram(scp, UpstreamFetchArguments(upstream, chunk, local_dir), kUpstreamRunTimeoutSeconds, cancel);
            if (!copy.started || copy.stopped || copy.exit_status != 0)
            {
                return Failed(UpstreamFailureMessage(upstream, copy, "Couldn't copy files from", false));
            }
        }
        exported.files.reserve(remote_names.size());
        for (const std::string& name : remote_names)
        {
            exported.files.push_back(local_dir + "/" + name);
        }
        return exported;
    }

}  // namespace ql
