#include "scp_server.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "file_export.hpp"
#include "remote_command.hpp"
#include "sftp_paths.hpp"

namespace ql
{

    // The longest control line ("C0644 <size> <name>") accepted.
    static const std::size_t kMaxScpLine = 1024;

    // How much of a file is read or written at a time.
    static const std::size_t kScpChunk = 32 * 1024;

    bool IsScpCommandLine(std::string_view line)
    {
        std::string_view::size_type start = line.find_first_not_of(" \t");
        if (start == std::string_view::npos)
        {
            return false;
        }
        std::string_view rest = line.substr(start);
        return rest.substr(0, 3) == "scp" && (rest.size() == 3 || rest[3] == ' ' || rest[3] == '\t');
    }

    bool ParseScpCommand(std::string_view line, ScpCommand* command, std::string* error)
    {
        std::vector<std::string> words;
        if (!SplitCommandLine(line, &words, error))
        {
            return false;
        }
        ScpCommand parsed;
        bool to = false;
        bool from = false;
        bool paths_only = false;
        std::vector<std::string> paths;
        for (std::size_t i = 1; i < words.size(); ++i)
        {
            const std::string& word = words[i];
            if (paths_only || word.empty() || word[0] != '-')
            {
                paths.emplace_back(word);
                continue;
            }
            if (word == "--")
            {
                paths_only = true;
            }
            else if (word == "-t")
            {
                to = true;
            }
            else if (word == "-f")
            {
                from = true;
            }
            else if (word == "-p")
            {
                parsed.preserve = true;
            }
            else if (word == "-d")
            {
                parsed.target_is_folder = true;
            }
            else if (word == "-r")
            {
                *error = "Folders can't be copied, only files.";
                return false;
            }
            else if (word != "-v" && word != "-q")
            {
                *error = "Unknown scp option: " + word + ".";
                return false;
            }
        }
        if (to == from)
        {
            *error = "scp needs one of -t and -f.";
            return false;
        }
        if (paths.size() != 1)
        {
            *error = "scp takes one path.";
            return false;
        }
        parsed.receive = to;
        parsed.path = std::move(paths[0]);
        *command = std::move(parsed);
        return true;
    }

    bool ParseScpFileLine(std::string_view line, std::uint64_t* size, std::string* name, std::string* error)
    {
        // "C" and four octal digits of mode, a space, the size, a space, the
        // name.
        if (line.size() < 9 || line[0] != 'C' || line[5] != ' ')
        {
            *error = "Bad file line.";
            return false;
        }
        for (std::size_t i = 1; i < 5; ++i)
        {
            if (line[i] < '0' || line[i] > '7')
            {
                *error = "Bad file line.";
                return false;
            }
        }
        std::size_t i = 6;
        std::uint64_t value = 0;
        std::size_t digits = 0;
        for (; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i, ++digits)
        {
            if (digits >= 18)
            {
                *error = "Bad file size.";
                return false;
            }
            value = value * 10 + static_cast<std::uint64_t>(line[i] - '0');
        }
        if (digits == 0 || i >= line.size() || line[i] != ' ' || i + 1 >= line.size())
        {
            *error = "Bad file line.";
            return false;
        }
        std::string_view file_name = line.substr(i + 1);
        if (file_name.find('/') != std::string_view::npos || file_name == "." || file_name == "..")
        {
            *error = "Bad file name.";
            return false;
        }
        *size = value;
        *name = std::string(file_name.data(), file_name.size());
        return true;
    }

    // One transfer: the user's folders, and whether anything failed.
    class ScpTransfer
    {
    public:
        ScpTransfer(ScpChannel* channel, const std::string& db_path, const std::string& username, bool view_only)
            : channel_(channel),
              exports_dir_(SessionExportsDir(db_path, username)),
              imports_dir_(SessionImportsDir(db_path, username)),
              view_only_(view_only)
        {
        }

        int Receive(const ScpCommand& command)
        {
            SftpPath target;
            if (!ResolveSftpPath(command.path, &target) || target.area != SftpArea::kImports || view_only_)
            {
                Fatal(view_only_ ? "View-only users can't upload." : "Files can only be sent to /imports.");
                return 1;
            }
            if (command.target_is_folder && !target.name.empty())
            {
                Fatal("/imports" + std::string("/") + target.name + " isn't a folder.");
                return 1;
            }
            if (!Ack())
            {
                return 1;
            }

            std::string line;
            while (ReadLine(&line))
            {
                if (line.empty())
                {
                    Fatal("Protocol error.");
                    return 1;
                }
                char kind = line[0];
                if (kind == 'T' || kind == 'E')
                {
                    // Times (ignored) and the end of a folder that was refused.
                    if (!Ack())
                    {
                        return 1;
                    }
                }
                else if (kind == 'C')
                {
                    if (!ReceiveFile(line, target))
                    {
                        return 1;
                    }
                }
                else if (kind == 'D')
                {
                    Warn("Folders can't be copied, only files.");
                }
                else if (kind == '\x01')
                {
                    // The client couldn't send a file; it goes on to the next.
                    failed_ = true;
                }
                else if (kind == '\x02')
                {
                    return 1;
                }
                else
                {
                    Fatal("Protocol error.");
                    return 1;
                }
            }
            return failed_ ? 1 : 0;
        }

        int Send(const ScpCommand& command)
        {
            // The client says it's ready first.
            if (!ReadAck())
            {
                return 1;
            }
            SftpPath source;
            std::filesystem::path real_path;
            if (!ResolveSftpPath(command.path, &source) || source.area == SftpArea::kRoot)
            {
                Warn(command.path + ": No such file.");
                return 1;
            }
            if (source.name.empty())
            {
                Warn("Folders can't be copied, only files.");
                return 1;
            }
            real_path =
                std::filesystem::path(source.area == SftpArea::kExports ? exports_dir_ : imports_dir_) / source.name;
            std::error_code error;
            std::filesystem::file_status status = std::filesystem::symlink_status(real_path, error);
            std::ifstream file(real_path, std::ios::binary);
            if (error || !std::filesystem::is_regular_file(status) || !file)
            {
                Warn(source.name + ": No such file.");
                return 1;
            }
            std::uintmax_t size = std::filesystem::file_size(real_path, error);
            if (error)
            {
                Warn(source.name + ": Can't read the file.");
                return 1;
            }

            if (command.preserve)
            {
                std::filesystem::file_time_type changed = std::filesystem::last_write_time(real_path, error);
                std::int64_t seconds = static_cast<std::int64_t>(std::time(nullptr));
                if (!error)
                {
                    seconds -= std::chrono::duration_cast<std::chrono::seconds>(
                                   std::filesystem::file_time_type::clock::now() - changed)
                                   .count();
                }
                std::string times = "T" + std::to_string(seconds) + " 0 " + std::to_string(seconds) + " 0\n";
                if (!WriteText(times) || !ReadAck())
                {
                    return 1;
                }
            }
            std::string header = "C0644 " + std::to_string(size) + " " + source.name + "\n";
            if (!WriteText(header) || !ReadAck())
            {
                return 1;
            }
            std::vector<char> buffer(kScpChunk);
            std::uintmax_t left = size;
            while (left > 0)
            {
                std::size_t chunk = left < kScpChunk ? static_cast<std::size_t>(left) : kScpChunk;
                if (!file.read(buffer.data(), static_cast<std::streamsize>(chunk)) ||
                    !channel_->Write(buffer.data(), chunk))
                {
                    return 1;
                }
                left -= chunk;
            }
            char done = '\0';
            if (!channel_->Write(&done, 1) || !ReadAck())
            {
                return 1;
            }
            return 0;
        }

    private:
        bool ReceiveFile(const std::string& line, const SftpPath& target)
        {
            std::uint64_t size = 0;
            std::string name;
            std::string error;
            if (!ParseScpFileLine(line, &size, &name, &error))
            {
                Fatal(error);
                return false;
            }
            // A target naming a file renames what's sent to it.
            if (!target.name.empty())
            {
                name = target.name;
            }
            if (!IsAllowedImportName(name))
            {
                // Refused before its data comes: the client skips to the next.
                Warn(name + ": only .qlnet and .qlsession files.");
                return true;
            }
            if (size > SftpUploadLimit(SftpImportsBytesUsed(imports_dir_, name)))
            {
                Warn(name + (size > kSftpMaxUploadBytes ? ": over the 25 MB limit." : ": /imports is full (100 MB)."));
                return true;
            }
            if (!EnsureDirectory(imports_dir_))
            {
                Warn(name + ": can't create the folder.");
                return true;
            }
            // Hidden (see ResolveSftpPath) until it's whole.
            std::string temp_path = imports_dir_ + "/." + TemporaryPathFor(name);
            std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
            if (!file || !Ack())
            {
                std::error_code ignored;
                std::filesystem::remove(temp_path, ignored);
                if (!file)
                {
                    Warn(name + ": can't create the file.");
                    return true;
                }
                return false;
            }

            std::vector<char> buffer(kScpChunk);
            std::uint64_t left = size;
            bool written = true;
            while (left > 0)
            {
                std::size_t chunk = left < kScpChunk ? static_cast<std::size_t>(left) : kScpChunk;
                if (!channel_->Read(buffer.data(), chunk))
                {
                    file.close();
                    std::error_code ignored;
                    std::filesystem::remove(temp_path, ignored);
                    return false;
                }
                written = written && static_cast<bool>(file.write(buffer.data(), static_cast<std::streamsize>(chunk)));
                left -= chunk;
            }
            file.close();
            written = written && !file.fail();
            // The client's own "all sent" (or its error).
            bool client_ok = ReadAck();
            std::string replace_error;
            if (!client_ok || !written || !ReplaceWithFile(temp_path, imports_dir_ + "/" + name, &replace_error))
            {
                std::error_code ignored;
                std::filesystem::remove(temp_path, ignored);
                if (!client_ok)
                {
                    return true;
                }
                Warn(name + ": can't save the file.");
                return true;
            }
            return Ack();
        }

        // Reads one control line, without its newline. False at the end of
        // the stream (the client is done) or a line too long.
        bool ReadLine(std::string* line)
        {
            line->clear();
            char c = '\0';
            while (channel_->Read(&c, 1))
            {
                if (c == '\n')
                {
                    return true;
                }
                if (line->size() >= kMaxScpLine)
                {
                    return false;
                }
                line->push_back(c);
            }
            return false;
        }

        // Reads the other side's reply: 0 is fine, 1 an error (it carries
        // on), 2 a fatal error. Either error comes with a line of text.
        bool ReadAck()
        {
            char reply = '\0';
            if (!channel_->Read(&reply, 1))
            {
                return false;
            }
            if (reply == '\0')
            {
                return true;
            }
            std::string message;
            ReadLine(&message);
            failed_ = true;
            return false;
        }

        bool WriteText(const std::string& text)
        {
            return channel_->Write(text.data(), text.size());
        }

        bool Ack()
        {
            char ok = '\0';
            return channel_->Write(&ok, 1);
        }

        // An error the client reports and carries on after.
        void Warn(const std::string& message)
        {
            failed_ = true;
            WriteText("\x01scp: " + message + "\n");
        }

        // An error that ends the transfer.
        void Fatal(const std::string& message)
        {
            failed_ = true;
            WriteText("\x02scp: " + message + "\n");
        }

        ScpChannel* channel_;
        std::string exports_dir_;
        std::string imports_dir_;
        bool view_only_;
        bool failed_ = false;
    };

    int RunScpCommand(const ScpCommand& command, ScpChannel* channel, const std::string& db_path,
                      const std::string& username, bool view_only)
    {
        ScpTransfer transfer(channel, db_path, username, view_only);
        return command.receive ? transfer.Receive(command) : transfer.Send(command);
    }

}  // namespace ql
