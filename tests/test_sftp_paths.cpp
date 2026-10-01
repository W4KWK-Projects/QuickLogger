// The SFTP server's view of an SSH user's files (sftp_paths.hpp).

#include <fstream>
#include <string>

#include "../src/sftp_paths.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // `client_path` resolved, as an absolute path again; "(none)" if it's
    // outside the tree.
    static std::string Resolved(const std::string& client_path)
    {
        SftpPath path;
        if (!ResolveSftpPath(client_path, &path))
        {
            return "(none)";
        }
        return SftpPathString(path);
    }

    QL_TEST(SftpPathsResolveToTheTwoFolders)
    {
        CHECK_EQ(Resolved(""), std::string("/"));
        CHECK_EQ(Resolved("."), std::string("/"));
        CHECK_EQ(Resolved("/"), std::string("/"));
        CHECK_EQ(Resolved("exports"), std::string("/exports"));
        CHECK_EQ(Resolved("/imports/"), std::string("/imports"));
        CHECK_EQ(Resolved("//imports//./Net.qlnet"), std::string("/imports/Net.qlnet"));
        CHECK_EQ(Resolved("/exports/../imports/Net.qlnet"), std::string("/imports/Net.qlnet"));
        CHECK_EQ(Resolved("/imports/Net.qlnet/.."), std::string("/imports"));
    }

    QL_TEST(SftpPathsNeverLeaveTheTree)
    {
        CHECK_EQ(Resolved("/etc/passwd"), std::string("(none)"));
        CHECK_EQ(Resolved("../../quicklogger.db"), std::string("(none)"));
        CHECK_EQ(Resolved("/../exports"), std::string("/exports"));
        CHECK_EQ(Resolved("/imports/../../settings/w4kwk.txt"), std::string("(none)"));
        CHECK_EQ(Resolved("/imports/sub/Net.qlnet"), std::string("(none)"));
        // Hidden names, as an upload's temporary file has.
        CHECK_EQ(Resolved("/imports/.Net.qlnet.tmp-1-2"), std::string("(none)"));
    }

    QL_TEST(SftpPathsKnowTheirAreaAndName)
    {
        SftpPath path;
        REQUIRE(ResolveSftpPath("/exports/Skywarn_2026-01-06.zip", &path));
        CHECK(path.area == SftpArea::kExports);
        CHECK_EQ(path.name, std::string("Skywarn_2026-01-06.zip"));
        REQUIRE(ResolveSftpPath("/imports", &path));
        CHECK(path.area == SftpArea::kImports);
        CHECK(path.name.empty());
    }

    QL_TEST(OnlyNetAndSessionFilesCanBeUploaded)
    {
        CHECK(IsAllowedImportName("Skywarn.qlnet"));
        CHECK(IsAllowedImportName("Skywarn_2026-01-06.qlsession"));
        CHECK(!IsAllowedImportName(".qlnet"));
        CHECK(!IsAllowedImportName("Skywarn.qlnet.txt"));
        CHECK(!IsAllowedImportName("Skywarn.zip"));
        CHECK(!IsAllowedImportName("Sky warn.qlnet"));
        CHECK(!IsAllowedImportName("_Skywarn.qlnet"));
        CHECK(!IsAllowedImportName("Sky\\warn.qlnet"));
    }

    static void WriteBytes(const std::string& path, std::size_t count)
    {
        std::ofstream file(path, std::ios::binary);
        file << std::string(count, 'x');
    }

    QL_TEST(ImportsUsageCountsEveryFileButTheOneBeingReplaced)
    {
        TempDir dir;
        CHECK_EQ(SftpImportsBytesUsed(dir.File("missing"), "Net.qlnet"), std::uint64_t(0));
        WriteBytes(dir.File("Net.qlnet"), 100);
        WriteBytes(dir.File("Other.qlsession"), 20);
        // An upload in progress.
        WriteBytes(dir.File(".Third.qlnet.tmp-1-2"), 3);
        CHECK_EQ(SftpImportsBytesUsed(dir.path(), "New.qlnet"), std::uint64_t(123));
        CHECK_EQ(SftpImportsBytesUsed(dir.path(), "Net.qlnet"), std::uint64_t(23));
    }

    QL_TEST(UploadsStopAtTheImportsTotal)
    {
        CHECK_EQ(SftpUploadLimit(0), kSftpMaxUploadBytes);
        CHECK_EQ(SftpUploadLimit(kSftpMaxImportsBytes - kSftpMaxUploadBytes), kSftpMaxUploadBytes);
        CHECK_EQ(SftpUploadLimit(kSftpMaxImportsBytes - 10), std::uint64_t(10));
        CHECK_EQ(SftpUploadLimit(kSftpMaxImportsBytes), std::uint64_t(0));
        CHECK_EQ(SftpUploadLimit(kSftpMaxImportsBytes + 1), std::uint64_t(0));
    }

    QL_TEST(SftpLongNamesLookLikeLsL)
    {
        std::string file = SftpLongName("Net.qlnet", 0100644, 1234, 0, "w4kwk");
        CHECK_EQ(file.substr(0, 10), std::string("-rw-r--r--"));
        CHECK(file.find(" w4kwk    w4kwk        1234 ") != std::string::npos);
        CHECK_EQ(file.substr(file.size() - 10), std::string(" Net.qlnet"));
        std::string folder = SftpLongName("exports", 0040555, 0, 0, "w4kwk");
        CHECK_EQ(folder.substr(0, 10), std::string("dr-xr-xr-x"));
    }

}  // namespace ql
