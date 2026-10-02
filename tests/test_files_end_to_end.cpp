// SFTP and SCP end to end: the system's real sftp and scp against a real
// QuickLogger SSH server on this machine (sftp_server.cpp, scp_server.cpp),
// the way a person at a plain terminal fetches exports and sends imports.
// Run by tests/ssh_end_to_end.sh with the push tests
// (test_push_end_to_end.cpp); without QL_PUSH_E2E_DIR they do nothing.
//
// The OpenSSH sftp client reports a refusal by its status code only, so
// refusals are checked by exit status and by what's left on the server.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/file_export.hpp"
#include "../src/run_program.hpp"
#include "../src/sftp_paths.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static std::string FilesTestDir()
    {
        const char* dir = std::getenv("QL_PUSH_E2E_DIR");
        return dir == nullptr ? std::string() : std::string(dir);
    }

    static std::string UpstreamDb(const std::string& dir)
    {
        return dir + "/up/quicklogger.db";
    }

    // Runs `commands` with sftp -b as `user`. sftp stops at the first
    // command that fails and exits 1.
    static ProgramResult Sftp(const std::string& dir, const std::string& user, const std::string& commands)
    {
        std::string batch = dir + "/batch.txt";
        WriteTextFile(batch, commands);
        std::vector<std::string> arguments = {"-b", batch, "-P", std::getenv("QL_PUSH_E2E_PORT"), user + "@127.0.0.1"};
        return RunProgram(FindProgramOnPath("sftp"), arguments, 120, nullptr);
    }

    // Runs scp with `arguments` after the port, over SFTP or, if `legacy`,
    // the old SCP protocol (-O).
    static ProgramResult Scp(bool legacy, const std::vector<std::string>& arguments)
    {
        SetTestEnvironment("SCP_EXTRA", legacy ? "-O" : "");
        std::vector<std::string> all = {"-P", std::getenv("QL_PUSH_E2E_PORT")};
        all.insert(all.end(), arguments.begin(), arguments.end());
        ProgramResult result = RunProgram(FindProgramOnPath("scp"), all, 120, nullptr);
        SetTestEnvironment("SCP_EXTRA", "");
        return result;
    }

    // The names in `folder`, hidden ones too.
    static std::vector<std::string> FilesIn(const std::string& folder)
    {
        std::vector<std::string> names;
        std::error_code ignored;
        for (std::filesystem::directory_iterator it(folder, ignored), end; !ignored && it != end; it.increment(ignored))
        {
            names.push_back(it->path().filename().string());
        }
        return names;
    }

    // A file of `bytes` zero bytes at `path`.
    static void MakeFileOfSize(const std::string& path, std::uint64_t bytes)
    {
        WriteTextFile(path, "");
        std::filesystem::resize_file(path, bytes);
    }

    QL_TEST(FilesEndToEndSeed)
    {
        std::string dir = FilesTestDir();
        if (dir.empty())
        {
            return;
        }
        std::string exports = SessionExportsDir(UpstreamDb(dir), "W4KWK");
        REQUIRE(EnsureDirectory(exports));
        WriteTextFile(exports + "/Log.txt", "the log\n");
        std::string viewer_exports = SessionExportsDir(UpstreamDb(dir), "K4VIEW");
        REQUIRE(EnsureDirectory(viewer_exports));
        WriteTextFile(viewer_exports + "/Viewer.txt", "viewer's\n");
        // Next to the database, never reachable over SFTP.
        WriteTextFile(dir + "/up/secret.txt", "secret\n");

        std::string files = dir + "/files";
        REQUIRE(EnsureDirectory(files));
        WriteTextFile(files + "/Net.qlnet", "first\n");
        WriteTextFile(files + "/Net2.qlnet", "second\n");
        WriteTextFile(files + "/notes.txt", "notes\n");
    }

    QL_TEST(FilesEndToEndRun)
    {
        std::string dir = FilesTestDir();
        if (dir.empty())
        {
            return;
        }
        REQUIRE(std::getenv("QL_PUSH_E2E_PORT") != nullptr);
        REQUIRE(!FindProgramOnPath("sftp").empty());
        std::string files = dir + "/files";
        std::string imports = SessionImportsDir(UpstreamDb(dir), "W4KWK");
        std::string exports = SessionExportsDir(UpstreamDb(dir), "W4KWK");
        // /imports starts empty: a push that fails leaves nothing there.
        CHECK(FilesIn(imports).empty());

        // Listing: the two folders, then the user's own exports.
        ProgramResult result = Sftp(dir, "W4KWK", "pwd\nls -1 /\nls -1 /exports\n");
        CHECK_EQ(result.exit_status, 0);
        CHECK(result.output.find("exports") != std::string::npos);
        CHECK(result.output.find("imports") != std::string::npos);
        CHECK(result.output.find("Log.txt") != std::string::npos);
        CHECK(result.output.find("Viewer.txt") == std::string::npos);

        // Downloading an export, with sftp and with scp both ways.
        result = Sftp(dir, "W4KWK", "get /exports/Log.txt " + files + "/sftp-log.txt\n");
        CHECK_EQ(result.exit_status, 0);
        CHECK_EQ(ReadTextFile(files + "/sftp-log.txt"), std::string("the log\n"));
        result = Scp(false, {"W4KWK@127.0.0.1:/exports/Log.txt", files + "/scp-log.txt"});
        CHECK_EQ(result.exit_status, 0);
        CHECK_EQ(ReadTextFile(files + "/scp-log.txt"), std::string("the log\n"));
        result = Scp(true, {"W4KWK@127.0.0.1:/exports/Log.txt", files + "/legacy-log.txt"});
        CHECK_EQ(result.exit_status, 0);
        CHECK_EQ(ReadTextFile(files + "/legacy-log.txt"), std::string("the log\n"));

        // Uploading, replacing an upload, listing and removing it.
        result = Sftp(dir, "W4KWK", "cd /imports\nput " + files + "/Net.qlnet\n");
        CHECK_EQ(result.exit_status, 0);
        CHECK_EQ(ReadTextFile(imports + "/Net.qlnet"), std::string("first\n"));
        result = Sftp(dir, "W4KWK", "put " + files + "/Net2.qlnet /imports/Net.qlnet\nls -1 /imports\n");
        CHECK_EQ(result.exit_status, 0);
        CHECK_EQ(ReadTextFile(imports + "/Net.qlnet"), std::string("second\n"));
        CHECK(result.output.find("Net.qlnet") != std::string::npos);
        result = Sftp(dir, "W4KWK", "rm /imports/Net.qlnet\n");
        CHECK_EQ(result.exit_status, 0);
        CHECK(!FileExists(imports + "/Net.qlnet"));
        result = Scp(true, {files + "/Net.qlnet", "W4KWK@127.0.0.1:/imports/"});
        CHECK_EQ(result.exit_status, 0);
        CHECK_EQ(ReadTextFile(imports + "/Net.qlnet"), std::string("first\n"));
        CHECK_EQ(Sftp(dir, "W4KWK", "rm /imports/Net.qlnet\n").exit_status, 0);

        // Refused: other kinds of file, writing to /exports, removing an
        // export, and anything outside the two folders.
        // The server's reason reaches the person's terminal (OpenSSH shows
        // only the status code for the reply itself, so the server also
        // writes the reason to the channel's standard error).
        result = Sftp(dir, "W4KWK", "put " + files + "/notes.txt /imports/\n");
        CHECK(result.exit_status != 0);
        CHECK(result.errors.find("Only .qlnet and .qlsession files") != std::string::npos);
        result = Scp(false, {files + "/notes.txt", "W4KWK@127.0.0.1:/imports/"});
        CHECK(result.exit_status != 0);
        CHECK(result.errors.find("Only .qlnet and .qlsession files") != std::string::npos);
        CHECK(Scp(true, {files + "/notes.txt", "W4KWK@127.0.0.1:/imports/"}).exit_status != 0);
        CHECK(!FileExists(imports + "/notes.txt"));
        CHECK(Sftp(dir, "W4KWK", "put " + files + "/Net.qlnet /exports/\n").exit_status != 0);
        CHECK(!FileExists(exports + "/Net.qlnet"));
        CHECK(Sftp(dir, "W4KWK", "rm /exports/Log.txt\n").exit_status != 0);
        CHECK(FileExists(exports + "/Log.txt"));
        CHECK(Sftp(dir, "W4KWK", "get /exports/../../secret.txt " + files + "/escaped1\n").exit_status != 0);
        CHECK(Sftp(dir, "W4KWK", "get /../secret.txt " + files + "/escaped2\n").exit_status != 0);
        CHECK(Sftp(dir, "W4KWK", "get /etc/passwd " + files + "/escaped3\n").exit_status != 0);
        CHECK(Scp(true, {"W4KWK@127.0.0.1:/exports/../../secret.txt", files + "/escaped4"}).exit_status != 0);
        CHECK(Sftp(dir, "W4KWK", "get /exports/Viewer.txt " + files + "/escaped5\n").exit_status != 0);
        for (const char* name : {"escaped1", "escaped2", "escaped3", "escaped4", "escaped5"})
        {
            CHECK(!FileExists(files + "/" + name));
        }

        // A view-only user can download, never upload.
        result = Sftp(dir, "K4VIEW", "get /exports/Viewer.txt " + files + "/viewer.txt\n");
        CHECK_EQ(result.exit_status, 0);
        CHECK_EQ(ReadTextFile(files + "/viewer.txt"), std::string("viewer's\n"));
        result = Sftp(dir, "K4VIEW", "put " + files + "/Net.qlnet /imports/\n");
        CHECK(result.exit_status != 0);
        CHECK(result.errors.find("View-only users can't upload") != std::string::npos);
        result = Scp(false, {files + "/Net.qlnet", "K4VIEW@127.0.0.1:/imports/"});
        CHECK(result.exit_status != 0);
        CHECK(result.errors.find("View-only users can't upload") != std::string::npos);
        CHECK(Scp(true, {files + "/Net.qlnet", "K4VIEW@127.0.0.1:/imports/"}).exit_status != 0);
        CHECK(!FileExists(SessionImportsDir(UpstreamDb(dir), "K4VIEW") + "/Net.qlnet"));

        // 25 MB a file, 100 MB in all; a refused upload leaves nothing,
        // not even its hidden temporary file.
        MakeFileOfSize(files + "/Big.qlnet", kSftpMaxUploadBytes + 1);
        result = Sftp(dir, "W4KWK", "put " + files + "/Big.qlnet /imports/\n");
        CHECK(result.exit_status != 0);
        CHECK(result.errors.find("over the 25 MB limit") != std::string::npos);
        result = Scp(false, {files + "/Big.qlnet", "W4KWK@127.0.0.1:/imports/"});
        CHECK(result.exit_status != 0);
        CHECK(result.errors.find("over the 25 MB limit") != std::string::npos);
        CHECK(Scp(true, {files + "/Big.qlnet", "W4KWK@127.0.0.1:/imports/"}).exit_status != 0);
        CHECK(FilesIn(imports).empty());
        MakeFileOfSize(files + "/Full.qlnet", kSftpMaxUploadBytes);
        std::string fill;
        for (int i = 1; i <= 4; ++i)
        {
            fill += "put " + files + "/Full.qlnet /imports/Full" + std::to_string(i) + ".qlnet\n";
        }
        CHECK_EQ(Sftp(dir, "W4KWK", fill).exit_status, 0);
        CHECK_EQ(FilesIn(imports).size(), std::size_t{4});
        result = Sftp(dir, "W4KWK", "put " + files + "/Net.qlnet /imports/\n");
        CHECK(result.exit_status != 0);
        CHECK(result.errors.find("is full") != std::string::npos);
        result = Scp(false, {files + "/Net.qlnet", "W4KWK@127.0.0.1:/imports/"});
        CHECK(result.exit_status != 0);
        CHECK(result.errors.find("is full") != std::string::npos);
        CHECK(Scp(true, {files + "/Net.qlnet", "W4KWK@127.0.0.1:/imports/"}).exit_status != 0);
        CHECK_EQ(FilesIn(imports).size(), std::size_t{4});
        // Replacing one of them still fits.
        CHECK_EQ(Sftp(dir, "W4KWK", "put " + files + "/Net.qlnet /imports/Full1.qlnet\n").exit_status, 0);
        CHECK_EQ(ReadTextFile(imports + "/Full1.qlnet"), std::string("first\n"));
        CHECK_EQ(Sftp(dir, "W4KWK",
                      "rm /imports/Full1.qlnet\nrm /imports/Full2.qlnet\nrm /imports/Full3.qlnet\n"
                      "rm /imports/Full4.qlnet\n")
                     .exit_status,
                 0);
        CHECK(FilesIn(imports).empty());
    }

}  // namespace ql
