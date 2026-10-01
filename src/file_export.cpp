#include "file_export.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

namespace ql
{

    std::string ExportsDir(const std::string& db_path)
    {
        std::string::size_type slash = db_path.find_last_of('/');
        std::string dir = slash == std::string::npos ? std::string(".") : db_path.substr(0, slash);
        return dir + "/exports";
    }

    std::string ImportsDir(const std::string& db_path)
    {
        std::string::size_type slash = db_path.find_last_of('/');
        std::string dir = slash == std::string::npos ? std::string(".") : db_path.substr(0, slash);
        return dir + "/imports";
    }

    static const char* const kSshUsersSubdir = "ssh-users";

    std::string SessionExportsDir(const std::string& db_path, const std::string& ssh_username)
    {
        if (ssh_username.empty())
        {
            return ExportsDir(db_path);
        }
        return ExportsDir(db_path) + "/" + kSshUsersSubdir + "/" + SanitizeFilenameComponent(ssh_username);
    }

    std::string SessionImportsDir(const std::string& db_path, const std::string& ssh_username)
    {
        if (ssh_username.empty())
        {
            return ImportsDir(db_path);
        }
        return ImportsDir(db_path) + "/" + kSshUsersSubdir + "/" + SanitizeFilenameComponent(ssh_username);
    }

    std::string SshUserSettingsPath(const std::string& db_path, const std::string& username)
    {
        std::string::size_type slash = db_path.find_last_of('/');
        std::string dir = slash == std::string::npos ? std::string(".") : db_path.substr(0, slash);
        return dir + "/settings/" + username + ".txt";
    }

    // Moves `from` to `to` if `from` exists, replacing whatever is at `to`
    // -- unless they're the same file, as a change of case alone is on a
    // file system that ignores case.
    static bool MoveReplacing(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error)
    {
        std::error_code code;
        if (!std::filesystem::exists(from, code))
        {
            return true;
        }
        if (std::filesystem::exists(to, code) && !std::filesystem::equivalent(from, to, code))
        {
            std::filesystem::remove_all(to, code);
        }
        std::filesystem::create_directories(to.parent_path(), code);
        std::filesystem::rename(from, to, code);
        if (code)
        {
            *error = "Couldn't move " + from.string() + " to " + to.string() + ": " + code.message();
            return false;
        }
        return true;
    }

    bool MoveSshUserFiles(const std::string& db_path, const std::string& old_username, const std::string& new_username,
                          std::string* error)
    {
        return MoveReplacing(SshUserSettingsPath(db_path, old_username), SshUserSettingsPath(db_path, new_username),
                             error) &&
               MoveReplacing(SessionExportsDir(db_path, old_username), SessionExportsDir(db_path, new_username),
                             error) &&
               MoveReplacing(SessionImportsDir(db_path, old_username), SessionImportsDir(db_path, new_username), error);
    }

    // RemoveOldSshUserFiles for one of the two ssh-users directories.
    static int RemoveOldFilesUnder(const std::filesystem::path& root, std::filesystem::file_time_type cutoff)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error))
        {
            return 0;
        }
        int removed = 0;
        std::vector<std::filesystem::path> user_dirs;
        for (const std::filesystem::directory_entry& user_dir : std::filesystem::directory_iterator(root, error))
        {
            if (!user_dir.is_directory(error))
            {
                continue;
            }
            user_dirs.push_back(user_dir.path());
            std::vector<std::filesystem::path> old_files;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(user_dir.path(), error))
            {
                if (entry.is_regular_file(error) && entry.last_write_time(error) < cutoff)
                {
                    old_files.push_back(entry.path());
                }
            }
            for (const std::filesystem::path& file : old_files)
            {
                if (std::filesystem::remove(file, error))
                {
                    ++removed;
                }
            }
        }
        for (const std::filesystem::path& user_dir : user_dirs)
        {
            if (std::filesystem::is_empty(user_dir, error))
            {
                std::filesystem::remove(user_dir, error);
            }
        }
        return removed;
    }

    int RemoveOldSshUserFiles(const std::string& db_path, std::int64_t max_age_seconds)
    {
        std::filesystem::file_time_type cutoff =
            std::filesystem::file_time_type::clock::now() - std::chrono::seconds(max_age_seconds);
        return RemoveOldFilesUnder(std::filesystem::path(ExportsDir(db_path)) / kSshUsersSubdir, cutoff) +
               RemoveOldFilesUnder(std::filesystem::path(ImportsDir(db_path)) / kSshUsersSubdir, cutoff);
    }

    std::vector<std::string> ListFilesWithExtension(const std::string& dir, const std::string& extension)
    {
        std::vector<std::string> names;
        std::error_code error;
        if (!std::filesystem::is_directory(dir, error))
        {
            return names;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, error))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            std::string name = entry.path().filename().string();
            if (name.size() >= extension.size() &&
                name.compare(name.size() - extension.size(), extension.size(), extension) == 0)
            {
                names.push_back(std::move(name));
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool EnsureDirectory(const std::string& dir)
    {
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        return std::filesystem::is_directory(dir, error);
    }

    std::string TemporaryPathFor(const std::string& path)
    {
        // The clock and a random number together: unique across processes
        // (SSH sessions) as well as within one.
        std::random_device random;
        return path + ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(random());
    }

    bool ReplaceWithFile(const std::string& temp_path, const std::string& path, std::string* error)
    {
        std::error_code rename_error;
        std::filesystem::rename(temp_path, path, rename_error);
        if (rename_error)
        {
            std::error_code ignored;
            std::filesystem::remove(temp_path, ignored);
            *error = "Could not replace " + path + ": " + rename_error.message();
            return false;
        }
        return true;
    }

    bool WriteExportFile(const std::string& path, const std::vector<std::string>& lines, std::string* error)
    {
        std::string::size_type slash = path.find_last_of('/');
        if (slash != std::string::npos)
        {
            EnsureDirectory(path.substr(0, slash));
        }

        std::string temp_path = TemporaryPathFor(path);
        {
            // Binary, so every platform writes the same bytes: in text mode
            // Windows would turn each "\n" into "\r\n".
            std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                *error = "Could not open " + path + " for writing.";
                return false;
            }
            for (const std::string& line : lines)
            {
                file << line << "\n";
            }
            file.flush();
            if (!file.good())
            {
                file.close();
                std::error_code ignored;
                std::filesystem::remove(temp_path, ignored);
                *error = "Failed while writing " + path + ".";
                return false;
            }
        }
        return ReplaceWithFile(temp_path, path, error);
    }

    std::string SanitizeFilenameComponent(const std::string& text)
    {
        std::string result;
        bool last_was_underscore = false;
        for (char c : text)
        {
            bool safe = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
            if (safe)
            {
                result.push_back(c);
                last_was_underscore = false;
            }
            else if (!last_was_underscore)
            {
                result.push_back('_');
                last_was_underscore = true;
            }
        }

        std::string::size_type start = result.find_first_not_of('_');
        if (start == std::string::npos)
        {
            return "export";
        }
        std::string::size_type end = result.find_last_not_of('_');
        return result.substr(start, end - start + 1);
    }

}  // namespace ql
