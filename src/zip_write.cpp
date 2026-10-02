#include "zip_write.hpp"

#include <ctime>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <iterator>

#include <zlib.h>

#include "date_utils.hpp"
#include "file_export.hpp"

namespace ql
{

    // From the ZIP format's APPNOTE, as in zip_extract.cpp: the local file
    // header (4.3.7), central directory header (4.3.12) and end of central
    // directory record (4.3.16).
    static constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
    static constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50;
    static constexpr std::uint32_t kEndRecordSignature = 0x06054b50;
    static constexpr std::uint16_t kVersionNeeded = 20;  // 2.0: deflate.
    static constexpr std::uint16_t kMethodDeflated = 8;
    // Bit 11: the name is UTF-8.
    static constexpr std::uint16_t kFlagUtf8 = 0x0800;

    struct WrittenEntry
    {
        std::string name;
        std::uint32_t crc32 = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t uncompressed_size = 0;
        std::uint32_t local_header_offset = 0;
    };

    static void AppendU16(std::string* out, std::uint16_t value)
    {
        out->push_back(static_cast<char>(value & 0xFFU));
        out->push_back(static_cast<char>((value >> 8U) & 0xFFU));
    }

    static void AppendU32(std::string* out, std::uint32_t value)
    {
        for (unsigned int shift = 0; shift < 32; shift += 8)
        {
            out->push_back(static_cast<char>((value >> shift) & 0xFFU));
        }
    }

    // `data` as a raw deflate stream (no zlib header), as ZIP keeps it.
    static bool Deflate(const std::string& data, std::string* compressed)
    {
        z_stream stream{};
        if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        {
            return false;
        }
        compressed->resize(deflateBound(&stream, static_cast<uLong>(data.size())));
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_out = reinterpret_cast<Bytef*>(&(*compressed)[0]);
        stream.avail_out = static_cast<uInt>(compressed->size());
        int result = deflate(&stream, Z_FINISH);
        compressed->resize(stream.total_out);
        deflateEnd(&stream);
        return result == Z_STREAM_END;
    }

    bool WriteZipArchive(const std::string& zip_path, const std::vector<std::string>& file_paths,
                         std::int64_t modified_at, std::string* error)
    {
        // MS-DOS date and time, as ZIP keeps them.
        std::tm local = LocalTime(static_cast<std::time_t>(modified_at));
        std::uint16_t dos_time =
            static_cast<std::uint16_t>((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
        std::uint16_t dos_date =
            static_cast<std::uint16_t>(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);

        std::string archive;
        std::vector<WrittenEntry> entries;
        for (const std::string& path : file_paths)
        {
            // Read in one go, at its size, not a character at a time.
            std::error_code size_error;
            std::uintmax_t size = std::filesystem::file_size(path, size_error);
            std::ifstream in(path, std::ios::binary);
            std::string data(size_error ? 0 : static_cast<std::size_t>(size), '\0');
            if (!in || size_error || !in.read(&data[0], static_cast<std::streamsize>(data.size())))
            {
                *error = "Couldn't read " + path + ".";
                return false;
            }
            std::string compressed;
            if (!Deflate(data, &compressed))
            {
                *error = "Couldn't compress " + path + ".";
                return false;
            }
            WrittenEntry entry;
            entry.name = std::filesystem::path(path).filename().string();
            entry.crc32 = static_cast<std::uint32_t>(
                crc32(0L, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
            entry.compressed_size = static_cast<std::uint32_t>(compressed.size());
            entry.uncompressed_size = static_cast<std::uint32_t>(data.size());
            entry.local_header_offset = static_cast<std::uint32_t>(archive.size());

            AppendU32(&archive, kLocalHeaderSignature);
            AppendU16(&archive, kVersionNeeded);
            AppendU16(&archive, kFlagUtf8);
            AppendU16(&archive, kMethodDeflated);
            AppendU16(&archive, dos_time);
            AppendU16(&archive, dos_date);
            AppendU32(&archive, entry.crc32);
            AppendU32(&archive, entry.compressed_size);
            AppendU32(&archive, entry.uncompressed_size);
            AppendU16(&archive, static_cast<std::uint16_t>(entry.name.size()));
            AppendU16(&archive, 0);  // No extra field.
            archive += entry.name;
            archive += compressed;
            entries.push_back(entry);
        }

        std::uint32_t directory_offset = static_cast<std::uint32_t>(archive.size());
        for (const WrittenEntry& entry : entries)
        {
            AppendU32(&archive, kCentralHeaderSignature);
            AppendU16(&archive, kVersionNeeded);  // Made by: MS-DOS, 2.0.
            AppendU16(&archive, kVersionNeeded);
            AppendU16(&archive, kFlagUtf8);
            AppendU16(&archive, kMethodDeflated);
            AppendU16(&archive, dos_time);
            AppendU16(&archive, dos_date);
            AppendU32(&archive, entry.crc32);
            AppendU32(&archive, entry.compressed_size);
            AppendU32(&archive, entry.uncompressed_size);
            AppendU16(&archive, static_cast<std::uint16_t>(entry.name.size()));
            AppendU16(&archive, 0);  // Extra field.
            AppendU16(&archive, 0);  // Comment.
            AppendU16(&archive, 0);  // Disk number.
            AppendU16(&archive, 0);  // Internal attributes.
            AppendU32(&archive, 0);  // External attributes.
            AppendU32(&archive, entry.local_header_offset);
            archive += entry.name;
        }
        std::uint32_t directory_size = static_cast<std::uint32_t>(archive.size()) - directory_offset;
        AppendU32(&archive, kEndRecordSignature);
        AppendU16(&archive, 0);  // This disk.
        AppendU16(&archive, 0);  // The directory's disk.
        AppendU16(&archive, static_cast<std::uint16_t>(entries.size()));
        AppendU16(&archive, static_cast<std::uint16_t>(entries.size()));
        AppendU32(&archive, directory_size);
        AppendU32(&archive, directory_offset);
        AppendU16(&archive, 0);  // No comment.

        std::string temp_path = TemporaryPathFor(zip_path);
        {
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
            if (!out)
            {
                out.close();
                std::error_code remove_error;
                std::filesystem::remove(temp_path, remove_error);
                *error = "Couldn't write " + zip_path + ".";
                return false;
            }
        }
        return ReplaceWithFile(temp_path, zip_path, error);
    }

}  // namespace ql
