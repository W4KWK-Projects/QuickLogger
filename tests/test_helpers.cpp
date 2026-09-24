#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <zlib.h>

namespace ql
{

    TempDir::TempDir()
    {
        static std::atomic<int> counter{0};
        std::int64_t stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("quicklogger-test-" + std::to_string(stamp) +
                                                      "-" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir);
        path_ = dir.string();
    }

    TempDir::~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::string TempDir::File(const std::string& name) const
    {
        return path_ + "/" + name;
    }

    void WriteTextFile(const std::string& path, const std::string& contents)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << contents;
    }

    std::string ReadTextFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    bool FileExists(const std::string& path)
    {
        std::error_code error;
        return std::filesystem::exists(path, error);
    }

    static void PutU16(std::string* out, std::uint32_t value)
    {
        out->push_back(static_cast<char>(value & 0xFFU));
        out->push_back(static_cast<char>((value >> 8U) & 0xFFU));
    }

    static void PutU32(std::string* out, std::uint32_t value)
    {
        PutU16(out, value & 0xFFFFU);
        PutU16(out, (value >> 16U) & 0xFFFFU);
    }

    static std::string RawDeflate(const std::string& data)
    {
        z_stream stream = {};
        if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK)
        {
            throw std::runtime_error("deflateInit2 failed");
        }
        std::string out(deflateBound(&stream, static_cast<uLong>(data.size())), '\0');
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_out = reinterpret_cast<Bytef*>(&out[0]);
        stream.avail_out = static_cast<uInt>(out.size());
        deflate(&stream, Z_FINISH);
        out.resize(stream.total_out);
        deflateEnd(&stream);
        return out;
    }

    void WriteZipFile(const std::string& path, const std::vector<ZipFixtureEntry>& entries)
    {
        std::string archive;
        std::string directory;
        for (const ZipFixtureEntry& entry : entries)
        {
            std::uint32_t crc = static_cast<std::uint32_t>(
                crc32(0L, reinterpret_cast<const Bytef*>(entry.contents.data()),
                      static_cast<uInt>(entry.contents.size())));
            std::string data = entry.deflate ? RawDeflate(entry.contents) : entry.contents;
            std::uint32_t method = entry.deflate ? 8 : 0;
            std::uint32_t offset = static_cast<std::uint32_t>(archive.size());

            PutU32(&archive, 0x04034b50);
            PutU16(&archive, 20);
            PutU16(&archive, 0);
            PutU16(&archive, method);
            PutU32(&archive, 0);  // Time/date.
            PutU32(&archive, crc);
            PutU32(&archive, static_cast<std::uint32_t>(data.size()));
            PutU32(&archive, static_cast<std::uint32_t>(entry.contents.size()));
            PutU16(&archive, static_cast<std::uint32_t>(entry.name.size()));
            PutU16(&archive, 0);
            archive += entry.name;
            archive += data;

            PutU32(&directory, 0x02014b50);
            PutU16(&directory, 20);
            PutU16(&directory, 20);
            PutU16(&directory, 0);
            PutU16(&directory, method);
            PutU32(&directory, 0);
            PutU32(&directory, crc);
            PutU32(&directory, static_cast<std::uint32_t>(data.size()));
            PutU32(&directory, static_cast<std::uint32_t>(entry.contents.size()));
            PutU16(&directory, static_cast<std::uint32_t>(entry.name.size()));
            PutU16(&directory, 0);
            PutU16(&directory, 0);
            PutU16(&directory, 0);
            PutU16(&directory, 0);
            PutU32(&directory, 0);
            PutU32(&directory, offset);
            directory += entry.name;
        }
        std::uint32_t directory_offset = static_cast<std::uint32_t>(archive.size());
        archive += directory;
        PutU32(&archive, 0x06054b50);
        PutU16(&archive, 0);
        PutU16(&archive, 0);
        PutU16(&archive, static_cast<std::uint32_t>(entries.size()));
        PutU16(&archive, static_cast<std::uint32_t>(entries.size()));
        PutU32(&archive, static_cast<std::uint32_t>(directory.size()));
        PutU32(&archive, directory_offset);
        PutU16(&archive, 0);
        WriteTextFile(path, archive);
    }

    std::string FileUrl(const std::string& path)
    {
        return "file://" + path;
    }

    Station MakeStation(const std::string& callsign, const std::string& name,
                        const std::string& zip, const std::string& city)
    {
        Station station;
        station.callsign = callsign;
        station.name = name;
        station.zip = zip;
        station.city = city;
        return station;
    }

    std::int64_t AddTestNet(Database* db, const std::string& name)
    {
        Net net;
        net.name = name;
        net.mode = "FM";
        return db->CreateNet(net);
    }

    std::int64_t AddTestInstance(Database* db, std::int64_t net_id, const std::string& date,
                                 std::int64_t started_at, const std::string& operator_callsign)
    {
        NetInstance instance;
        instance.net_id = net_id;
        instance.instance_date = date;
        instance.started_at = started_at;
        instance.net_control_callsign = operator_callsign;
        instance.created_by = operator_callsign;
        instance.operator_role = kRoleNetControl;
        return db->CreateNetInstance(instance);
    }

    std::int64_t AddTestCheckIn(Database* db, std::int64_t instance_id, const std::string& callsign,
                                int sequence, int designated_role)
    {
        db->RecordManualCheckInStation(MakeStation(callsign), 1);
        CheckIn check_in;
        check_in.net_instance_id = instance_id;
        check_in.callsign = callsign;
        check_in.sequence_number = sequence;
        check_in.checked_in_at = 1000 + sequence;
        check_in.designated_role = designated_role;
        return db->AddCheckIn(check_in);
    }

}  // namespace ql
