#include "zip_extract.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>

#include <zlib.h>

namespace ql
{

    // The record signatures, sizes, and field offsets below are from the
    // ZIP file format's "APPNOTE" specification, sections 4.3.7 (local file
    // header), 4.3.12 (central directory file header) and 4.3.16 (end of
    // central directory record).
    static constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
    static constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50;
    static constexpr std::uint32_t kEndRecordSignature = 0x06054b50;
    static constexpr std::size_t kLocalHeaderSize = 30;
    static constexpr std::size_t kCentralHeaderSize = 46;
    static constexpr std::size_t kEndRecordSize = 22;
    // The end record is followed by an archive comment of at most this many
    // bytes, so it is always found within this distance of the file's end.
    static constexpr std::size_t kMaxCommentSize = 65535;
    static constexpr std::size_t kIoBufferSize = 64 * 1024;

    static constexpr std::uint16_t kFlagEncrypted = 0x0001;
    static constexpr std::uint16_t kMethodStored = 0;
    static constexpr std::uint16_t kMethodDeflated = 8;

    // A field whose real value doesn't fit in 16/32 bits is set to all ones
    // and the true value moves to a "ZIP64" extension record.
    static constexpr std::uint16_t kZip64Marker16 = 0xFFFF;
    static constexpr std::uint32_t kZip64Marker32 = 0xFFFFFFFF;

    struct ZipEntry
    {
        std::string name;
        std::uint16_t flags = 0;
        std::uint16_t method = 0;
        std::uint32_t crc32 = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t uncompressed_size = 0;
        std::uint32_t local_header_offset = 0;
    };

    static std::uint16_t ReadU16(const unsigned char* bytes)
    {
        return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
    }

    static std::uint32_t ReadU32(const unsigned char* bytes)
    {
        return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
               (static_cast<std::uint32_t>(bytes[2]) << 16) |
               (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    static std::string BaseName(const std::string& entry_name)
    {
        std::string::size_type slash = entry_name.find_last_of("/\\");
        return slash == std::string::npos ? entry_name : entry_name.substr(slash + 1);
    }

    // Reads the archive's central directory into `entries`.
    static bool ReadCentralDirectory(std::ifstream* file, std::vector<ZipEntry>* entries,
                                     std::string* error)
    {
        file->seekg(0, std::ios::end);
        std::streamoff file_size = file->tellg();
        if (file_size < static_cast<std::streamoff>(kEndRecordSize))
        {
            *error = "Not a ZIP archive (too small).";
            return false;
        }

        std::size_t tail_size = static_cast<std::size_t>(
            std::min<std::streamoff>(file_size, kEndRecordSize + kMaxCommentSize));
        std::vector<unsigned char> tail(tail_size);
        file->seekg(file_size - static_cast<std::streamoff>(tail_size), std::ios::beg);
        file->read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail_size));
        if (!file->good())
        {
            *error = "Could not read the ZIP archive.";
            return false;
        }

        // Scan backward for the end record; a candidate only counts if its
        // comment length lands exactly on the end of the file.
        std::size_t end_record = tail_size;
        for (std::size_t i = tail_size - kEndRecordSize + 1; i-- > 0;)
        {
            if (ReadU32(&tail[i]) == kEndRecordSignature &&
                i + kEndRecordSize + ReadU16(&tail[i + 20]) == tail_size)
            {
                end_record = i;
                break;
            }
        }
        if (end_record == tail_size)
        {
            *error = "Not a ZIP archive (no end-of-central-directory record).";
            return false;
        }

        std::uint16_t entry_count = ReadU16(&tail[end_record + 10]);
        std::uint32_t directory_size = ReadU32(&tail[end_record + 12]);
        std::uint32_t directory_offset = ReadU32(&tail[end_record + 16]);
        if (entry_count == kZip64Marker16 || directory_size == kZip64Marker32 ||
            directory_offset == kZip64Marker32)
        {
            *error = "ZIP64 archives aren't supported.";
            return false;
        }
        if (static_cast<std::streamoff>(directory_offset) + directory_size > file_size)
        {
            *error = "Corrupt ZIP archive (central directory out of range).";
            return false;
        }

        std::vector<unsigned char> directory(directory_size);
        file->seekg(static_cast<std::streamoff>(directory_offset), std::ios::beg);
        file->read(reinterpret_cast<char*>(directory.data()),
                   static_cast<std::streamsize>(directory_size));
        if (!file->good())
        {
            *error = "Could not read the ZIP archive's central directory.";
            return false;
        }

        std::size_t position = 0;
        for (std::uint16_t i = 0; i < entry_count; ++i)
        {
            if (position + kCentralHeaderSize > directory.size() ||
                ReadU32(&directory[position]) != kCentralHeaderSignature)
            {
                *error = "Corrupt ZIP archive (bad central directory entry).";
                return false;
            }
            const unsigned char* header = &directory[position];
            std::size_t name_length = ReadU16(header + 28);
            std::size_t extra_length = ReadU16(header + 30);
            std::size_t comment_length = ReadU16(header + 32);
            if (position + kCentralHeaderSize + name_length + extra_length + comment_length >
                directory.size())
            {
                *error = "Corrupt ZIP archive (central directory entry out of range).";
                return false;
            }

            ZipEntry entry;
            entry.flags = ReadU16(header + 8);
            entry.method = ReadU16(header + 10);
            entry.crc32 = ReadU32(header + 16);
            entry.compressed_size = ReadU32(header + 20);
            entry.uncompressed_size = ReadU32(header + 24);
            entry.local_header_offset = ReadU32(header + 42);
            entry.name.assign(reinterpret_cast<const char*>(header + kCentralHeaderSize),
                              name_length);
            if (entry.compressed_size == kZip64Marker32 ||
                entry.uncompressed_size == kZip64Marker32 ||
                entry.local_header_offset == kZip64Marker32)
            {
                *error = "ZIP64 archives aren't supported.";
                return false;
            }
            entries->push_back(entry);
            position += kCentralHeaderSize + name_length + extra_length + comment_length;
        }
        return true;
    }

    // Streams one entry's data to `dest_path`, then verifies its size and
    // CRC-32 against what the central directory recorded.
    static bool ExtractEntry(std::ifstream* file, const ZipEntry& entry,
                             const std::string& dest_path, std::string* error)
    {
        if ((entry.flags & kFlagEncrypted) != 0)
        {
            *error = entry.name + " is encrypted, which isn't supported.";
            return false;
        }
        if (entry.method != kMethodStored && entry.method != kMethodDeflated)
        {
            *error = entry.name + " uses an unsupported compression method.";
            return false;
        }

        // The local header repeats the name and carries its own (possibly
        // different) extra-field length, so the data's start has to be
        // computed from it rather than from the central directory.
        unsigned char local_header[kLocalHeaderSize];
        file->clear();
        file->seekg(static_cast<std::streamoff>(entry.local_header_offset), std::ios::beg);
        file->read(reinterpret_cast<char*>(local_header), kLocalHeaderSize);
        if (!file->good() || ReadU32(local_header) != kLocalHeaderSignature)
        {
            *error = "Corrupt ZIP archive (bad local header for " + entry.name + ").";
            return false;
        }
        std::streamoff data_offset = static_cast<std::streamoff>(entry.local_header_offset) +
                                     kLocalHeaderSize + ReadU16(local_header + 26) +
                                     ReadU16(local_header + 28);
        file->seekg(data_offset, std::ios::beg);

        std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            *error = "Could not open " + dest_path + " for writing.";
            return false;
        }

        std::vector<unsigned char> in_buffer(kIoBufferSize);
        std::vector<unsigned char> out_buffer(kIoBufferSize);
        std::uint32_t remaining = entry.compressed_size;
        std::uint64_t written = 0;
        uLong crc = crc32(0L, Z_NULL, 0);

        z_stream stream = {};
        bool deflated = entry.method == kMethodDeflated;
        if (deflated && inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        {
            *error = "Could not initialize decompression.";
            return false;
        }

        bool ok = true;
        int status = Z_OK;
        // True when the last inflate() call filled the whole output buffer,
        // in which case it may still be holding more output even though it
        // has consumed all its input -- it has to be called again before
        // any more input is fetched (or before running out of input counts
        // as a truncated archive).
        bool output_full = false;
        while (ok && (deflated ? status != Z_STREAM_END : remaining > 0))
        {
            if (!deflated || (stream.avail_in == 0 && !output_full))
            {
                if (remaining == 0)
                {
                    *error = "Corrupt ZIP archive (" + entry.name + " is truncated).";
                    ok = false;
                    break;
                }
                std::size_t chunk = std::min<std::size_t>(remaining, in_buffer.size());
                file->read(reinterpret_cast<char*>(in_buffer.data()),
                           static_cast<std::streamsize>(chunk));
                if (static_cast<std::size_t>(file->gcount()) != chunk)
                {
                    *error = "Corrupt ZIP archive (" + entry.name + " is truncated).";
                    ok = false;
                    break;
                }
                remaining -= static_cast<std::uint32_t>(chunk);
                stream.next_in = in_buffer.data();
                stream.avail_in = static_cast<uInt>(chunk);
            }

            const unsigned char* produced_data = nullptr;
            std::size_t produced = 0;
            if (deflated)
            {
                stream.next_out = out_buffer.data();
                stream.avail_out = static_cast<uInt>(out_buffer.size());
                status = inflate(&stream, Z_NO_FLUSH);
                if (status != Z_OK && status != Z_STREAM_END)
                {
                    *error = "Corrupt ZIP archive (could not decompress " + entry.name + ").";
                    ok = false;
                    break;
                }
                output_full = stream.avail_out == 0;
                produced_data = out_buffer.data();
                produced = out_buffer.size() - stream.avail_out;
            }
            else
            {
                produced_data = in_buffer.data();
                produced = stream.avail_in;
                stream.avail_in = 0;
            }

            out.write(reinterpret_cast<const char*>(produced_data),
                      static_cast<std::streamsize>(produced));
            crc = crc32(crc, produced_data, static_cast<uInt>(produced));
            written += produced;
        }
        if (deflated)
        {
            inflateEnd(&stream);
        }

        if (ok)
        {
            out.close();
            if (out.fail())
            {
                *error = "Failed while writing " + dest_path + ".";
                ok = false;
            }
            else if (written != entry.uncompressed_size ||
                     static_cast<std::uint32_t>(crc) != entry.crc32)
            {
                *error = "Corrupt ZIP archive (" + entry.name + " failed its checksum).";
                ok = false;
            }
        }
        return ok;
    }

    bool ExtractZipEntries(const std::string& zip_path, const std::string& dest_dir,
                           const std::vector<std::string>& wanted_names, std::string* error)
    {
        std::ifstream file(zip_path, std::ios::binary);
        if (!file.is_open())
        {
            *error = "Could not open " + zip_path + ".";
            return false;
        }

        std::vector<ZipEntry> entries;
        if (!ReadCentralDirectory(&file, &entries, error))
        {
            return false;
        }

        for (const std::string& wanted : wanted_names)
        {
            bool found = false;
            for (const ZipEntry& entry : entries)
            {
                if (BaseName(entry.name) != wanted)
                {
                    continue;
                }
                found = true;
                if (!ExtractEntry(&file, entry, dest_dir + "/" + wanted, error))
                {
                    return false;
                }
                break;
            }
            if (!found)
            {
                *error = wanted + " isn't in the archive.";
                return false;
            }
        }
        return true;
    }

}  // namespace ql
