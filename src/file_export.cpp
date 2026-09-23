#include "file_export.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

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
                names.push_back(name);
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

    bool WriteExportFile(const std::string& path, const std::vector<std::string>& lines,
                         std::string* error)
    {
        std::string::size_type slash = path.find_last_of('/');
        if (slash != std::string::npos)
        {
            EnsureDirectory(path.substr(0, slash));
        }

        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open())
        {
            *error = "Could not open " + path + " for writing.";
            return false;
        }
        for (const std::string& line : lines)
        {
            file << line << "\n";
        }
        if (!file.good())
        {
            *error = "Failed while writing " + path + ".";
            return false;
        }
        return true;
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
