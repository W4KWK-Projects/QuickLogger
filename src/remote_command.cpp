#include "remote_command.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <exception>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "date_utils.hpp"
#include "file_export.hpp"
#include "net_slice.hpp"
#include "sftp_paths.hpp"
#include "text_utils.hpp"
#include "version.hpp"

namespace ql
{

    // A character a word may hold outside quotes.
    static bool IsPlainWordCharacter(char c)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            return true;
        }
        switch (c)
        {
            case '.':
            case '_':
            case '-':
            case '/':
            case '+':
            case '=':
            case ':':
            case ',':
            case '@':
            case '%':
                return true;
            default:
                return false;
        }
    }

    bool SplitCommandLine(std::string_view line, std::vector<std::string>* words, std::string* error)
    {
        words->clear();
        std::string word;
        bool in_word = false;
        bool in_quotes = false;
        // The last word was a quoted string, which has to end the word.
        bool after_quote = false;
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            char c = line[i];
            unsigned char byte = static_cast<unsigned char>(c);
            if ((byte < 0x20 && !(c == '\t' && !in_quotes)) || byte == 0x7f)
            {
                *error = "The command has a control character in it.";
                return false;
            }
            if (in_quotes)
            {
                if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\'))
                {
                    word += line[i + 1];
                    ++i;
                }
                else if (c == '"')
                {
                    in_quotes = false;
                    after_quote = true;
                }
                else
                {
                    word += c;
                }
                continue;
            }
            if (c == ' ' || c == '\t')
            {
                if (in_word)
                {
                    words->emplace_back(std::move(word));
                    word.clear();
                    in_word = false;
                }
                after_quote = false;
                continue;
            }
            if (after_quote)
            {
                *error = "A quoted string has to be a word of its own.";
                return false;
            }
            if (c == '"')
            {
                if (in_word)
                {
                    *error = "A quoted string has to be a word of its own.";
                    return false;
                }
                in_word = true;
                in_quotes = true;
                continue;
            }
            if (byte >= 0x80)
            {
                *error = "The command has a character that isn't allowed outside quotes.";
                return false;
            }
            if (!IsPlainWordCharacter(c))
            {
                *error = std::string("The command has a character that isn't allowed outside quotes: ") + c;
                return false;
            }
            in_word = true;
            word += c;
        }
        if (in_quotes)
        {
            *error = "A quoted string isn't closed.";
            return false;
        }
        if (in_word)
        {
            words->emplace_back(std::move(word));
        }
        return true;
    }

    bool ParseRemoteCommand(std::string_view line, RemoteCommand* command, std::string* error)
    {
        std::vector<std::string> words;
        if (!SplitCommandLine(line, &words, error))
        {
            return false;
        }
        if (words.empty())
        {
            *error = "No command given.";
            return false;
        }
        RemoteCommand parsed;
        if (words[0] == "version")
        {
            if (words.size() != 1)
            {
                *error = "version takes no arguments.";
                return false;
            }
            parsed.kind = RemoteCommandKind::kVersion;
            *command = std::move(parsed);
            return true;
        }
        if (words[0] != "import-session")
        {
            *error = "Unknown command: " + words[0] + ". QuickLogger runs only import-session and version.";
            return false;
        }

        parsed.kind = RemoteCommandKind::kImportSession;
        std::size_t next = 1;
        if (next < words.size() && words[next] == "--confirm-net")
        {
            if (next + 1 >= words.size())
            {
                *error = "--confirm-net needs a net name.";
                return false;
            }
            parsed.has_confirm_net = true;
            parsed.confirm_net = words[next + 1];
            next += 2;
        }
        if (next >= words.size())
        {
            *error = "Usage: import-session [--confirm-net \"<net name>\"] <file>";
            return false;
        }
        if (words[next].size() > 1 && words[next][0] == '-' && words[next][1] == '-')
        {
            *error = "Unknown option: " + words[next] + ".";
            return false;
        }
        if (next + 1 != words.size())
        {
            *error = "import-session takes one file.";
            return false;
        }
        parsed.file = words[next];
        *command = std::move(parsed);
        return true;
    }

    // One result's lines, kept in the documented order whatever order
    // they're filled in.
    struct RemoteResultFields
    {
        std::string status;
        std::string net;
        bool has_net = false;
        std::int64_t net_id = 0;
        std::string session;
        std::string message;
    };

    // `value` on one line: every control character (a newline in a net name)
    // becomes a space.
    static void AppendLine(std::string* output, std::string_view key, std::string_view value)
    {
        output->append(key.data(), key.size());
        output->append(": ");
        std::size_t start = output->size();
        output->append(value.data(), value.size());
        for (std::size_t i = start; i < output->size(); ++i)
        {
            if (static_cast<unsigned char>((*output)[i]) < 0x20 || (*output)[i] == '\x7f')
            {
                (*output)[i] = ' ';
            }
        }
        output->push_back('\n');
    }

    static RemoteCommandResult MakeResult(const RemoteResultFields& fields, int exit_status)
    {
        RemoteCommandResult result;
        result.exit_status = exit_status;
        result.output = "QUICKLOGGER-RESULT " + std::to_string(kRemoteCommandInterface) + "\n";
        AppendLine(&result.output, "status", fields.status);
        if (fields.has_net)
        {
            AppendLine(&result.output, "net", fields.net);
            AppendLine(&result.output, "net-id", std::to_string(fields.net_id));
        }
        if (!fields.session.empty())
        {
            AppendLine(&result.output, "session", fields.session);
        }
        if (!fields.message.empty())
        {
            AppendLine(&result.output, "message", fields.message);
        }
        return result;
    }

    static RemoteCommandResult Simple(const char* status, const std::string& message, int exit_status)
    {
        RemoteResultFields fields;
        fields.status = status;
        fields.message = message;
        return MakeResult(fields, exit_status);
    }

    RemoteCommandResult RemoteCommandParseError(const std::string& error)
    {
        return Simple("error", error, kRemoteExitError);
    }

    // A session as the result names it: its start in UTC, 24-hour, as
    // "2026-09-14 23:30 UTC"; just its date for one logged before start
    // times were kept.
    static std::string SessionLabel(const NetInstance& session)
    {
        if (session.started_at <= 0)
        {
            return session.instance_date;
        }
        std::tm utc = UtcTime(static_cast<std::time_t>(session.started_at));
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M UTC", &utc);
        return text;
    }

    static const char* ServiceName(NetService service)
    {
        return service == NetService::kGmrs ? "GMRS" : "Amateur Radio";
    }

    static bool CompareNetNames(const Net* left, const Net* right)
    {
        std::string left_upper = ToUpperAscii(left->name);
        std::string right_upper = ToUpperAscii(right->name);
        if (left_upper != right_upper)
        {
            return left_upper < right_upper;
        }
        return left->name < right->name;
    }

    // The outcome of import-session once the file has been read.
    class SessionImport
    {
    public:
        SessionImport(Database* db, const NetSlice& slice, std::int64_t now) : db_(db), slice_(slice), now_(now)
        {
            fields_.session = SessionLabel(slice_.instances[0]);
        }

        RemoteCommandResult Run(const RemoteCommand& command)
        {
            if (slice_.net.is_ad_hoc)
            {
                if (command.has_confirm_net)
                {
                    return Finish("refused",
                                  "An ad hoc session goes into a new ad hoc net; --confirm-net is only "
                                  "for a recurring net's session.",
                                  kRemoteExitRefused);
                }
                return ImportAdHoc();
            }

            // Only nets on the session's own service: an Amateur Radio
            // session never goes into a GMRS net, or the other way round.
            std::vector<Net> nets = db_->GetAllNets();
            std::vector<const Net*> recurring;
            std::vector<const Net*> other_service;
            recurring.reserve(nets.size());
            for (const Net& net : nets)
            {
                if (net.is_ad_hoc)
                {
                    continue;
                }
                if (net.service == slice_.net.service)
                {
                    recurring.emplace_back(&net);
                }
                else
                {
                    other_service.emplace_back(&net);
                }
            }

            if (command.has_confirm_net)
            {
                const Net* confirmed = FindNamed(recurring, command.confirm_net);
                const Net* elsewhere = confirmed == nullptr ? FindNamed(other_service, command.confirm_net) : nullptr;
                if (elsewhere != nullptr)
                {
                    SetNet(*elsewhere);
                    return Finish("refused",
                                  "\"" + elsewhere->name + "\" is on " + ServiceName(elsewhere->service) +
                                      " and this session was logged on " + ServiceName(slice_.net.service) +
                                      ", so it wasn't imported.",
                                  kRemoteExitRefused);
                }
                if (confirmed == nullptr)
                {
                    return Finish("no-match", "No net here is named \"" + command.confirm_net + "\".",
                                  kRemoteExitNoMatch);
                }
                if (!NetNamesAreTheSame(confirmed->name, slice_.net.name) &&
                    !NetNamesLookAlike(slice_.net.name, confirmed->name, /*alike_if_unsure=*/false))
                {
                    SetNet(*confirmed);
                    return Finish("refused",
                                  "\"" + confirmed->name + "\" doesn't look like \"" + slice_.net.name +
                                      "\", the net this session was logged in, so it wasn't imported.",
                                  kRemoteExitRefused);
                }
                return ImportInto(*confirmed);
            }

            const Net* same = FindNamed(recurring, slice_.net.name);
            if (same != nullptr)
            {
                return ImportInto(*same);
            }

            std::vector<const Net*> look_alikes;
            for (const Net* net : recurring)
            {
                if (NetNamesLookAlike(slice_.net.name, net->name, /*alike_if_unsure=*/false))
                {
                    look_alikes.emplace_back(net);
                }
            }
            if (look_alikes.empty())
            {
                return Finish("no-match", "No net here looks like \"" + slice_.net.name + "\".", kRemoteExitNoMatch);
            }
            std::sort(look_alikes.begin(), look_alikes.end(), CompareNetNames);
            SetNet(*look_alikes[0]);
            std::string message = "The session was logged as \"" + slice_.net.name + "\"; confirm \"" +
                                  look_alikes[0]->name + "\" to import it there.";
            if (look_alikes.size() > 1)
            {
                message += " " + std::to_string(look_alikes.size() - 1) + " other net" +
                           (look_alikes.size() > 2 ? "s look" : " looks") + " alike too.";
            }
            return Finish("needs-confirmation", message, kRemoteExitNeedsConfirmation);
        }

        // Imported, or found already imported: the file can go.
        bool Done() const
        {
            return done_;
        }

    private:
        static const Net* FindNamed(const std::vector<const Net*>& nets, const std::string& name)
        {
            for (const Net* net : nets)
            {
                if (NetNamesAreTheSame(net->name, name))
                {
                    return net;
                }
            }
            return nullptr;
        }

        void SetNet(const Net& net)
        {
            fields_.has_net = true;
            fields_.net = net.name;
            fields_.net_id = net.id;
        }

        RemoteCommandResult Finish(const char* status, const std::string& message, int exit_status)
        {
            fields_.status = status;
            fields_.message = message;
            return MakeResult(fields_, exit_status);
        }

        RemoteCommandResult ImportInto(const Net& net)
        {
            SetNet(net);
            const NetInstance& source = slice_.instances[0];
            std::string error;
            // The check and the import under one write lock, so two pushes of
            // the same session at once can't both import it.
            Database::WriteTransaction transaction(db_);
            if (db_->HasNetInstance(net.id, source.instance_date, source.started_at))
            {
                done_ = true;
                return Finish("already-imported", net.name + " already has this session.", kRemoteExitOk);
            }
            std::int64_t instance_id = ApplySessionSlice(db_, slice_, net.id, &error);
            if (instance_id == 0)
            {
                return Finish("error", error, kRemoteExitError);
            }
            transaction.Commit();
            done_ = true;
            std::string message = "Imported the session into " + net.name;
            if (net.name != slice_.net.name)
            {
                message += " (logged as \"" + slice_.net.name + "\")";
            }
            return Finish("imported", message + ".", kRemoteExitOk);
        }

        RemoteCommandResult ImportAdHoc()
        {
            const NetInstance& source = slice_.instances[0];
            std::string error;
            Database::WriteTransaction transaction(db_);
            std::optional<NetInstance> existing =
                db_->FindAdHocSession(slice_.net.name, source.instance_date, source.started_at);
            if (existing.has_value())
            {
                fields_.has_net = true;
                fields_.net = slice_.net.name;
                fields_.net_id = existing->net_id;
                done_ = true;
                return Finish("already-imported", "The ad hoc net " + slice_.net.name + " already has this session.",
                              kRemoteExitOk);
            }
            std::int64_t net_id = 0;
            std::int64_t instance_id = ApplyAdHocSessionSlice(db_, slice_, now_, &net_id, &error);
            if (instance_id == 0)
            {
                return Finish("error", error, kRemoteExitError);
            }
            transaction.Commit();
            fields_.has_net = true;
            fields_.net = slice_.net.name;
            fields_.net_id = net_id;
            done_ = true;
            return Finish("imported", "Imported the session as a new ad hoc net, " + slice_.net.name + ".",
                          kRemoteExitOk);
        }

        Database* db_;
        const NetSlice& slice_;
        std::int64_t now_;
        RemoteResultFields fields_;
        bool done_ = false;
    };

    static RemoteCommandResult ImportSession(const RemoteCommand& command, Database* db, const std::string& db_path,
                                             const std::string& username, std::int64_t now)
    {
        // A bare name is in /imports, as is the only place one may be.
        std::string client_path =
            command.file.find('/') == std::string::npos ? "/imports/" + command.file : command.file;
        SftpPath path;
        if (!ResolveSftpPath(client_path, &path) || path.area != SftpArea::kImports || path.name.empty() ||
            !IsAllowedImportName(path.name) || path.name.size() < 10 ||
            path.name.compare(path.name.size() - 10, 10, ".qlsession") != 0)
        {
            return Simple("refused", "The file has to be a .qlsession in your /imports.", kRemoteExitRefused);
        }

        std::string file_path = SessionImportsDir(db_path, username) + "/" + path.name;
        std::error_code error_code;
        std::filesystem::file_status status = std::filesystem::symlink_status(file_path, error_code);
        if (error_code || !std::filesystem::is_regular_file(status))
        {
            return Simple("refused", "There's no " + path.name + " in your /imports.", kRemoteExitRefused);
        }
        std::uintmax_t size = std::filesystem::file_size(file_path, error_code);
        if (error_code || size > kSftpMaxUploadBytes)
        {
            return Simple("refused", path.name + " is over the 25 MB limit.", kRemoteExitRefused);
        }

        std::string error;
        std::optional<NetSlice> slice = ReadSessionSliceFile(file_path, &error);
        if (!slice.has_value())
        {
            return Simple("refused", path.name + " can't be read: " + error, kRemoteExitRefused);
        }

        SessionImport import(db, *slice, now);
        RemoteCommandResult result = import.Run(command);
        if (import.Done())
        {
            std::filesystem::remove(file_path, error_code);
        }
        return result;
    }

    RemoteCommandResult RunRemoteCommand(const RemoteCommand& command, Database* db, const std::string& db_path,
                                         const std::string& username, bool view_only, std::int64_t now)
    {
        if (view_only)
        {
            return Simple("refused", "View-only users can't run commands.", kRemoteExitRefused);
        }
        try
        {
            if (command.kind == RemoteCommandKind::kVersion)
            {
                RemoteCommandResult result;
                result.output = "QUICKLOGGER-RESULT " + std::to_string(kRemoteCommandInterface) + "\n";
                AppendLine(&result.output, "status", "ok");
                AppendLine(&result.output, "version", QuickLoggerVersion());
                AppendLine(&result.output, "interface", std::to_string(kRemoteCommandInterface));
                return result;
            }
            return ImportSession(command, db, db_path, username, now);
        }
        catch (const std::exception& e)
        {
            return Simple("error", std::string("Import failed: ") + e.what(), kRemoteExitError);
        }
    }

}  // namespace ql
