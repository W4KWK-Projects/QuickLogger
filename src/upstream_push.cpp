#include "upstream_push.hpp"

#include <cctype>

namespace ql
{

    const char* const kNoSshMessage = "Pushing needs ssh and scp, which aren't installed here.";

    static bool IsPlainNameCharacter(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '_';
    }

    static bool IsPlainName(std::string_view name)
    {
        if (name.empty() || name.size() > 253 || name[0] == '-')
        {
            return false;
        }
        for (char c : name)
        {
            if (!IsPlainNameCharacter(c))
            {
                return false;
            }
        }
        return true;
    }

    bool IsValidUpstreamHost(std::string_view host)
    {
        return IsPlainName(host);
    }

    bool IsValidUpstreamUser(std::string_view user)
    {
        return IsPlainName(user) && user.size() <= 64;
    }

    // The options scp and ssh share: no prompts, a bounded wait to connect.
    static void AddCommonOptions(std::vector<std::string>* arguments)
    {
        arguments->emplace_back("-o");
        arguments->emplace_back("BatchMode=yes");
        arguments->emplace_back("-o");
        arguments->emplace_back("ConnectTimeout=" + std::to_string(kUpstreamConnectTimeoutSeconds));
    }

    std::vector<std::string> UpstreamScpArguments(const Upstream& upstream, const std::string& local_path,
                                                  const std::string& remote_name)
    {
        std::vector<std::string> arguments;
        arguments.reserve(9);
        AddCommonOptions(&arguments);
        arguments.emplace_back("-P");
        arguments.emplace_back(std::to_string(upstream.port));
        arguments.emplace_back("--");
        arguments.emplace_back(local_path);
        arguments.emplace_back(upstream.user + "@" + upstream.host + ":/imports/" + remote_name);
        return arguments;
    }

    // `text` as one double-quoted word for import-session's command line.
    static std::string Quoted(const std::string& text)
    {
        std::string quoted = "\"";
        for (char c : text)
        {
            if (c == '"' || c == '\\')
            {
                quoted += '\\';
                quoted += c;
            }
            else if (static_cast<unsigned char>(c) < 0x20 || c == '\x7f')
            {
                quoted += ' ';
            }
            else
            {
                quoted += c;
            }
        }
        quoted += '"';
        return quoted;
    }

    std::vector<std::string> UpstreamImportArguments(const Upstream& upstream, const std::string& remote_name,
                                                     const std::string& confirm_net)
    {
        std::vector<std::string> arguments;
        arguments.reserve(11);
        AddCommonOptions(&arguments);
        arguments.emplace_back("-p");
        arguments.emplace_back(std::to_string(upstream.port));
        arguments.emplace_back("-l");
        arguments.emplace_back(upstream.user);
        arguments.emplace_back("--");
        arguments.emplace_back(upstream.host);
        std::string command = "import-session ";
        if (!confirm_net.empty())
        {
            command += "--confirm-net " + Quoted(confirm_net) + " ";
        }
        command += remote_name;
        arguments.emplace_back(std::move(command));
        return arguments;
    }

    ImportReply ParseImportReply(std::string_view output)
    {
        ImportReply reply;
        bool first = true;
        bool has_status = false;
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
                    return reply;
                }
                first = false;
                continue;
            }
            std::string_view::size_type colon = line.find(": ");
            if (colon == std::string_view::npos)
            {
                continue;
            }
            std::string_view key = line.substr(0, colon);
            std::string value(line.substr(colon + 2));
            if (key == "status")
            {
                has_status = true;
                if (value == "imported")
                {
                    reply.status = ImportReplyStatus::kImported;
                }
                else if (value == "already-imported")
                {
                    reply.status = ImportReplyStatus::kAlreadyImported;
                }
                else if (value == "needs-confirmation")
                {
                    reply.status = ImportReplyStatus::kNeedsConfirmation;
                }
                else if (value == "no-match")
                {
                    reply.status = ImportReplyStatus::kNoMatch;
                }
                else if (value == "refused")
                {
                    reply.status = ImportReplyStatus::kRefused;
                }
                else if (value == "error")
                {
                    reply.status = ImportReplyStatus::kError;
                }
                else
                {
                    reply.status = ImportReplyStatus::kUnreadable;
                    return reply;
                }
            }
            else if (key == "net")
            {
                reply.net = std::move(value);
            }
            else if (key == "net-id")
            {
                reply.net_id = value.empty() || value.size() > 18 ||
                                       value.find_first_not_of("0123456789") != std::string::npos
                                   ? 0
                                   : std::stoll(value);
            }
            else if (key == "session")
            {
                reply.session = std::move(value);
            }
            else if (key == "message")
            {
                reply.message = std::move(value);
            }
        }
        if (!has_status || (reply.status == ImportReplyStatus::kNeedsConfirmation && reply.net.empty()))
        {
            reply.status = ImportReplyStatus::kUnreadable;
        }
        return reply;
    }

    static bool Contains(const std::string& text, const char* part)
    {
        return text.find(part) != std::string::npos;
    }

    static PushResult Failed(const std::string& message)
    {
        PushResult result;
        result.kind = PushResultKind::kFailed;
        result.message = message;
        return result;
    }

    // Why scp or ssh failed, from what it printed.
    static PushResult ConnectionFailure(const Upstream& upstream, const ProgramResult& run, const char* fallback)
    {
        const std::string& host = upstream.host;
        if (!run.started)
        {
            return Failed(kNoSshMessage);
        }
        if (run.stopped)
        {
            return Failed("Couldn't reach " + host + ".");
        }
        const std::string& errors = run.errors;
        if (Contains(errors, "Host key verification failed") || Contains(errors, "IDENTIFICATION HAS CHANGED") ||
            Contains(errors, "No matching host key") || Contains(errors, "host key is known"))
        {
            return Failed(host + "'s host key isn't known here or has changed; log in once with ssh to check it.");
        }
        if (Contains(errors, "Permission denied"))
        {
            return Failed(host + " refused your key.");
        }
        if (Contains(errors, "Could not resolve hostname") || Contains(errors, "Name or service not known"))
        {
            return Failed("Couldn't find " + host + ".");
        }
        if (Contains(errors, "View-only users"))
        {
            return Failed("Your user on " + host + " is view-only.");
        }
        if (Contains(errors, "over the 25 MB limit"))
        {
            return Failed("The session is too big for " + host + ".");
        }
        if (Contains(errors, "is full"))
        {
            return Failed("Your /imports on " + host + " is full.");
        }
        if (Contains(errors, "Connection refused") || Contains(errors, "timed out") ||
            Contains(errors, "No route to host") || Contains(errors, "unreachable") ||
            Contains(errors, "Connection closed") || Contains(errors, "Connection reset") ||
            Contains(errors, "lost connection"))
        {
            return Failed("Couldn't reach " + host + ".");
        }
        return Failed(std::string(fallback) + " " + host + ".");
    }

    PushResult DecidePushResult(const Upstream& upstream, const ProgramResult& copy, const ProgramResult& import,
                                const std::string& session_net)
    {
        const std::string& host = upstream.host;
        if (!copy.started || copy.stopped || copy.exit_status != 0)
        {
            return ConnectionFailure(upstream, copy, "Couldn't copy the session to");
        }
        if (!import.started || import.stopped || import.exit_status == 255)
        {
            return ConnectionFailure(upstream, import, "Couldn't run the import on");
        }

        ImportReply reply = ParseImportReply(import.output);
        PushResult result;
        switch (reply.status)
        {
            case ImportReplyStatus::kImported:
                result.kind = PushResultKind::kPushed;
                result.message = "Pushed to " + reply.net + " on " + host + ".";
                return result;
            case ImportReplyStatus::kAlreadyImported:
                result.kind = PushResultKind::kPushed;
                result.message = host + " already had this session, in " + reply.net + ".";
                return result;
            case ImportReplyStatus::kNeedsConfirmation:
                result.kind = PushResultKind::kNeedsConfirmation;
                result.upstream_net = reply.net;
                result.message = "Push to " + reply.net + " on " + host + "?";
                return result;
            case ImportReplyStatus::kNoMatch:
                return Failed("No net named like " + session_net + " on " + host + ".");
            case ImportReplyStatus::kRefused:
                if (Contains(reply.message, "View-only"))
                {
                    return Failed("Your user on " + host + " is view-only.");
                }
                return Failed(host + " refused the session.");
            case ImportReplyStatus::kError:
                return Failed(host + " couldn't import the session.");
            case ImportReplyStatus::kUnreadable:
                break;
        }
        return Failed("The upstream isn't a QuickLogger, or is a different version.");
    }

    PushResult PushSessionFile(const Upstream& upstream, const std::string& local_path, const std::string& remote_name,
                               const std::string& confirm_net, const std::string& session_net,
                               const std::atomic<bool>* cancel)
    {
        std::string scp = FindProgramOnPath("scp");
        std::string ssh = FindProgramOnPath("ssh");
        if (scp.empty() || ssh.empty())
        {
            return Failed(kNoSshMessage);
        }
        ProgramResult copy;
        ProgramResult import;
        // Copied again for a confirmation too, in case the first copy has
        // gone since (the upstream clears old uploads).
        copy = RunProgram(scp, UpstreamScpArguments(upstream, local_path, remote_name), kUpstreamRunTimeoutSeconds,
                          cancel);
        if (copy.started && !copy.stopped && copy.exit_status == 0)
        {
            import = RunProgram(ssh, UpstreamImportArguments(upstream, remote_name, confirm_net),
                                kUpstreamRunTimeoutSeconds, cancel);
        }
        return DecidePushResult(upstream, copy, import, session_net);
    }

    bool UpstreamToolsAvailable()
    {
        return !FindProgramOnPath("ssh").empty() && !FindProgramOnPath("scp").empty();
    }

}  // namespace ql
