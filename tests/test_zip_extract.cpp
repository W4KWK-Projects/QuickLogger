// The in-process ZIP extractor used for the FCC and Census downloads.

#include <string>
#include <vector>

#include "../src/zip_extract.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    static std::string LargeText()
    {
        std::string text;
        for (int i = 0; i < 20000; ++i)
        {
            text += "HD|" + std::to_string(i) + "|some|repeated|licence|data\n";
        }
        return text;
    }

    QL_TEST(ZipExtractsDeflatedAndStoredEntries)
    {
        TempDir dir;
        std::string big = LargeText();
        WriteZipFile(
            dir.File("a.zip"),
            {{"HD.dat", big, true}, {"EN.dat", "EN|1|name\n", false}, {"empty.txt", "", true}});
        std::string error;
        REQUIRE(ExtractZipEntries(dir.File("a.zip"), dir.path(), {"HD.dat", "EN.dat", "empty.txt"},
                                  &error));
        CHECK_EQ(ReadTextFile(dir.File("HD.dat")), big);
        CHECK_EQ(ReadTextFile(dir.File("EN.dat")), std::string("EN|1|name\n"));
        CHECK_EQ(ReadTextFile(dir.File("empty.txt")), std::string(""));
    }

    QL_TEST(ZipExtractsOnlyTheNamedEntries)
    {
        TempDir dir;
        WriteZipFile(dir.File("a.zip"), {{"keep.txt", "k", true}, {"skip.txt", "s", true}});
        std::string error;
        REQUIRE(ExtractZipEntries(dir.File("a.zip"), dir.path(), {"keep.txt"}, &error));
        CHECK(FileExists(dir.File("keep.txt")));
        CHECK(!FileExists(dir.File("skip.txt")));
    }

    QL_TEST(ZipIgnoresDirectoriesInsideTheArchive)
    {
        TempDir dir;
        // Like `unzip -j`: a nested path lands in the destination by its base
        // name -- which also stops "../" from escaping the destination.
        WriteZipFile(dir.File("a.zip"),
                     {{"../../escape.txt", "x", true}, {"some/dir/inner.txt", "y", true}});
        std::string error;
        REQUIRE(ExtractZipEntries(dir.File("a.zip"), dir.File(""), {"escape.txt", "inner.txt"},
                                  &error));
        CHECK_EQ(ReadTextFile(dir.File("escape.txt")), std::string("x"));
        CHECK_EQ(ReadTextFile(dir.File("inner.txt")), std::string("y"));
    }

    QL_TEST(ZipReportsAMissingEntry)
    {
        TempDir dir;
        WriteZipFile(dir.File("a.zip"), {{"one.txt", "1", true}});
        std::string error;
        CHECK(!ExtractZipEntries(dir.File("a.zip"), dir.path(), {"two.txt"}, &error));
        CHECK(error.find("two.txt") != std::string::npos);
    }

    QL_TEST(ZipRejectsFilesThatArentArchives)
    {
        TempDir dir;
        WriteTextFile(dir.File("not.zip"), "this is plainly not a zip archive at all");
        WriteTextFile(dir.File("empty.zip"), "");
        std::string error;
        CHECK(!ExtractZipEntries(dir.File("not.zip"), dir.path(), {"x"}, &error));
        CHECK(!error.empty());
        error.clear();
        CHECK(!ExtractZipEntries(dir.File("empty.zip"), dir.path(), {"x"}, &error));
        CHECK(!error.empty());
        error.clear();
        CHECK(!ExtractZipEntries(dir.File("missing.zip"), dir.path(), {"x"}, &error));
        CHECK(!error.empty());
    }

    QL_TEST(ZipRejectsTruncatedArchives)
    {
        TempDir dir;
        WriteZipFile(dir.File("a.zip"), {{"HD.dat", LargeText(), true}});
        std::string whole = ReadTextFile(dir.File("a.zip"));
        WriteTextFile(dir.File("cut.zip"), whole.substr(0, whole.size() / 2));
        std::string error;
        CHECK(!ExtractZipEntries(dir.File("cut.zip"), dir.path(), {"HD.dat"}, &error));
        CHECK(!error.empty());
    }

    QL_TEST(ZipDetectsCorruptedData)
    {
        TempDir dir;
        WriteZipFile(dir.File("stored.zip"), {{"a.txt", std::string(5000, 'a'), false}});
        std::string bytes = ReadTextFile(dir.File("stored.zip"));
        bytes[100] = 'b';  // Inside the stored data.
        WriteTextFile(dir.File("stored.zip"), bytes);
        std::string error;
        CHECK(!ExtractZipEntries(dir.File("stored.zip"), dir.path(), {"a.txt"}, &error));
        CHECK(error.find("checksum") != std::string::npos);

        WriteZipFile(dir.File("deflated.zip"), {{"b.txt", LargeText(), true}});
        bytes = ReadTextFile(dir.File("deflated.zip"));
        bytes[bytes.size() / 3] = static_cast<char>(bytes[bytes.size() / 3] ^ 0x5A);
        WriteTextFile(dir.File("deflated.zip"), bytes);
        error.clear();
        CHECK(!ExtractZipEntries(dir.File("deflated.zip"), dir.path(), {"b.txt"}, &error));
        CHECK(!error.empty());
    }

}  // namespace ql
