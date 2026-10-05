#include "mosh_bridge.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>

namespace ql
{

    // What a mosh client run with --experimental-remote-ip=remote puts
    // before mosh-server, word for word: a shell command printing the
    // server's SSH_CONNECTION. Recognized as a whole, never run.
    static constexpr std::string_view kSshConnectionProbe =
        "sh -c '[ -n \"$SSH_CONNECTION\" ] && printf \"\\nMOSH SSH_CONNECTION %s\\n\" \"$SSH_CONNECTION\"' ; ";

    static constexpr std::string_view kMoshServerName = "mosh-server";

    // A character a word may hold outside quotes.
    static bool IsPlainWordChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '/' || c == '=' || c == ':' || c == ',' || c == '@' || c == '+' || c == '%' || c == '-';
    }

    // The line without the SSH_CONNECTION probe, and whether it had one.
    static std::string_view WithoutProbe(std::string_view line, bool* had_probe)
    {
        *had_probe = line.substr(0, kSshConnectionProbe.size()) == kSshConnectionProbe;
        return *had_probe ? line.substr(kSshConnectionProbe.size()) : line;
    }

    // Splits shell words as a POSIX shell would for the subset a mosh
    // client writes: plain characters, 'single-quoted' text and \x. Refuses
    // anything else (variables, globs, ; | & and so on) rather than
    // guessing.
    static bool SplitShellWords(std::string_view line, std::vector<std::string>* words, std::string* error)
    {
        words->clear();
        std::string word;
        bool in_word = false;
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            char c = line[i];
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t')
            {
                *error = "The command has a control character in it.";
                return false;
            }
            if (c == ' ' || c == '\t')
            {
                if (in_word)
                {
                    words->push_back(word);
                    word.clear();
                    in_word = false;
                }
                continue;
            }
            in_word = true;
            if (c == '\'')
            {
                std::size_t end = line.find('\'', i + 1);
                if (end == std::string_view::npos)
                {
                    *error = "The command has an unclosed quote.";
                    return false;
                }
                word.append(line.substr(i + 1, end - i - 1));
                i = end;
            }
            else if (c == '\\' && i + 1 < line.size())
            {
                word.push_back(line[++i]);
            }
            else if (IsPlainWordChar(c))
            {
                word.push_back(c);
            }
            else
            {
                *error = std::string("The command has a '") + c + "' in it, which isn't allowed.";
                return false;
            }
        }
        if (in_word)
        {
            words->push_back(word);
        }
        return true;
    }

    static bool AllOf(const std::string& text, bool (*allowed)(char), std::size_t max_size)
    {
        if (text.empty() || text.size() > max_size)
        {
            return false;
        }
        for (char c : text)
        {
            if (!allowed(c))
            {
                return false;
            }
        }
        return true;
    }

    static bool IsDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    static bool IsPortChar(char c)
    {
        return IsDigit(c) || c == ':';
    }

    static bool IsAddressChar(char c)
    {
        return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '%' ||
               c == '-';
    }

    static bool IsLocaleValueChar(char c)
    {
        return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '@' ||
               c == '-';
    }

    // LANG, LANGUAGE or LC_<letters>=<a locale name>: what a mosh client
    // passes with -l.
    static bool IsLocaleSetting(const std::string& setting)
    {
        std::string::size_type equals = setting.find('=');
        if (equals == std::string::npos)
        {
            return false;
        }
        std::string name = setting.substr(0, equals);
        bool name_ok = name == "LANG" || name == "LANGUAGE";
        if (!name_ok && name.size() > 3 && name.compare(0, 3, "LC_") == 0)
        {
            name_ok = true;
            for (std::size_t i = 3; i < name.size(); ++i)
            {
                name_ok = name_ok && name[i] >= 'A' && name[i] <= 'Z';
            }
        }
        return name_ok && AllOf(setting.substr(equals + 1), IsLocaleValueChar, 64);
    }

    static std::string_view Basename(std::string_view path)
    {
        std::string_view::size_type slash = path.rfind('/');
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    bool IsMoshServerCommandLine(std::string_view line)
    {
        bool had_probe = false;
        std::string_view rest = WithoutProbe(line, &had_probe);
        std::string_view::size_type space = rest.find(' ');
        std::string_view program = rest.substr(0, space);
        // Quoted, as a path with spaces might be.
        if (program.size() >= 2 && program.front() == '\'' && program.back() == '\'')
        {
            program = program.substr(1, program.size() - 2);
        }
        return Basename(program) == kMoshServerName;
    }

    bool ParseMoshServerCommand(std::string_view line, MoshServerRequest* request, std::string* error)
    {
        *request = MoshServerRequest();
        // Termius ends its command with a newline.
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        {
            line.remove_suffix(1);
        }
        std::vector<std::string> words;
        if (!SplitShellWords(WithoutProbe(line, &request->report_ssh_connection), &words, error))
        {
            return false;
        }
        if (words.size() < 2 || Basename(words[0]) != kMoshServerName || words[1] != "new")
        {
            *error = "Only \"mosh-server new\" can be run.";
            return false;
        }
        for (std::size_t i = 2; i < words.size(); ++i)
        {
            const std::string& option = words[i];
            if (option == "--")
            {
                break;  // The client's own command: never run.
            }
            if (option == "-s" || option == "-v")
            {
                request->options.push_back(option);
                continue;
            }
            // Termius sends a bare -l (no NAME=VALUE) when its locale field is empty:
            // "mosh-server new -s -l -p 60000:61000". Read as the locale Termius
            // itself documents as its default, rather than refusing the line.
            if (option == "-l" && (i + 1 >= words.size() || words[i + 1].compare(0, 1, "-") == 0))
            {
                request->options.push_back(option);
                request->options.push_back("LANG=en_US.UTF-8");
                continue;
            }
            bool takes_value = option == "-i" || option == "-p" || option == "-c" || option == "-l";
            if (!takes_value)
            {
                *error = "mosh-server's option " + option + " isn't allowed.";
                return false;
            }
            if (i + 1 >= words.size())
            {
                *error = "mosh-server's option " + option + " needs a value.";
                return false;
            }
            const std::string& value = words[++i];
            bool value_ok = (option == "-i" && AllOf(value, IsAddressChar, 64)) ||
                            (option == "-p" && AllOf(value, IsPortChar, 11)) ||
                            (option == "-c" && AllOf(value, IsDigit, 3)) || (option == "-l" && IsLocaleSetting(value));
            if (!value_ok)
            {
                *error = "mosh-server's option " + option + " has a value that isn't allowed.";
                return false;
            }
            request->options.push_back(option);
            request->options.push_back(value);
        }
        return true;
    }

    std::vector<std::string> MoshServerArgv(const std::string& mosh_server_path, const MoshServerRequest& request,
                                            const std::string& self_path)
    {
        std::vector<std::string> argv{mosh_server_path, "new"};
        argv.insert(argv.end(), request.options.begin(), request.options.end());
        argv.push_back("--");
        argv.push_back(self_path);
        argv.push_back("--mosh-session");
        return argv;
    }

    static std::string TokenDir(const std::string& dir)
    {
        return dir + "/mosh";
    }

    static bool IsTokenName(const std::string& token)
    {
        if (token.size() != 32)
        {
            return false;
        }
        for (char c : token)
        {
            if (!IsDigit(c) && !(c >= 'a' && c <= 'f'))
            {
                return false;
            }
        }
        return true;
    }

    // A token file's time, username and key (a third line; none, 0, in one
    // from before it was kept); false if it can't be read.
    static bool ReadToken(const std::filesystem::path& path, std::int64_t* made_at, std::string* username,
                          std::int64_t* key_id)
    {
        std::ifstream in(path);
        std::string time_line;
        if (!std::getline(in, time_line) || !std::getline(in, *username) || time_line.empty() ||
            !AllOf(time_line, IsDigit, 19) || username->empty())
        {
            return false;
        }
        *made_at = std::stoll(time_line);
        std::string key_line;
        *key_id =
            std::getline(in, key_line) && !key_line.empty() && AllOf(key_line, IsDigit, 18) ? std::stoll(key_line) : 0;
        return true;
    }

    // Removes tokens too old to be used, and anything else in the folder.
    static void RemoveStaleTokens(const std::string& token_dir, std::int64_t now)
    {
        std::error_code code;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(token_dir, code))
        {
            std::int64_t made_at = 0;
            std::string username;
            std::int64_t key_id = 0;
            if (!IsTokenName(entry.path().filename().string()) ||
                !ReadToken(entry.path(), &made_at, &username, &key_id) || now - made_at > kMoshTokenSeconds)
            {
                std::filesystem::remove(entry.path(), code);
            }
        }
    }

    std::string CreateMoshToken(const std::string& dir, const std::string& username, std::int64_t now,
                                std::string* error, std::int64_t key_id)
    {
        std::string token_dir = TokenDir(dir);
        std::error_code code;
        std::filesystem::create_directories(token_dir, code);
        std::filesystem::permissions(token_dir, std::filesystem::perms::owner_all, code);
        RemoveStaleTokens(token_dir, now);

        std::random_device random;
        static const char kHex[] = "0123456789abcdef";
        std::string token;
        for (int i = 0; i < 4; ++i)
        {
            std::uint32_t bits = random();
            for (int j = 0; j < 8; ++j)
            {
                token.push_back(kHex[bits & 0xf]);
                bits >>= 4;
            }
        }
        std::filesystem::path path = std::filesystem::path(token_dir) / token;
        {
            std::ofstream out(path, std::ios::trunc);
            out << now << "\n" << username << "\n" << key_id << "\n";
            if (!out)
            {
                *error = "Couldn't write " + path.string() + ".";
                return "";
            }
        }
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     code);
        return token;
    }

    bool ConsumeMoshToken(const std::string& dir, const std::string& token, std::int64_t now, std::string* username,
                          std::int64_t* key_id)
    {
        if (!IsTokenName(token))
        {
            return false;
        }
        std::filesystem::path path = std::filesystem::path(TokenDir(dir)) / token;
        std::int64_t made_at = 0;
        std::int64_t key = 0;
        bool read = ReadToken(path, &made_at, username, &key);
        if (key_id != nullptr)
        {
            *key_id = key;
        }
        std::error_code code;
        bool removed = std::filesystem::remove(path, code);
        // Removed by this process, so no other can use it.
        return read && removed && made_at <= now && now - made_at <= kMoshTokenSeconds;
    }

}  // namespace ql
