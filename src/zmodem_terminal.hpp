#pragma once

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <deque>

#include "zmodem_protocol.hpp"

// POSIX only: the real terminal as a ZmodemChannel.
namespace ql
{

    // The real terminal as a ZmodemChannel: standard input and output, put
    // in raw mode for as long as this lives (the line discipline would
    // otherwise eat XON/XOFF, turn CR into LF and echo the protocol back).
    class TerminalChannel : public ZmodemChannel
    {
    public:
        TerminalChannel()
        {
            have_settings_ = ::tcgetattr(STDIN_FILENO, &saved_) == 0;
            if (have_settings_)
            {
                struct termios raw = saved_;
                ::cfmakeraw(&raw);
                raw.c_cc[VMIN] = 1;
                raw.c_cc[VTIME] = 0;
                ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
            }
        }

        ~TerminalChannel() override
        {
            if (have_settings_)
            {
                ::tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_);
            }
        }

        TerminalChannel(const TerminalChannel&) = delete;
        TerminalChannel& operator=(const TerminalChannel&) = delete;

        bool Write(const unsigned char* data, std::size_t size) override
        {
            std::size_t done = 0;
            while (done < size)
            {
                ssize_t count = ::write(STDOUT_FILENO, data + done, size - done);
                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    struct pollfd writable{};
                    writable.fd = STDOUT_FILENO;
                    writable.events = POLLOUT;
                    ::poll(&writable, 1, 1000);
                    continue;
                }
                if (count <= 0)
                {
                    return false;
                }
                done += static_cast<std::size_t>(count);
            }
            return true;
        }

        int ReadByte(int timeout_ms) override
        {
            if (buffer_.empty())
            {
                struct pollfd readable{};
                readable.fd = STDIN_FILENO;
                readable.events = POLLIN;
                int ready = ::poll(&readable, 1, timeout_ms);
                if (ready < 0)
                {
                    return errno == EINTR ? kZmodemTimeout : kZmodemClosed;
                }
                if (ready == 0)
                {
                    return kZmodemTimeout;
                }
                unsigned char chunk[4096];
                ssize_t count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
                if (count < 0 && (errno == EINTR || errno == EAGAIN))
                {
                    return kZmodemTimeout;
                }
                if (count <= 0)
                {
                    return kZmodemClosed;
                }
                for (ssize_t i = 0; i < count; ++i)
                {
                    buffer_.push_back(chunk[i]);
                }
            }
            int byte = buffer_.front();
            buffer_.pop_front();
            return byte;
        }

    private:
        struct termios saved_{};
        bool have_settings_ = false;
        std::deque<unsigned char> buffer_;
    };

}  // namespace ql
