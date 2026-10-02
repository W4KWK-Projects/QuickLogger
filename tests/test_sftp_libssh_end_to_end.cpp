// What OpenSSH's sftp and scp never do, sent by a libssh client of our own to
// a real QuickLogger SSH server on this machine: a second SFTP session on one
// connection, a resumed upload (APPEND) and a write past the end of the file.
// Run by tests/ssh_end_to_end.sh with the other end-to-end tests; without
// QL_PUSH_E2E_DIR they do nothing.

#include "test_framework.hpp"

#if defined(QUICKLOGGER_WITH_SSH)

#include <fcntl.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/file_export.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // A libssh client logged in as `user` with the run's key. The server's
    // host key isn't checked: it's the one this test started, on this machine.
    class SftpClient
    {
    public:
        SftpClient(const std::string& dir, const std::string& user)
        {
            session_ = ssh_new();
            std::string host = "127.0.0.1";
            int port = std::atoi(std::getenv("QL_PUSH_E2E_PORT"));
            ssh_options_set(session_, SSH_OPTIONS_HOST, host.c_str());
            ssh_options_set(session_, SSH_OPTIONS_PORT, &port);
            ssh_options_set(session_, SSH_OPTIONS_USER, user.c_str());
            long timeout = 20;
            ssh_options_set(session_, SSH_OPTIONS_TIMEOUT, &timeout);
            if (ssh_connect(session_) != SSH_OK)
            {
                return;
            }
            ssh_key key = nullptr;
            if (ssh_pki_import_privkey_file((dir + "/ssh/key").c_str(), nullptr, nullptr, nullptr, &key) != SSH_OK)
            {
                return;
            }
            logged_in_ = ssh_userauth_publickey(session_, nullptr, key) == SSH_AUTH_SUCCESS;
            ssh_key_free(key);
        }

        ~SftpClient()
        {
            for (sftp_session sftp : sessions_)
            {
                sftp_free(sftp);
            }
            ssh_disconnect(session_);
            ssh_free(session_);
        }

        bool logged_in() const
        {
            return logged_in_;
        }

        // A new SFTP session on this connection; null if the server refused.
        sftp_session OpenSession()
        {
            sftp_session sftp = sftp_new(session_);
            if (sftp == nullptr)
            {
                return nullptr;
            }
            if (sftp_init(sftp) != SSH_OK)
            {
                sftp_free(sftp);
                return nullptr;
            }
            sessions_.push_back(sftp);
            return sftp;
        }

    private:
        ssh_session session_ = nullptr;
        bool logged_in_ = false;
        std::vector<sftp_session> sessions_;
    };

    static std::vector<std::string> NamesIn(const std::string& folder)
    {
        std::vector<std::string> names;
        std::error_code ignored;
        for (std::filesystem::directory_iterator it(folder, ignored), end; !ignored && it != end; it.increment(ignored))
        {
            names.push_back(it->path().filename().string());
        }
        return names;
    }

    QL_TEST(SftpLibsshEndToEnd)
    {
        const char* dir_text = std::getenv("QL_PUSH_E2E_DIR");
        if (dir_text == nullptr)
        {
            return;
        }
        REQUIRE(std::getenv("QL_PUSH_E2E_PORT") != nullptr);
        std::string dir = dir_text;
        std::string imports = SessionImportsDir(dir + "/up/quicklogger.db", "W4KWK");
        REQUIRE(EnsureDirectory(imports));
        std::error_code ignored;
        for (const std::string& name : NamesIn(imports))
        {
            std::filesystem::remove(imports + "/" + name, ignored);
        }

        SftpClient client(dir, "W4KWK");
        REQUIRE(client.logged_in());
        sftp_session first = client.OpenSession();
        REQUIRE(first != nullptr);

        // A resumed upload is refused, and leaves nothing, not even a
        // hidden temporary file.
        sftp_file file = sftp_open(first, "/imports/Resume.qlnet", O_WRONLY | O_CREAT | O_APPEND, 0644);
        CHECK(file == nullptr);
        if (file != nullptr)
        {
            sftp_close(file);
        }
        CHECK(NamesIn(imports).empty());

        // A write that skips ahead is refused too: an upload starts from
        // nothing and is written in order.
        file = sftp_open(first, "/imports/Skip.qlnet", O_WRONLY | O_CREAT, 0644);
        REQUIRE(file != nullptr);
        sftp_seek64(file, 100);
        ssize_t written = sftp_write(file, "x", 1);
        CHECK(written < 0);
        sftp_close(file);
        CHECK(!FileExists(imports + "/Skip.qlnet"));
        CHECK(NamesIn(imports).empty());

        // A second SFTP session on the same connection is refused...
        CHECK(client.OpenSession() == nullptr);

        // ...and the first one carries on, upload and all.
        file = sftp_open(first, "/imports/Whole.qlnet", O_WRONLY | O_CREAT, 0644);
        REQUIRE(file != nullptr);
        CHECK_EQ(sftp_write(file, "whole\n", 6), static_cast<ssize_t>(6));
        CHECK_EQ(sftp_close(file), 0);
        CHECK_EQ(ReadTextFile(imports + "/Whole.qlnet"), std::string("whole\n"));

        // A connection of its own gets its session as usual.
        SftpClient other(dir, "W4KWK");
        REQUIRE(other.logged_in());
        CHECK(other.OpenSession() != nullptr);
        std::filesystem::remove(imports + "/Whole.qlnet", ignored);
    }

}  // namespace ql

#else

namespace ql
{
    QL_TEST(SftpLibsshEndToEnd) {}
}  // namespace ql

#endif
