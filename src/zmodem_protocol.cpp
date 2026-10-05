#include "zmodem_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ql
{

    // Bytes and frame types of the protocol (Chuck Forsberg's ZMODEM).
    static constexpr unsigned char kZpad = '*';
    static constexpr unsigned char kZdle = 0x18;
    static constexpr unsigned char kXon = 0x11;

    // After a ZDLE, how a data subpacket ends.
    static constexpr unsigned char kZcrce = 'h';  // Last; the frame ends.
    static constexpr unsigned char kZcrcg = 'i';  // More follows, no answer.
    static constexpr unsigned char kZcrcq = 'j';  // More follows, answer wanted.
    static constexpr unsigned char kZcrcw = 'k';  // Frame ends, answer wanted.

    static constexpr int kZrqinit = 0;
    static constexpr int kZrinit = 1;
    static constexpr int kZsinit = 2;
    static constexpr int kZack = 3;
    static constexpr int kZfile = 4;
    static constexpr int kZskip = 5;
    static constexpr int kZnak = 6;
    static constexpr int kZabort = 7;
    static constexpr int kZfin = 8;
    static constexpr int kZrpos = 9;
    static constexpr int kZdata = 10;
    static constexpr int kZeof = 11;
    static constexpr int kZferr = 12;
    static constexpr int kZcrc = 13;
    static constexpr int kZcan = 16;

    // ZRINIT's flags (the header's last byte).
    static constexpr unsigned char kCanFdx = 0x01;
    static constexpr unsigned char kCanOvio = 0x02;
    static constexpr unsigned char kCanFc32 = 0x20;
    static constexpr unsigned char kEscCtl = 0x40;

    static constexpr std::size_t kBlockSize = 1024;

    // What reading one byte through ZDLE escapes can give besides a byte.
    static constexpr int kFrameEndBase = 0x100;  // + the frame-end letter.
    static constexpr int kGotCancel = 0x200;     // Five ZDLEs in a row: the other end gave up.

    // ---- Checksums -------------------------------------------------------------

    static std::uint16_t Crc16Update(std::uint16_t crc, unsigned char byte)
    {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8));
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = static_cast<std::uint16_t>((crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1);
        }
        return crc;
    }

    static std::uint32_t Crc32Update(std::uint32_t crc, unsigned char byte)
    {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        return crc;
    }

    std::uint16_t ZmodemCrc16(const unsigned char* data, std::size_t size, std::uint16_t crc)
    {
        for (std::size_t i = 0; i < size; ++i)
        {
            crc = Crc16Update(crc, data[i]);
        }
        return crc;
    }

    std::uint32_t ZmodemCrc32(const unsigned char* data, std::size_t size)
    {
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < size; ++i)
        {
            crc = Crc32Update(crc, data[i]);
        }
        return ~crc;
    }

    // ---- Writing ---------------------------------------------------------------

    // A header: a type and four bytes (a file position, or flags).
    struct Header
    {
        int type = 0;
        unsigned char bytes[4] = {0, 0, 0, 0};

        std::uint32_t Position() const
        {
            return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                   (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
        }
    };

    static Header MakePositionHeader(int type, std::uint32_t position)
    {
        Header header;
        header.type = type;
        header.bytes[0] = static_cast<unsigned char>(position & 0xFF);
        header.bytes[1] = static_cast<unsigned char>((position >> 8) & 0xFF);
        header.bytes[2] = static_cast<unsigned char>((position >> 16) & 0xFF);
        header.bytes[3] = static_cast<unsigned char>((position >> 24) & 0xFF);
        return header;
    }

    // Appends `byte`, ZDLE-escaped if the line might eat or misread it.
    static void PutEscaped(std::string* out, unsigned char byte, bool escape_controls, unsigned char* previous)
    {
        unsigned char plain = byte & 0x7F;
        bool escape = byte == kZdle || plain == 0x10 || plain == 0x11 || plain == 0x13 ||
                      (escape_controls && (byte & 0x60) == 0) || (plain == 0x0D && (*previous & 0x7F) == '@');
        if (escape)
        {
            out->push_back(static_cast<char>(kZdle));
            byte = static_cast<unsigned char>(byte ^ 0x40);
        }
        out->push_back(static_cast<char>(byte));
        *previous = byte;
    }

    static void PutHexByte(std::string* out, unsigned char byte)
    {
        static const char kDigits[] = "0123456789abcdef";
        out->push_back(kDigits[byte >> 4]);
        out->push_back(kDigits[byte & 0x0F]);
    }

    // A hex header: readable on any line, what the opening exchange uses.
    static std::string HexHeader(const Header& header)
    {
        std::string out;
        out.push_back(static_cast<char>(kZpad));
        out.push_back(static_cast<char>(kZpad));
        out.push_back(static_cast<char>(kZdle));
        out.push_back('B');
        unsigned char body[5] = {static_cast<unsigned char>(header.type), header.bytes[0], header.bytes[1],
                                 header.bytes[2], header.bytes[3]};
        for (unsigned char byte : body)
        {
            PutHexByte(&out, byte);
        }
        std::uint16_t crc = ZmodemCrc16(body, sizeof(body));
        PutHexByte(&out, static_cast<unsigned char>(crc >> 8));
        PutHexByte(&out, static_cast<unsigned char>(crc & 0xFF));
        out.push_back('\r');
        out.push_back(static_cast<char>(0x8A));
        if (header.type != kZfin && header.type != kZack)
        {
            out.push_back(static_cast<char>(kXon));
        }
        return out;
    }

    // A binary header, with a 16- or 32-bit checksum.
    static std::string BinaryHeader(const Header& header, bool crc32, bool escape_controls)
    {
        std::string out;
        out.push_back(static_cast<char>(kZpad));
        out.push_back(static_cast<char>(kZdle));
        out.push_back(crc32 ? 'C' : 'A');
        unsigned char body[5] = {static_cast<unsigned char>(header.type), header.bytes[0], header.bytes[1],
                                 header.bytes[2], header.bytes[3]};
        unsigned char previous = 0;
        for (unsigned char byte : body)
        {
            PutEscaped(&out, byte, escape_controls, &previous);
        }
        if (crc32)
        {
            std::uint32_t crc = ZmodemCrc32(body, sizeof(body));
            for (int shift = 0; shift < 32; shift += 8)
            {
                PutEscaped(&out, static_cast<unsigned char>((crc >> shift) & 0xFF), escape_controls, &previous);
            }
        }
        else
        {
            std::uint16_t crc = ZmodemCrc16(body, sizeof(body));
            PutEscaped(&out, static_cast<unsigned char>(crc >> 8), escape_controls, &previous);
            PutEscaped(&out, static_cast<unsigned char>(crc & 0xFF), escape_controls, &previous);
        }
        return out;
    }

    // One data subpacket: the bytes, escaped, then ZDLE and the ending
    // letter, then the checksum of both.
    static std::string DataSubpacket(const unsigned char* data, std::size_t size, unsigned char end, bool crc32,
                                     bool escape_controls)
    {
        std::string out;
        unsigned char previous = 0;
        std::uint16_t crc16 = 0;
        std::uint32_t crc32_value = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < size; ++i)
        {
            PutEscaped(&out, data[i], escape_controls, &previous);
            crc16 = Crc16Update(crc16, data[i]);
            crc32_value = Crc32Update(crc32_value, data[i]);
        }
        out.push_back(static_cast<char>(kZdle));
        out.push_back(static_cast<char>(end));
        crc16 = Crc16Update(crc16, end);
        crc32_value = Crc32Update(crc32_value, end);
        if (crc32)
        {
            std::uint32_t crc = ~crc32_value;
            for (int shift = 0; shift < 32; shift += 8)
            {
                PutEscaped(&out, static_cast<unsigned char>((crc >> shift) & 0xFF), escape_controls, &previous);
            }
        }
        else
        {
            PutEscaped(&out, static_cast<unsigned char>(crc16 >> 8), escape_controls, &previous);
            PutEscaped(&out, static_cast<unsigned char>(crc16 & 0xFF), escape_controls, &previous);
        }
        if (end == kZcrcw)
        {
            out.push_back(static_cast<char>(kXon));
        }
        return out;
    }

    // ---- Reading ---------------------------------------------------------------

    class Clock
    {
    public:
        static std::chrono::steady_clock::time_point In(int seconds)
        {
            return std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        }

        // Milliseconds left until `deadline`, at most `cap`.
        static int Left(std::chrono::steady_clock::time_point deadline, int cap)
        {
            std::chrono::milliseconds left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
            {
                return 0;
            }
            return static_cast<int>(std::min<long long>(left.count(), cap));
        }
    };

    // Reads from the channel, undoing ZDLE escapes.
    class Reader
    {
    public:
        explicit Reader(ZmodemChannel* channel) : channel_(channel) {}

        // A raw byte, or kZmodemTimeout/kZmodemClosed.
        int Raw(int timeout_ms)
        {
            return channel_->ReadByte(timeout_ms);
        }

        // A data byte with its escape undone: 0 to 255, kFrameEndBase + a
        // frame-ending letter, kGotCancel, or kZmodemTimeout/kZmodemClosed.
        int Unescaped(int timeout_ms)
        {
            int cancels = 0;
            while (true)
            {
                int byte = channel_->ReadByte(timeout_ms);
                if (byte < 0)
                {
                    return byte;
                }
                // The line's own flow control, never data.
                if (byte == 0x11 || byte == 0x13 || byte == 0x91 || byte == 0x93)
                {
                    continue;
                }
                if (byte != kZdle)
                {
                    return byte;
                }
                ++cancels;
                while (true)
                {
                    byte = channel_->ReadByte(timeout_ms);
                    if (byte < 0)
                    {
                        return byte;
                    }
                    if (byte == kZdle)
                    {
                        if (++cancels >= 5)
                        {
                            return kGotCancel;
                        }
                        continue;
                    }
                    break;
                }
                if (byte == kZcrce || byte == kZcrcg || byte == kZcrcq || byte == kZcrcw)
                {
                    return kFrameEndBase + byte;
                }
                if (byte == 'l')
                {
                    return 0x7F;
                }
                if (byte == 'm')
                {
                    return 0xFF;
                }
                if ((byte & 0x60) == 0x40)
                {
                    return byte ^ 0x40;
                }
                // Not a valid escape: skip it.
                cancels = 0;
            }
        }

    private:
        ZmodemChannel* channel_;
    };

    static int HexValue(int c)
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    }

    // What waiting for a header ended in.
    enum class HeaderResult
    {
        kGot,
        kTimeout,
        kClosed,
        kCancelled,
        kBad,  // Garbled: the caller asks again.
    };

    // Reads the next header, skipping whatever comes before it (the shell
    // text a terminal may echo, say). `crc32` is set for a binary header
    // with the long checksum, since that says what its data uses.
    static HeaderResult ReadHeader(Reader* reader, int timeout_ms, Header* header, bool* crc32, int pads = 0)
    {
        while (true)
        {
            int byte = reader->Raw(timeout_ms);
            if (byte == kZmodemTimeout)
            {
                return HeaderResult::kTimeout;
            }
            if (byte == kZmodemClosed)
            {
                return HeaderResult::kClosed;
            }
            if (byte == kZpad)
            {
                ++pads;
                continue;
            }
            if (byte == kZdle && pads > 0)
            {
                break;
            }
            pads = 0;
        }
        int kind = reader->Raw(timeout_ms);
        if (kind < 0)
        {
            return kind == kZmodemTimeout ? HeaderResult::kTimeout : HeaderResult::kClosed;
        }
        unsigned char body[5];
        *crc32 = false;
        if (kind == 'B')
        {
            int digits[14];
            for (int i = 0; i < 14; ++i)
            {
                int c = reader->Raw(timeout_ms);
                if (c < 0)
                {
                    return c == kZmodemTimeout ? HeaderResult::kTimeout : HeaderResult::kClosed;
                }
                digits[i] = HexValue(c);
                if (digits[i] < 0)
                {
                    return HeaderResult::kBad;
                }
            }
            for (int i = 0; i < 5; ++i)
            {
                body[i] = static_cast<unsigned char>(digits[i * 2] * 16 + digits[i * 2 + 1]);
            }
            unsigned int sent =
                static_cast<unsigned int>(digits[10] << 12 | digits[11] << 8 | digits[12] << 4 | digits[13]);
            if (sent != ZmodemCrc16(body, sizeof(body)))
            {
                return HeaderResult::kBad;
            }
        }
        else if (kind == 'A' || kind == 'C')
        {
            *crc32 = kind == 'C';
            for (int i = 0; i < 5; ++i)
            {
                int byte = reader->Unescaped(timeout_ms);
                if (byte == kGotCancel)
                {
                    return HeaderResult::kCancelled;
                }
                if (byte < 0)
                {
                    return byte == kZmodemTimeout ? HeaderResult::kTimeout : HeaderResult::kClosed;
                }
                if (byte > 0xFF)
                {
                    return HeaderResult::kBad;
                }
                body[i] = static_cast<unsigned char>(byte);
            }
            int check_bytes = *crc32 ? 4 : 2;
            unsigned char check[4] = {0, 0, 0, 0};
            for (int i = 0; i < check_bytes; ++i)
            {
                int byte = reader->Unescaped(timeout_ms);
                if (byte < 0 || byte > 0xFF)
                {
                    return byte == kZmodemClosed ? HeaderResult::kClosed : HeaderResult::kBad;
                }
                check[i] = static_cast<unsigned char>(byte);
            }
            if (*crc32)
            {
                std::uint32_t sent =
                    static_cast<std::uint32_t>(check[0]) | (static_cast<std::uint32_t>(check[1]) << 8) |
                    (static_cast<std::uint32_t>(check[2]) << 16) | (static_cast<std::uint32_t>(check[3]) << 24);
                if (sent != ZmodemCrc32(body, sizeof(body)))
                {
                    return HeaderResult::kBad;
                }
            }
            else
            {
                unsigned int sent = static_cast<unsigned int>(check[0] << 8 | check[1]);
                if (sent != ZmodemCrc16(body, sizeof(body)))
                {
                    return HeaderResult::kBad;
                }
            }
        }
        else
        {
            return HeaderResult::kBad;
        }
        header->type = body[0];
        for (int i = 0; i < 4; ++i)
        {
            header->bytes[i] = body[i + 1];
        }
        return HeaderResult::kGot;
    }

    // What reading a data subpacket ended in.
    enum class SubpacketResult
    {
        kGot,
        kTimeout,
        kClosed,
        kCancelled,
        kBad,  // The checksum failed.
        kTooBig,
    };

    // Reads one data subpacket, adding its bytes to `data` (at most
    // `limit` in all); `end` is its ending letter.
    static SubpacketResult ReadSubpacket(Reader* reader, int timeout_ms, bool crc32, std::string* data,
                                         std::size_t limit, unsigned char* end)
    {
        std::uint16_t crc16 = 0;
        std::uint32_t crc32_value = 0xFFFFFFFFu;
        std::string block;
        while (true)
        {
            int byte = reader->Unescaped(timeout_ms);
            if (byte == kGotCancel)
            {
                return SubpacketResult::kCancelled;
            }
            if (byte == kZmodemTimeout)
            {
                return SubpacketResult::kTimeout;
            }
            if (byte == kZmodemClosed)
            {
                return SubpacketResult::kClosed;
            }
            if (byte >= kFrameEndBase)
            {
                *end = static_cast<unsigned char>(byte - kFrameEndBase);
                break;
            }
            block.push_back(static_cast<char>(byte));
            crc16 = Crc16Update(crc16, static_cast<unsigned char>(byte));
            crc32_value = Crc32Update(crc32_value, static_cast<unsigned char>(byte));
            if (data->size() + block.size() > limit)
            {
                return SubpacketResult::kTooBig;
            }
        }
        crc16 = Crc16Update(crc16, *end);
        crc32_value = Crc32Update(crc32_value, *end);
        int check_bytes = crc32 ? 4 : 2;
        std::uint32_t sent = 0;
        for (int i = 0; i < check_bytes; ++i)
        {
            int byte = reader->Unescaped(timeout_ms);
            if (byte < 0 || byte > 0xFF)
            {
                return byte == kZmodemClosed ? SubpacketResult::kClosed : SubpacketResult::kBad;
            }
            sent = crc32 ? sent | (static_cast<std::uint32_t>(byte) << (8 * i))
                         : (sent << 8) | static_cast<unsigned>(byte);
        }
        if (crc32 ? sent != ~crc32_value : sent != crc16)
        {
            return SubpacketResult::kBad;
        }
        data->append(block);
        return SubpacketResult::kGot;
    }

    static bool WriteString(ZmodemChannel* channel, const std::string& text)
    {
        return channel->Write(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    }

    // ---- Sending ---------------------------------------------------------------

    // The file information that goes after ZFILE: its name, then size,
    // time and mode (the last two in octal), and what remains of the batch.
    static std::string FileInformation(const ZmodemFile& file, std::size_t files_left, std::size_t bytes_left)
    {
        char numbers[160];
        std::snprintf(numbers, sizeof(numbers), "%zu %llo 100644 0 %zu %zu", file.data.size(),
                      static_cast<unsigned long long>(file.mtime > 0 ? file.mtime : 0), files_left, bytes_left);
        std::string info = file.name;
        info.push_back('\0');
        info += numbers;
        info.push_back('\0');
        return info;
    }

    class Sender
    {
    public:
        Sender(ZmodemChannel* channel, std::string* error) : channel_(channel), reader_(channel), error_(error) {}

        bool Run(const std::vector<ZmodemFile>& files, int start_timeout_seconds, bool* answered)
        {
            *answered = false;
            if (!Start(start_timeout_seconds))
            {
                return false;
            }
            *answered = true;
            std::size_t bytes_left = 0;
            for (const ZmodemFile& file : files)
            {
                bytes_left += file.data.size();
            }
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                if (!SendFile(files[i], files.size() - i, bytes_left))
                {
                    return false;
                }
                bytes_left -= files[i].data.size();
            }
            return Finish();
        }

    private:
        bool Fail(const std::string& message)
        {
            *error_ = message;
            return false;
        }

        bool Send(const std::string& bytes)
        {
            return WriteString(channel_, bytes) || Fail("The terminal went away.");
        }

        bool SendHeader(const Header& header)
        {
            return Send(BinaryHeader(header, crc32_, escape_controls_));
        }

        // Opens with the request a receiver answers, until it does.
        bool Start(int timeout_seconds)
        {
            Header request;
            request.type = kZrqinit;
            std::chrono::steady_clock::time_point deadline = Clock::In(timeout_seconds);
            std::string text = "rz\r" + HexHeader(request);
            if (!Send(text))
            {
                return false;
            }
            std::chrono::steady_clock::time_point next_ask = Clock::In(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                Header header;
                bool long_check = false;
                HeaderResult result = ReadHeader(&reader_, Clock::Left(deadline, 500), &header, &long_check);
                if (result == HeaderResult::kClosed)
                {
                    return Fail("The terminal went away.");
                }
                if (result == HeaderResult::kCancelled)
                {
                    return Fail("The terminal cancelled the transfer.");
                }
                if (result == HeaderResult::kGot)
                {
                    if (header.type == kZrinit)
                    {
                        crc32_ = (header.bytes[3] & kCanFc32) != 0;
                        escape_controls_ = (header.bytes[3] & kEscCtl) != 0;
                        return true;
                    }
                    if (header.type == kZcan || header.type == kZabort)
                    {
                        return Fail("The terminal cancelled the transfer.");
                    }
                }
                if (std::chrono::steady_clock::now() >= next_ask)
                {
                    if (!Send(HexHeader(request)))
                    {
                        return false;
                    }
                    next_ask = Clock::In(5);
                }
            }
            return Fail("The terminal didn't answer a ZMODEM transfer.");
        }

        // Waits for the receiver's next header: nothing for `seconds` is a
        // failure. Skips what doesn't matter (a repeated ZRINIT).
        bool Await(int seconds, Header* header)
        {
            std::chrono::steady_clock::time_point deadline = Clock::In(seconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                bool long_check = false;
                HeaderResult result = ReadHeader(&reader_, Clock::Left(deadline, 500), header, &long_check);
                if (result == HeaderResult::kGot)
                {
                    if (header->type == kZcan || header->type == kZabort || header->type == kZferr)
                    {
                        return Fail("The terminal cancelled the transfer.");
                    }
                    return true;
                }
                if (result == HeaderResult::kClosed)
                {
                    return Fail("The terminal went away.");
                }
                if (result == HeaderResult::kCancelled)
                {
                    return Fail("The terminal cancelled the transfer.");
                }
            }
            return Fail("The terminal stopped answering.");
        }

        bool SendFile(const ZmodemFile& file, std::size_t files_left, std::size_t bytes_left)
        {
            Header offer;
            offer.type = kZfile;
            offer.bytes[3] = 1;  // Binary.
            std::string info = FileInformation(file, files_left, bytes_left);
            if (!SendHeader(offer) || !Send(DataSubpacket(reinterpret_cast<const unsigned char*>(info.data()),
                                                          info.size(), kZcrcw, crc32_, escape_controls_)))
            {
                return false;
            }
            std::uint32_t start = 0;
            while (true)
            {
                Header answer;
                if (!Await(30, &answer))
                {
                    return false;
                }
                if (answer.type == kZskip)
                {
                    return true;
                }
                if (answer.type == kZrpos)
                {
                    start = answer.Position();
                    break;
                }
                if (answer.type == kZcrc)
                {
                    Header crc = MakePositionHeader(
                        kZcrc, ZmodemCrc32(reinterpret_cast<const unsigned char*>(file.data.data()), file.data.size()));
                    if (!SendHeader(crc))
                    {
                        return false;
                    }
                }
                // Anything else (a repeated ZRINIT, say): keep waiting.
            }
            return SendData(file, start);
        }

        bool SendData(const ZmodemFile& file, std::uint32_t start)
        {
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(file.data.data());
            std::size_t size = file.data.size();
            std::size_t position = std::min<std::size_t>(start, size);
            while (true)
            {
                if (!SendHeader(MakePositionHeader(kZdata, static_cast<std::uint32_t>(position))))
                {
                    return false;
                }
                bool restarted = false;
                while (true)
                {
                    std::size_t count = std::min(kBlockSize, size - position);
                    bool last = position + count >= size;
                    if (!Send(DataSubpacket(bytes + position, count, last ? kZcrce : kZcrcg, crc32_, escape_controls_)))
                    {
                        return false;
                    }
                    position += count;
                    if (last)
                    {
                        break;
                    }
                    // The receiver may ask, mid-stream, to go back.
                    if (!CheckForRestart(&position, &restarted))
                    {
                        return false;
                    }
                    if (restarted)
                    {
                        break;
                    }
                }
                if (restarted)
                {
                    continue;
                }
                if (!SendHeader(MakePositionHeader(kZeof, static_cast<std::uint32_t>(size))))
                {
                    return false;
                }
                // Ready for the next file, or a request to resend.
                while (true)
                {
                    Header answer;
                    if (!Await(30, &answer))
                    {
                        return false;
                    }
                    if (answer.type == kZrinit)
                    {
                        return true;
                    }
                    if (answer.type == kZrpos)
                    {
                        position = std::min<std::size_t>(answer.Position(), size);
                        break;
                    }
                }
            }
        }

        // Looks, without waiting, for a header from the receiver while
        // data is going out; a ZRPOS moves `position` back.
        bool CheckForRestart(std::size_t* position, bool* restarted)
        {
            int first = reader_.Raw(0);
            if (first == kZmodemTimeout)
            {
                return true;
            }
            if (first == kZmodemClosed)
            {
                return Fail("The terminal went away.");
            }
            if (first != kZpad)
            {
                return true;  // Echoed noise: not a header.
            }
            Header header;
            bool long_check = false;
            // The padding byte is read already; the rest follows at once.
            HeaderResult result = ReadHeader(&reader_, 1000, &header, &long_check, 1);
            if (result == HeaderResult::kCancelled)
            {
                return Fail("The terminal cancelled the transfer.");
            }
            if (result != HeaderResult::kGot)
            {
                return true;
            }
            if (header.type == kZcan || header.type == kZabort)
            {
                return Fail("The terminal cancelled the transfer.");
            }
            if (header.type == kZrpos)
            {
                *position = header.Position();
                *restarted = true;
            }
            return true;
        }

        bool Finish()
        {
            Header fin;
            fin.type = kZfin;
            if (!Send(HexHeader(fin)))
            {
                return false;
            }
            std::chrono::steady_clock::time_point deadline = Clock::In(10);
            while (std::chrono::steady_clock::now() < deadline)
            {
                Header answer;
                bool long_check = false;
                HeaderResult result = ReadHeader(&reader_, Clock::Left(deadline, 500), &answer, &long_check);
                if (result == HeaderResult::kGot && answer.type == kZfin)
                {
                    return Send("OO");
                }
                if (result == HeaderResult::kClosed)
                {
                    return true;  // It has what it needs.
                }
            }
            return true;
        }

        ZmodemChannel* channel_;
        Reader reader_;
        std::string* error_;
        bool crc32_ = false;
        bool escape_controls_ = false;
    };

    bool ZmodemSend(ZmodemChannel* channel, const std::vector<ZmodemFile>& files, int start_timeout_seconds,
                    bool* answered, std::string* error)
    {
        Sender sender(channel, error);
        return sender.Run(files, start_timeout_seconds, answered);
    }

    // ---- Receiving -------------------------------------------------------------

    class Receiver
    {
    public:
        Receiver(ZmodemChannel* channel, std::vector<ZmodemFile>* received, std::size_t max_file_bytes,
                 int idle_timeout_seconds, std::string* error)
            : channel_(channel),
              reader_(channel),
              received_(received),
              max_file_bytes_(max_file_bytes),
              idle_seconds_(idle_timeout_seconds),
              error_(error)
        {
        }

        bool Run()
        {
            if (!SendReady())
            {
                return false;
            }
            std::chrono::steady_clock::time_point deadline = Clock::In(idle_seconds_);
            int asks = 0;
            while (true)
            {
                Header header;
                bool long_check = false;
                HeaderResult result = ReadHeader(&reader_, Clock::Left(deadline, 1000), &header, &long_check);
                if (result == HeaderResult::kClosed)
                {
                    return Fail("The terminal went away.");
                }
                if (result == HeaderResult::kCancelled)
                {
                    return Fail("The transfer was cancelled.");
                }
                if (result == HeaderResult::kBad)
                {
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return Fail("The sender stopped answering.");
                    }
                    if (!SendHexHeader(MakePositionHeader(kZnak, 0)))
                    {
                        return false;
                    }
                    continue;
                }
                if (result == HeaderResult::kTimeout)
                {
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return Fail("The sender stopped answering.");
                    }
                    // Say again that we are ready, now and then.
                    if (++asks % 3 == 0 && !SendReady())
                    {
                        return false;
                    }
                    continue;
                }
                deadline = Clock::In(idle_seconds_);
                if (header.type == kZrqinit)
                {
                    if (!SendReady())
                    {
                        return false;
                    }
                }
                else if (header.type == kZsinit)
                {
                    std::string ignored;
                    unsigned char end = 0;
                    ReadSubpacket(&reader_, 10000, long_check, &ignored, 4096, &end);
                    if (!SendHexHeader(MakePositionHeader(kZack, 1)))
                    {
                        return false;
                    }
                }
                else if (header.type == kZfile)
                {
                    if (!ReceiveFile(long_check))
                    {
                        return false;
                    }
                }
                else if (header.type == kZfin)
                {
                    Header fin;
                    fin.type = kZfin;
                    if (!SendHexHeader(fin))
                    {
                        return false;
                    }
                    // The sender's "OO", if it comes.
                    reader_.Raw(500);
                    reader_.Raw(100);
                    return true;
                }
                else if (header.type == kZcan || header.type == kZabort)
                {
                    return Fail("The transfer was cancelled.");
                }
            }
        }

    private:
        bool Fail(const std::string& message)
        {
            *error_ = message;
            return false;
        }

        bool SendHexHeader(const Header& header)
        {
            return WriteString(channel_, HexHeader(header)) || Fail("The terminal went away.");
        }

        bool SendReady()
        {
            Header ready;
            ready.type = kZrinit;
            // Escape controls, as `rz -e` asks: an unescaped XON/XOFF is eaten by
            // flow control on an SSH pty and corrupts the file.
            ready.bytes[3] = kCanFdx | kCanOvio | kCanFc32 | kEscCtl;
            return SendHexHeader(ready);
        }

        // "name\0size mtime ..." into a file to fill.
        static bool ParseFileInformation(const std::string& info, ZmodemFile* file, std::size_t* size)
        {
            std::string::size_type nul = info.find('\0');
            if (nul == std::string::npos || nul == 0)
            {
                return false;
            }
            std::string name = info.substr(0, nul);
            // Never a folder.
            std::string::size_type slash = name.find_last_of("/\\");
            file->name = slash == std::string::npos ? name : name.substr(slash + 1);
            if (file->name.empty() || file->name == "." || file->name == "..")
            {
                return false;
            }
            unsigned long long bytes = 0;
            unsigned long long modified = 0;
            if (std::sscanf(info.c_str() + nul + 1, "%llu %llo", &bytes, &modified) < 1)
            {
                bytes = 0;
            }
            *size = static_cast<std::size_t>(bytes);
            file->mtime = static_cast<std::int64_t>(modified);
            return true;
        }

        bool ReceiveFile(bool crc32)
        {
            std::string info;
            unsigned char end = 0;
            SubpacketResult got = ReadSubpacket(&reader_, 10000, crc32, &info, 4096, &end);
            if (got != SubpacketResult::kGot)
            {
                return SendHexHeader(MakePositionHeader(kZnak, 0));
            }
            ZmodemFile file;
            std::size_t size = 0;
            if (!ParseFileInformation(info, &file, &size) || size > max_file_bytes_)
            {
                return SendHexHeader(MakePositionHeader(kZskip, 0));
            }
            if (!SendHexHeader(MakePositionHeader(kZrpos, 0)))
            {
                return false;
            }
            while (true)
            {
                Header header;
                bool long_check = false;
                HeaderResult result = ReadHeader(&reader_, idle_seconds_ * 1000, &header, &long_check);
                if (result == HeaderResult::kClosed || result == HeaderResult::kTimeout)
                {
                    return Fail("The sender stopped answering.");
                }
                if (result == HeaderResult::kCancelled)
                {
                    return Fail("The transfer was cancelled.");
                }
                if (result == HeaderResult::kBad)
                {
                    if (!SendHexHeader(MakePositionHeader(kZrpos, static_cast<std::uint32_t>(file.data.size()))))
                    {
                        return false;
                    }
                    continue;
                }
                if (header.type == kZdata)
                {
                    if (header.Position() != file.data.size())
                    {
                        // Out of step: ask again from where we are.
                        if (!SendHexHeader(MakePositionHeader(kZrpos, static_cast<std::uint32_t>(file.data.size()))))
                        {
                            return false;
                        }
                        continue;
                    }
                    if (!ReceiveData(long_check, &file))
                    {
                        return false;
                    }
                }
                else if (header.type == kZeof)
                {
                    if (header.Position() != file.data.size())
                    {
                        if (!SendHexHeader(MakePositionHeader(kZrpos, static_cast<std::uint32_t>(file.data.size()))))
                        {
                            return false;
                        }
                        continue;
                    }
                    received_->push_back(std::move(file));
                    return SendReady();
                }
                else if (header.type == kZfile)
                {
                    // The offer again (our answer was lost): consume it.
                    std::string again;
                    ReadSubpacket(&reader_, 10000, long_check, &again, 4096, &end);
                    if (!SendHexHeader(MakePositionHeader(kZrpos, static_cast<std::uint32_t>(file.data.size()))))
                    {
                        return false;
                    }
                }
                else if (header.type == kZcan || header.type == kZabort)
                {
                    return Fail("The transfer was cancelled.");
                }
            }
        }

        // The subpackets after a ZDATA, until one ends the frame or one is
        // bad (then the sender is asked to resend from where this stands).
        bool ReceiveData(bool crc32, ZmodemFile* file)
        {
            while (true)
            {
                unsigned char end = 0;
                SubpacketResult got =
                    ReadSubpacket(&reader_, idle_seconds_ * 1000, crc32, &file->data, max_file_bytes_, &end);
                if (got == SubpacketResult::kTooBig)
                {
                    return Fail("A file is too big.");
                }
                if (got == SubpacketResult::kCancelled)
                {
                    return Fail("The transfer was cancelled.");
                }
                if (got == SubpacketResult::kClosed || got == SubpacketResult::kTimeout)
                {
                    return Fail("The sender stopped answering.");
                }
                if (got == SubpacketResult::kBad)
                {
                    return SendHexHeader(MakePositionHeader(kZrpos, static_cast<std::uint32_t>(file->data.size())));
                }
                if (end == kZcrcq || end == kZcrcw)
                {
                    if (!SendHexHeader(MakePositionHeader(kZack, static_cast<std::uint32_t>(file->data.size()))))
                    {
                        return false;
                    }
                }
                if (end == kZcrce || end == kZcrcw)
                {
                    return true;
                }
            }
        }

        ZmodemChannel* channel_;
        Reader reader_;
        std::vector<ZmodemFile>* received_;
        std::size_t max_file_bytes_;
        int idle_seconds_;
        std::string* error_;
    };

    bool ZmodemReceive(ZmodemChannel* channel, std::vector<ZmodemFile>* received, std::size_t max_file_bytes,
                       int idle_timeout_seconds, std::string* error)
    {
        Receiver receiver(channel, received, max_file_bytes, idle_timeout_seconds, error);
        return receiver.Run();
    }

}  // namespace ql
