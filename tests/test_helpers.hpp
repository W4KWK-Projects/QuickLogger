#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../src/db/database.hpp"
#include "../src/models.hpp"

namespace ql
{

    // A fresh, empty directory under the system temp directory, deleted
    // (with everything in it) when this goes out of scope. Every test that
    // touches files works inside one of these -- never the real app data.
    class TempDir
    {
    public:
        TempDir();
        ~TempDir();
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        [[nodiscard]] const std::string& path() const
        {
            return path_;
        }
        // `name` inside this directory.
        [[nodiscard]] std::string File(const std::string& name) const;

    private:
        std::string path_;
    };

    void WriteTextFile(const std::string& path, const std::string& contents);
    std::string ReadTextFile(const std::string& path);
    bool FileExists(const std::string& path);

    // One file to put in a test ZIP archive.
    struct ZipFixtureEntry
    {
        std::string name;
        std::string contents;
        bool deflate = true;  // Otherwise stored uncompressed.
    };

    // Writes a ZIP archive at `path` holding `entries`, the way zip tools do
    // (local headers, central directory, end record), so ExtractZipEntries
    // can be tested without shipping binary fixtures.
    void WriteZipFile(const std::string& path, const std::vector<ZipFixtureEntry>& entries);

    // A file:// URL for a local path, for DataSources in tests.
    std::string FileUrl(const std::string& path);

    // Convenience builders for database rows.
    Station MakeStation(const std::string& callsign, const std::string& name = "",
                        const std::string& zip = "", const std::string& city = "");
    std::int64_t AddTestNet(Database* db, const std::string& name);
    std::int64_t AddTestInstance(Database* db, std::int64_t net_id, const std::string& date,
                                 std::int64_t started_at, const std::string& operator_callsign);
    std::int64_t AddTestCheckIn(Database* db, std::int64_t instance_id, const std::string& callsign,
                                int sequence, int designated_role = kRoleNone);

}  // namespace ql
