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

    std::vector<std::string> ListFilesWithExtension(const std::string& dir,
                                                    const std::string& extension)
    {
        std::vector<std::string> names;
        std::error_code error;
        if (!std::filesystem::is_directory(dir, error))
        {
            return names;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(dir, error))
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
        return path + ".tmp-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
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

    bool WriteExportFile(const std::string& path, const std::vector<std::string>& lines,
                         std::string* error)
    {
        std::string::size_type slash = path.find_last_of('/');
        if (slash != std::string::npos)
        {
            EnsureDirectory(path.substr(0, slash));
        }

        std::string temp_path = TemporaryPathFor(path);
        {
            std::ofstream file(temp_path, std::ios::trunc);
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
            bool safe =
                std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
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
