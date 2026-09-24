// Small, pure helpers: text, dates, distances, settings files and export
// file handling.

#include <algorithm>
#include <string>
#include <vector>

#include "../src/date_utils.hpp"
#include "../src/file_export.hpp"
#include "../src/geo_utils.hpp"
#include "../src/public_key.hpp"
#include "../src/settings.hpp"
#include "../src/text_utils.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // ---- text_utils ------------------------------------------------------------

    QL_TEST(ToUpperAsciiUppercasesOnlyAsciiLetters)
    {
        CHECK_EQ(ToUpperAscii("w4kwk"), std::string("W4KWK"));
        CHECK_EQ(ToUpperAscii("Mixed Case 123"), std::string("MIXED CASE 123"));
        CHECK_EQ(ToUpperAscii(""), std::string(""));
        // Non-ASCII bytes pass through untouched rather than being mangled.
        CHECK_EQ(ToUpperAscii("lo\xC3\xADza"), std::string("LO\xC3\xADZA"));
    }

    QL_TEST(NormalizeCallsignKeepsOnlyCallsignCharacters)
    {
        CHECK_EQ(NormalizeCallsign("w4kwk"), std::string("W4KWK"));
        CHECK_EQ(NormalizeCallsign(" W4KWK \t"), std::string("W4KWK"));
        CHECK_EQ(NormalizeCallsign("w4kwk/m"), std::string("W4KWK/M"));
        CHECK_EQ(NormalizeCallsign("VE3/W4KWK"), std::string("VE3/W4KWK"));
        CHECK_EQ(NormalizeCallsign("K4%_A'B\"-C"), std::string("K4ABC"));
        CHECK_EQ(NormalizeCallsign("K4\xC3\x89Z"), std::string("K4Z"));
        CHECK_EQ(NormalizeCallsign("   "), std::string(""));
    }

    QL_TEST(NormalizePlaceNameFoldsCaseAccentsAndPeriods)
    {
        CHECK_EQ(NormalizePlaceName("Newton"), std::string("NEWTON"));
        CHECK_EQ(NormalizePlaceName("Lo\xC3\xADza"), std::string("LOIZA"));
        CHECK_EQ(NormalizePlaceName("Pe\xC3\xB1uelas"), std::string("PENUELAS"));
        CHECK_EQ(NormalizePlaceName("St. Louis"), std::string("ST LOUIS"));
        CHECK_EQ(NormalizePlaceName("MAYAG\xC3\x9C"
                                    "EZ"),
                 std::string("MAYAGUEZ"));
    }

    // ---- date_utils ------------------------------------------------------------

    QL_TEST(CurrentDateIsIso8601)
    {
        std::string date = CurrentDateIso8601();
        REQUIRE(date.size() == 10);
        CHECK(date[4] == '-');
        CHECK(date[7] == '-');
    }

    QL_TEST(FormatLocalTimeOfDayIsBlankForUnrecordedTimes)
    {
        CHECK_EQ(FormatLocalTimeOfDay(0), std::string(""));
        CHECK_EQ(FormatLocalTimeOfDay(-5), std::string(""));
    }

    QL_TEST(FormatLocalTimeOfDayIsTwelveHourWithoutSeconds)
    {
        std::string text = FormatLocalTimeOfDay(1790000000);
        REQUIRE(text.size() == 8);  // "hh:mm AM"
        CHECK(text[2] == ':');
        CHECK(text.substr(6) == "AM" || text.substr(6) == "PM");
    }

    QL_TEST(FormatLocalDateIsIsoOrBlank)
    {
        CHECK_EQ(FormatLocalDate(0), std::string(""));
        std::string date = FormatLocalDate(1790000000);
        REQUIRE(date.size() == 10);
        CHECK(date.compare(0, 4, "2026") == 0);
        CHECK(date[4] == '-' && date[7] == '-');
    }

    // ---- public_key ------------------------------------------------------------

    // Throwaway keys made for these tests with ssh-keygen; nothing uses them.
    static const char* kEd25519Key =
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINM3eCDBCkdxto9OIGli2KKno"
        "rIhCylrEpYHnMPxdkAI test@quicklogger";
    static const char* kRsaKey =
        "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQDFLDPZBt02sHALYLmeiR4m17D3gNFE9vtKzBRNfZ+sBxb0Cp/"
        "Izcs9qQMAkUZi6aEQRTW7RPZAkL3kNkcw+/T3bA1N5qTYK9tkbs8qR72boXJcgSp/"
        "LfzgLDmM1A+zVNpmlWXhn4EuhJa"
        "0wG/WiNZoWGEIKjdSCTUfIX4hcsspnQ== rsa@test";
    static const char* kEcdsaKey =
        "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBPYZZ9VZA4tifTMUe"
        "aD4+NLAlPM4vzya7Gu9uPVDpEo2sNfAt3I7zE92dNSClawZGhwfo1iPr+IIYJgRU6d/hzc= ec@test";

    QL_TEST(ValidPublicKeysAreAccepted)
    {
        std::string normalized;
        std::string error;
        CHECK(ValidatePublicKey(kEd25519Key, &normalized, &error));
        CHECK_EQ(normalized, std::string(kEd25519Key));
        CHECK(ValidatePublicKey(kRsaKey, &normalized, &error));
        CHECK(ValidatePublicKey(kEcdsaKey, &normalized, &error));

        // Pasted with stray whitespace and a line break: tidied.
        std::string messy = std::string("  ") + kEd25519Key + "\n";
        std::string::size_type space = messy.find(' ', 3);
        messy.replace(space, 1, " \t ");
        REQUIRE(ValidatePublicKey(messy, &normalized, &error));
        CHECK_EQ(normalized, std::string(kEd25519Key));

        // No comment is fine.
        std::string bare(kEd25519Key);
        bare = bare.substr(0, bare.rfind(' '));
        CHECK(ValidatePublicKey(bare, &normalized, &error));
    }

    static std::string KeyError(const std::string& text)
    {
        std::string normalized;
        std::string error;
        return ValidatePublicKey(text, &normalized, &error) ? std::string("(accepted)") : error;
    }

    QL_TEST(BadPublicKeysAreExplained)
    {
        CHECK(KeyError("").find("Paste the user's public key") != std::string::npos);
        CHECK(KeyError("-----BEGIN OPENSSH PRIVATE KEY----- b3BlbnNzaC1rZXktdjE=")
                  .find("private key") != std::string::npos);
        CHECK(KeyError("---- BEGIN SSH2 PUBLIC KEY ---- AAAAB3Nza").find("ssh-keygen -i") !=
              std::string::npos);

        std::string ed(kEd25519Key);
        std::string data = ed.substr(ed.find(' ') + 1);
        CHECK(KeyError(data).find("key type is missing") != std::string::npos);
        CHECK(KeyError("ssh-dss " + data).find("isn't an SSH key type") != std::string::npos);
        CHECK(KeyError("ssh-ed25519").find("missing") != std::string::npos);

        // Cut short while copying (still a whole number of base64 groups).
        std::string key_data = data.substr(0, data.find(' '));
        CHECK(KeyError("ssh-ed25519 " + key_data.substr(0, key_data.size() - 4)).find("cut off") !=
              std::string::npos);
        // Cut mid-group, and a stray character.
        CHECK(KeyError("ssh-ed25519 " + key_data.substr(0, key_data.size() - 3)) != "(accepted)");
        CHECK(KeyError("ssh-ed25519 " + key_data.substr(0, 10) + "!" + key_data.substr(11)) !=
              "(accepted)");
        // Real key data under the wrong type name.
        CHECK(KeyError("ssh-rsa " + key_data).find("isn't valid") != std::string::npos);
        // Every error shows what a key should look like.
        CHECK(KeyError("ssh-rsa " + key_data).find("like: ssh-ed25519") != std::string::npos);
    }

    // ---- geo_utils -------------------------------------------------------------

    QL_TEST(DistanceMilesMatchesKnownDistances)
    {
        CHECK(DistanceMiles(35.0, -85.0, 35.0, -85.0) < 0.001);
        // Chattanooga to Atlanta is about 106 miles in a straight line.
        double miles = DistanceMiles(35.0456, -85.3097, 33.7490, -84.3880);
        CHECK(miles > 100.0 && miles < 112.0);
        // Symmetric.
        CHECK(DistanceMiles(33.7490, -84.3880, 35.0456, -85.3097) - miles < 0.0001);
    }

    QL_TEST(NearbyZip3PrefixesKeepsOnlyPrefixesInRange)
    {
        std::vector<ZipCentroid> centroids;
        ZipCentroid near_one;
        near_one.zip = "37415";
        near_one.lat = 35.1;
        near_one.lon = -85.3;
        ZipCentroid near_two;
        near_two.zip = "30752";
        near_two.lat = 34.87;
        near_two.lon = -85.51;
        ZipCentroid far;
        far.zip = "90210";
        far.lat = 34.09;
        far.lon = -118.4;
        ZipCentroid malformed;
        malformed.zip = "12";
        centroids.push_back(near_one);
        centroids.push_back(near_two);
        centroids.push_back(far);
        centroids.push_back(malformed);

        std::vector<std::string> prefixes = NearbyZip3Prefixes(35.05, -85.31, centroids);
        CHECK_EQ(prefixes.size(), std::size_t{2});
        CHECK(std::find(prefixes.begin(), prefixes.end(), "374") != prefixes.end());
        CHECK(std::find(prefixes.begin(), prefixes.end(), "307") != prefixes.end());
        CHECK(std::find(prefixes.begin(), prefixes.end(), "902") == prefixes.end());
    }

    QL_TEST(NearbyZipsAreWithinRangeAndNearestFirst)
    {
        std::vector<ZipCentroid> centroids = {{"30752", 34.87, -85.51},
                                              {"37415", 35.10, -85.28},
                                              {"90210", 34.09, -118.40},
                                              {"37402", 35.05, -85.31}};
        std::vector<NearbyZip> nearby = NearbyZips(35.10, -85.28, centroids);
        REQUIRE(nearby.size() == 3);
        CHECK_EQ(nearby[0].zip, std::string("37415"));
        CHECK(nearby[0].miles < 0.01);
        CHECK_EQ(nearby[1].zip, std::string("37402"));
        CHECK_EQ(nearby[2].zip, std::string("30752"));
        CHECK(nearby[1].miles < nearby[2].miles);
    }

    // ---- settings --------------------------------------------------------------

    QL_TEST(SettingsRoundTripThroughTheFile)
    {
        TempDir dir;
        AppSettings settings;
        settings.callsign = "W4KWK";
        settings.location = "37415";
        SaveSettings(dir.File("settings.txt"), settings);

        AppSettings loaded = LoadSettings(dir.File("settings.txt"));
        CHECK_EQ(loaded.callsign, std::string("W4KWK"));
        CHECK_EQ(loaded.location, std::string("37415"));
    }

    QL_TEST(SettingsFromAMissingFileAreEmpty)
    {
        TempDir dir;
        AppSettings loaded = LoadSettings(dir.File("does-not-exist.txt"));
        CHECK(loaded.callsign.empty());
        CHECK(loaded.location.empty());
        CHECK(!SettingsAreComplete(loaded));
    }

    QL_TEST(SettingsToleratesJunkAndWhitespace)
    {
        TempDir dir;
        WriteTextFile(dir.File("settings.txt"),
                      "garbage line without equals\n  callsign =  N4XYZ  \r\nlocation=30301\n"
                      "unknown=value\n");
        AppSettings loaded = LoadSettings(dir.File("settings.txt"));
        CHECK_EQ(loaded.callsign, std::string("N4XYZ"));
        CHECK_EQ(loaded.location, std::string("30301"));
    }

    QL_TEST(SettingsScrubsOldQrzCredentials)
    {
        TempDir dir;
        WriteTextFile(dir.File("settings.txt"),
                      "callsign=W4KWK\nqrz_username=someone\nqrz_password=secret\n"
                      "location=37415\n");
        AppSettings loaded = LoadSettings(dir.File("settings.txt"));
        CHECK_EQ(loaded.callsign, std::string("W4KWK"));
        std::string rewritten = ReadTextFile(dir.File("settings.txt"));
        CHECK(rewritten.find("qrz") == std::string::npos);
        CHECK(rewritten.find("secret") == std::string::npos);
        CHECK(rewritten.find("callsign=W4KWK") != std::string::npos);
    }

    QL_TEST(SettingsCompleteNeedsCallsignAndFiveDigitZip)
    {
        AppSettings settings;
        settings.callsign = "W4KWK";
        settings.location = "37415";
        CHECK(SettingsAreComplete(settings));
        settings.location = "3741";
        CHECK(!SettingsAreComplete(settings));
        settings.location = "3741a";
        CHECK(!SettingsAreComplete(settings));
        settings.location = "37415-1234";
        CHECK(!SettingsAreComplete(settings));
        settings.location = "37415";
        settings.callsign = "";
        CHECK(!SettingsAreComplete(settings));
    }

    // ---- file_export -----------------------------------------------------------

    QL_TEST(SanitizeFilenameComponentMakesSafeNames)
    {
        CHECK_EQ(SanitizeFilenameComponent("220 EOR net"), std::string("220_EOR_net"));
        CHECK_EQ(SanitizeFilenameComponent("a/b\\c:d"), std::string("a_b_c_d"));
        CHECK_EQ(SanitizeFilenameComponent("  !!  "), std::string("export"));
        CHECK_EQ(SanitizeFilenameComponent(""), std::string("export"));
        CHECK_EQ(SanitizeFilenameComponent("$(rm -rf /)"), std::string("rm_-rf"));
        CHECK_EQ(SanitizeFilenameComponent("2026-09-24"), std::string("2026-09-24"));
    }

    QL_TEST(ExportAndImportDirectoriesSitNextToTheDatabase)
    {
        CHECK_EQ(ExportsDir("quicklogger.db"), std::string("./exports"));
        CHECK_EQ(ExportsDir("/data/ql/quicklogger.db"), std::string("/data/ql/exports"));
        CHECK_EQ(ImportsDir("/data/ql/quicklogger.db"), std::string("/data/ql/imports"));
    }

    QL_TEST(WriteExportFileCreatesMissingDirectories)
    {
        TempDir dir;
        std::string path = dir.File("a/b c/log.txt");
        std::string error;
        CHECK(WriteExportFile(path, {"one", "two"}, &error));
        CHECK_EQ(ReadTextFile(path), std::string("one\ntwo\n"));
        CHECK(error.empty());
    }

    QL_TEST(WriteExportFileReportsAnUnwritablePath)
    {
        TempDir dir;
        WriteTextFile(dir.File("a-file"), "x");
        std::string error;
        // A path "inside" a regular file can't be created.
        CHECK(!WriteExportFile(dir.File("a-file/log.txt"), {"one"}, &error));
        CHECK(!error.empty());
    }

    QL_TEST(ListFilesWithExtensionIsSortedAndFiltered)
    {
        TempDir dir;
        WriteTextFile(dir.File("b.qlnet"), "");
        WriteTextFile(dir.File("a.qlnet"), "");
        WriteTextFile(dir.File("c.txt"), "");
        EnsureDirectory(dir.File("d.qlnet"));  // A directory, not a file.
        std::vector<std::string> names = ListFilesWithExtension(dir.path(), ".qlnet");
        REQUIRE(names.size() == 2);
        CHECK_EQ(names[0], std::string("a.qlnet"));
        CHECK_EQ(names[1], std::string("b.qlnet"));
        CHECK(ListFilesWithExtension(dir.File("missing"), ".qlnet").empty());
    }

}  // namespace ql
