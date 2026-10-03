#include "sftp_paths.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <vector>

#include "date_utils.hpp"
#include "file_export.hpp"

namespace ql
{

    bool ResolveSftpPath(std::string_view client_path, SftpPath* out)
    {
        std::vector<std::string_view> parts;
        parts.reserve(4);
        std::string_view::size_type start = 0;
        while (start <= client_path.size())
        {
            std::string_view::size_type slash = client_path.find('/', start);
            if (slash == std::string_view::npos)
            {
                slash = client_path.size();
            }
            std::string_view part = client_path.substr(start, slash - start);
            start = slash + 1;
            if (part.empty() || part == ".")
            {
                continue;
            }
            if (part == "..")
            {
                // As in POSIX, ".." at the top stays at the top.
                if (!parts.empty())
                {
                    parts.pop_back();
                }
                continue;
            }
            parts.emplace_back(part);
        }

        SftpPath path;
        if (parts.empty())
        {
            *out = std::move(path);
            return true;
        }
        if (parts[0] == "exports")
        {
            path.area = SftpArea::kExports;
        }
        else if (parts[0] == "imports")
        {
            path.area = SftpArea::kImports;
        }
        else
        {
            return false;
        }
        if (parts.size() > 2)
        {
            return false;
        }
        if (parts.size() == 2)
        {
            if (parts[1][0] == '.')
            {
                return false;
            }
            path.name = std::string(parts[1].data(), parts[1].size());
        }
        *out = std::move(path);
        return true;
    }

    std::string SftpPathString(const SftpPath& path)
    {
        if (path.area == SftpArea::kRoot)
        {
            return "/";
        }
        std::string result = path.area == SftpArea::kExports ? "/exports" : "/imports";
        if (!path.name.empty())
        {
            result += '/';
            result += path.name;
        }
        return result;
    }

    static bool EndsWith(std::string_view text, std::string_view suffix)
    {
        return text.size() > suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
    }

    bool IsAllowedImportName(std::string_view name)
    {
        if (!EndsWith(name, ".qlnet") && !EndsWith(name, ".qlsession"))
        {
            return false;
        }
        std::string text(name.data(), name.size());
        return name[0] != '.' && SanitizeFilenameComponent(text) == text;
    }

    std::uint64_t SftpImportsBytesUsed(const std::string& dir, std::string_view except_name)
    {
        std::uint64_t used = 0;
        std::error_code error;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, error))
        {
            if (!entry.is_regular_file(error) || entry.path().filename().string() == except_name)
            {
                continue;
            }
            std::uintmax_t size = entry.file_size(error);
            if (!error)
            {
                used += static_cast<std::uint64_t>(size);
            }
        }
        return used;
    }

    std::uint64_t SftpUploadLimit(std::uint64_t used_bytes)
    {
        if (used_bytes >= kSftpMaxImportsBytes)
        {
            return 0;
        }
        return std::min(kSftpMaxUploadBytes, kSftpMaxImportsBytes - used_bytes);
    }

    std::string SftpLongName(std::string_view name, std::uint32_t permissions, std::uint64_t size, std::int64_t mtime,
                             std::string_view owner)
    {
        std::string mode = (permissions & 0170000) == 0040000 ? "d" : "-";
        static const char kLetters[] = "rwxrwxrwx";
        for (int bit = 0; bit < 9; ++bit)
        {
            mode += (permissions & (0400u >> bit)) != 0 ? kLetters[bit] : '-';
        }

        // As OpenSSH's own server and `ls -l`: the time of day for the last
        // six months, the year before that.
        std::tm local = LocalTime(static_cast<std::time_t>(mtime));
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        const std::int64_t kSixMonths = 182LL * 24 * 60 * 60;
        bool recent = mtime > now - kSixMonths && mtime <= now + kSixMonths;
        char when[32];
        std::strftime(when, sizeof(when), recent ? "%b %e %H:%M" : "%b %e  %Y", &local);

        std::string owner_text(owner.data(), owner.size());
        char line[512];
        std::snprintf(line, sizeof(line), "%s    1 %-8s %-8s %8llu %s ", mode.c_str(), owner_text.c_str(),
                      owner_text.c_str(), static_cast<unsigned long long>(size), when);
        std::string result = line;
        result.append(name.data(), name.size());
        return result;
    }

}  // namespace ql
