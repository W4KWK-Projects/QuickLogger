#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ql
{

    // QuickLogger's own ZMODEM, so a transfer needs no `sz`/`rz` (lrzsz):
    // Alpine has no such package, and the protocol's opening exchange can
    // tell whether a terminal speaks ZMODEM at all. This is the protocol
    // alone, over a ZmodemChannel; zmodem_send.hpp joins it to the real
    // terminal.

    // The two ends of a transfer's byte stream.
    class ZmodemChannel
    {
    public:
        virtual ~ZmodemChannel() = default;

        // Sends `size` bytes; false if the other end is gone.
        virtual bool Write(const unsigned char* data, std::size_t size) = 0;

        // The next byte, waiting up to `timeout_ms` for it (0 looks without
        // waiting): 0 to 255, or kZmodemTimeout, or kZmodemClosed once
        // nothing more will come.
        virtual int ReadByte(int timeout_ms) = 0;
    };

    constexpr int kZmodemTimeout = -1;
    constexpr int kZmodemClosed = -2;

    // One file as sent or received: its name (no folders), contents and
    // modification time (seconds since 1970, 0 if unknown).
    struct ZmodemFile
    {
        std::string name;
        std::string data;
        std::int64_t mtime = 0;
    };

    // Sends `files` as one batch. Waits up to `start_timeout_seconds` for
    // the other end to answer the opening request: it is the answer, or
    // its absence, that says whether the terminal speaks ZMODEM (see
    // `answered`, set once it has). False, with `error` set, if it never
    // answers, cancels, or the transfer fails.
    bool ZmodemSend(ZmodemChannel* channel, const std::vector<ZmodemFile>& files, int start_timeout_seconds,
                    bool* answered, std::string* error);

    // Receives whatever batch the other end sends into `received`, waiting
    // up to `idle_timeout_seconds` for the sender to start or to say
    // something next. A file over `max_file_bytes` is refused. False, with
    // `error` set, if it fails or the sender goes quiet.
    bool ZmodemReceive(ZmodemChannel* channel, std::vector<ZmodemFile>* received, std::size_t max_file_bytes,
                       int idle_timeout_seconds, std::string* error);

    // The checksums the protocol uses, exposed for testing.
    std::uint16_t ZmodemCrc16(const unsigned char* data, std::size_t size, std::uint16_t crc = 0);
    std::uint32_t ZmodemCrc32(const unsigned char* data, std::size_t size);

}  // namespace ql
