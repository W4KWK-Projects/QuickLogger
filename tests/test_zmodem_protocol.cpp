// QuickLogger's own ZMODEM (zmodem_protocol.hpp): sender and receiver
// against each other over in-memory pipes, and against the real lrzsz
// programs when they are installed.
// POSIX only: its channels are a pty, pipes to a program and the terminal, none
// of which Windows has (ZMODEM needs an SSH server, which Windows doesn't run).
#if !defined(_WIN32)

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <pty.h>
#include <utmp.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <util.h>
#endif

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/zmodem_protocol.hpp"
#include "../src/zmodem_terminal.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

extern char** environ;

namespace ql
{

    // One direction of a byte stream between two threads.
    class BytePipe
    {
    public:
        void Push(const unsigned char* data, std::size_t size)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (std::size_t i = 0; i < size; ++i)
            {
                bytes_.push_back(data[i]);
            }
            ready_.notify_all();
        }

        int Pop(int timeout_ms)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !bytes_.empty() || closed_; });
            if (bytes_.empty())
            {
                return closed_ ? kZmodemClosed : kZmodemTimeout;
            }
            int byte = bytes_.front();
            bytes_.pop_front();
            return byte;
        }

        void Close()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            ready_.notify_all();
        }

    private:
        std::mutex mutex_;
        std::condition_variable ready_;
        std::deque<unsigned char> bytes_;
        bool closed_ = false;
    };

    // One end of two BytePipes.
    class PipeChannel : public ZmodemChannel
    {
    public:
        PipeChannel(BytePipe* in, BytePipe* out) : in_(in), out_(out) {}

        bool Write(const unsigned char* data, std::size_t size) override
        {
            out_->Push(data, size);
            return true;
        }

        int ReadByte(int timeout_ms) override
        {
            return in_->Pop(timeout_ms);
        }

    private:
        BytePipe* in_;
        BytePipe* out_;
    };

    // A real program's stdin and stdout, as a channel.
    class ProcessChannel : public ZmodemChannel
    {
    public:
        ProcessChannel(const std::vector<std::string>& argv, const std::string& directory)
        {
            int to_child[2];
            int from_child[2];
            REQUIRE(::pipe(to_child) == 0);
            REQUIRE(::pipe(from_child) == 0);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, to_child[0], 0);
            posix_spawn_file_actions_adddup2(&actions, from_child[1], 1);
            posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addclose(&actions, to_child[1]);
            posix_spawn_file_actions_addclose(&actions, from_child[0]);
            posix_spawn_file_actions_addclose(&actions, to_child[0]);
            posix_spawn_file_actions_addclose(&actions, from_child[1]);
            // lrzsz's receiver saves into the working directory.
            std::string command = "cd '" + directory + "' && exec";
            for (const std::string& word : argv)
            {
                command += " '" + word + "'";
            }
            std::vector<std::string> shell{"/bin/sh", "-c", command};
            std::vector<char*> pointers;
            for (std::string& word : shell)
            {
                pointers.push_back(&word[0]);
            }
            pointers.push_back(nullptr);
            // The usual homes of lrzsz, for a test run from a bare PATH.
            std::string path = std::string("PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin");
            std::vector<char*> env;
            env.push_back(&path[0]);
            env.push_back(nullptr);
            REQUIRE(posix_spawn(&pid_, "/bin/sh", &actions, nullptr, pointers.data(), env.data()) == 0);
            posix_spawn_file_actions_destroy(&actions);
            ::close(to_child[0]);
            ::close(from_child[1]);
            write_fd_ = to_child[1];
            read_fd_ = from_child[0];
        }

        ~ProcessChannel() override
        {
            ::close(write_fd_);
            ::close(read_fd_);
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }

        bool Write(const unsigned char* data, std::size_t size) override
        {
            std::size_t done = 0;
            while (done < size)
            {
                ssize_t count = ::write(write_fd_, data + done, size - done);
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
            struct pollfd readable{};
            readable.fd = read_fd_;
            readable.events = POLLIN;
            int ready = ::poll(&readable, 1, timeout_ms);
            if (ready <= 0)
            {
                return kZmodemTimeout;
            }
            unsigned char byte = 0;
            ssize_t count = ::read(read_fd_, &byte, 1);
            return count == 1 ? byte : kZmodemClosed;
        }

    private:
        pid_t pid_ = -1;
        int write_fd_ = -1;
        int read_fd_ = -1;
    };

    static bool ProgramExists(const std::string& name)
    {
        for (const char* directory : {"/opt/homebrew/bin/", "/usr/local/bin/", "/usr/bin/"})
        {
            if (::access((std::string(directory) + name).c_str(), X_OK) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // The first of `names` that is installed, or "".
    static std::string FirstInstalled(const std::vector<std::string>& names)
    {
        for (const std::string& name : names)
        {
            if (ProgramExists(name))
            {
                return name;
            }
        }
        return "";
    }

    // Every byte value, so escaping is exercised, then more.
    static std::string AwkwardBytes(std::size_t size)
    {
        std::string data;
        for (std::size_t i = 0; i < size; ++i)
        {
            data.push_back(static_cast<char>((i * 7 + i / 256) & 0xFF));
        }
        // Runs that need care: "@\r" and the escape character itself.
        data += std::string("@\r@\r\x18\x18\x10\x11\x13\x90\x91\x93", 13);
        return data;
    }

    QL_TEST(ZmodemChecksumsMatchTheStandardCheckValues)
    {
        const unsigned char text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        CHECK_EQ(static_cast<int>(ZmodemCrc16(text, sizeof(text))), 0x31C3);
        CHECK_EQ(ZmodemCrc32(text, sizeof(text)), std::uint32_t{0xCBF43926u});
    }

    QL_TEST(OurSenderAndReceiverTransferFilesToEachOther)
    {
        BytePipe to_receiver;
        BytePipe to_sender;
        PipeChannel sender_end(&to_sender, &to_receiver);
        PipeChannel receiver_end(&to_receiver, &to_sender);

        std::vector<ZmodemFile> files(3);
        files[0].name = "Skywarn_2026-09-24_log.txt";
        files[0].data = "a log\nof lines\n";
        files[0].mtime = 1790000000;
        files[1].name = "big.bin";
        files[1].data = AwkwardBytes(300000);
        files[2].name = "empty.txt";

        std::vector<ZmodemFile> received;
        std::string receive_error;
        bool received_ok = false;
        std::thread receiver([&]
                             { received_ok = ZmodemReceive(&receiver_end, &received, 1 << 20, 10, &receive_error); });
        std::string send_error;
        bool answered = false;
        bool sent_ok = ZmodemSend(&sender_end, files, 10, &answered, &send_error);
        receiver.join();

        CHECK(sent_ok);
        CHECK(answered);
        CHECK(received_ok);
        CHECK_EQ(send_error, std::string(""));
        CHECK_EQ(receive_error, std::string(""));
        REQUIRE(received.size() == 3);
        for (std::size_t i = 0; i < 3; ++i)
        {
            CHECK_EQ(received[i].name, files[i].name);
            CHECK(received[i].data == files[i].data);
        }
        CHECK_EQ(received[0].mtime, files[0].mtime);
    }

    QL_TEST(ASenderGivesUpWhenNothingAnswers)
    {
        BytePipe nowhere;
        BytePipe silent;
        PipeChannel channel(&silent, &nowhere);
        std::vector<ZmodemFile> files(1);
        files[0].name = "x.txt";
        files[0].data = "x";
        std::string error;
        bool answered = true;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        CHECK(!ZmodemSend(&channel, files, 1, &answered, &error));
        CHECK(!answered);
        CHECK(!error.empty());
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    }

    QL_TEST(OurSenderFeedsTheRealRz)
    {
        std::string rz = FirstInstalled({"rz", "lrz"});
        if (rz.empty())
        {
            return;  // lrzsz isn't installed here.
        }
        TempDir dir;
        ProcessChannel channel({rz, "-b", "-e"}, dir.path());
        std::vector<ZmodemFile> files(2);
        files[0].name = "Weekly Net.qlsession";
        files[0].data = AwkwardBytes(70000);
        files[1].name = "notes.txt";
        files[1].data = "hello\n";
        std::string error;
        bool answered = false;
        CHECK(ZmodemSend(&channel, files, 10, &answered, &error));
        CHECK_EQ(error, std::string(""));
        CHECK(answered);
        for (const ZmodemFile& file : files)
        {
            std::ifstream in(dir.File(file.name), std::ios::binary);
            std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            CHECK(saved == file.data);
        }
    }

    // The in-app sender over a real pty, as over SSH, with the real rz
    // standing in for the user's terminal: raw mode and flow-control bytes
    // in the data are where the old sz/rz setup came to grief.
    QL_TEST(OurSenderOverARealPtyFeedsTheRealRz)
    {
        std::string rz = FirstInstalled({"rz", "lrz"});
        if (rz.empty())
        {
            return;
        }
        TempDir dir;
        std::string contents = AwkwardBytes(120000);
        int master = -1;
        int slave = -1;
        REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
        pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0)
        {
            ::close(master);
            ::login_tty(slave);
            std::vector<ZmodemFile> files(1);
            files[0].name = "Skywarn.qlsession";
            files[0].data = contents;
            bool answered = false;
            std::string error;
            bool ok = false;
            {
                TerminalChannel terminal;
                ok = ZmodemSend(&terminal, files, 15, &answered, &error);
            }
            ::_exit(ok ? 0 : 1);
        }
        ::close(slave);
        ProcessChannel receiver({rz, "-b"}, dir.path());
        int status = -1;
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            bool moved = false;
            int byte = receiver.ReadByte(0);
            while (byte >= 0)
            {
                unsigned char out = static_cast<unsigned char>(byte);
                ::write(master, &out, 1);
                moved = true;
                byte = receiver.ReadByte(0);
            }
            struct pollfd readable{};
            readable.fd = master;
            readable.events = POLLIN;
            while (::poll(&readable, 1, 0) > 0)
            {
                unsigned char chunk[4096];
                ssize_t count = ::read(master, chunk, sizeof(chunk));
                if (count <= 0)
                {
                    break;
                }
                receiver.Write(chunk, static_cast<std::size_t>(count));
                moved = true;
            }
            if (::waitpid(child, &status, WNOHANG) == child)
            {
                break;
            }
            if (!moved)
            {
                ::usleep(1000);
            }
        }
        ::close(master);
        if (status == -1)
        {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
        }
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        std::ifstream in(dir.File("Skywarn.qlsession"), std::ios::binary);
        std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(saved == contents);
    }

    QL_TEST(OurReceiverTakesFilesFromTheRealSz)
    {
        std::string sz = FirstInstalled({"sz", "lsz"});
        if (sz.empty())
        {
            return;
        }
        TempDir dir;
        std::string first = AwkwardBytes(90000);
        {
            std::ofstream out(dir.File("one.bin"), std::ios::binary);
            out << first;
        }
        {
            std::ofstream out(dir.File("two.txt"), std::ios::binary);
            out << "second\n";
        }
        ProcessChannel channel({sz, "-b", "one.bin", "two.txt"}, dir.path());
        std::vector<ZmodemFile> received;
        std::string error;
        CHECK(ZmodemReceive(&channel, &received, 1 << 20, 10, &error));
        CHECK_EQ(error, std::string(""));
        REQUIRE(received.size() == 2);
        CHECK_EQ(received[0].name, std::string("one.bin"));
        CHECK(received[0].data == first);
        CHECK_EQ(received[1].name, std::string("two.txt"));
        CHECK_EQ(received[1].data, std::string("second\n"));
    }

}  // namespace ql

#endif  // _WIN32
