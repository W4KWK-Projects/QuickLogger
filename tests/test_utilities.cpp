// Small, pure helpers: text, dates, distances, settings files and export
// file handling.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/adif_export.hpp"
#include "../src/callsign_rules.hpp"
#include "../src/date_utils.hpp"
#include "../src/file_export.hpp"
#include "../src/frequency_rules.hpp"
#include "../src/geo_utils.hpp"
#include "../src/mode_rules.hpp"
#include "../src/public_key.hpp"
#include "../src/settings.hpp"
#include "../src/text_utils.hpp"
#include "../src/update_check.hpp"
#include "../src/version.hpp"
#include "../src/zmodem_send.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    // ---- modes -----------------------------------------------------------------

    QL_TEST(OldFreeTextModesBecomeAKnownModeOrBlank)
    {
        CHECK_EQ(NormalizeMode("fm"), std::string("FM"));
        CHECK_EQ(NormalizeMode(" FM "), std::string("FM"));
        CHECK_EQ(NormalizeMode("usb"), std::string("SSB"));
        CHECK_EQ(NormalizeMode("D-Star"), std::string("D-STAR"));
        CHECK_EQ(NormalizeMode("dstar"), std::string("D-STAR"));
        CHECK_EQ(NormalizeMode("YSF"), std::string("Fusion"));
        CHECK_EQ(NormalizeMode("C4FM"), std::string("Fusion"));
        CHECK_EQ(NormalizeMode("System Fusion"), std::string("Fusion"));
        CHECK_EQ(NormalizeMode("dmr"), std::string("DMR"));
        CHECK_EQ(NormalizeMode("Digital"), std::string(""));
        CHECK_EQ(NormalizeMode("FM & DMR"), std::string(""));
        CHECK_EQ(NormalizeMode("FM, SSB"), std::string(""));
        CHECK_EQ(NormalizeMode(""), std::string(""));
        for (const std::string& mode : NetModes())
        {
            CHECK_EQ(NormalizeMode(mode), mode);
        }
    }

    QL_TEST(ModesMapToAdifModeAndSubmode)
    {
        std::string mode;
        std::string submode;
        AdifMode("SSB", &mode, &submode);
        CHECK_EQ(mode, std::string("SSB"));
        CHECK_EQ(submode, std::string(""));
        AdifMode("D-STAR", &mode, &submode);
        CHECK_EQ(mode, std::string("DIGITALVOICE"));
        CHECK_EQ(submode, std::string("DSTAR"));
        AdifMode("DMR", &mode, &submode);
        CHECK_EQ(submode, std::string("DMR"));
        AdifMode("Fusion", &mode, &submode);
        CHECK_EQ(submode, std::string("C4FM"));
        AdifMode("", &mode, &submode);
        CHECK_EQ(mode, std::string(""));
        CHECK_EQ(submode, std::string(""));
    }

    // ---- ADIF ------------------------------------------------------------------

    QL_TEST(AdifFieldsAreAsciiWithByteLengths)
    {
        CHECK_EQ(FoldToAscii("Jos\xC3\xA9 Mu\xC3\xB1oz"), std::string("Jose Munoz"));
        CHECK_EQ(FoldToAscii("Stra\xC3\x9F"
                             "e \xC5\x81\xC3\xB3"
                             "d\xC5\xBA"),
                 std::string("Strasse Lodz"));
        CHECK_EQ(FoldToAscii("It\xE2\x80\x99s \xE2\x80\x9Cok\xE2\x80\x9D"), std::string("It's \"ok\""));
        CHECK_EQ(FoldToAscii("snow \xE2\x9D\x84"), std::string("snow "));

        CHECK_EQ(FirstNameFirst("Shults, Roger D"), std::string("Roger D Shults"));
        CHECK_EQ(FirstNameFirst("Stansberry Jr, Roger L"), std::string("Roger L Stansberry Jr"));
        CHECK_EQ(FirstNameFirst("Wes Keene"), std::string("Wes Keene"));
        CHECK_EQ(FirstNameFirst("Smith, John, Jr"), std::string("Smith, John, Jr"));
        CHECK_EQ(FirstNameFirst("Smith,"), std::string("Smith"));
        CHECK_EQ(AdifBand("146.940"), std::string("2m"));
        CHECK_EQ(AdifBand("7.235"), std::string("40m"));
        CHECK_EQ(AdifBand("443.500"), std::string("70cm"));
        CHECK_EQ(AdifBand("100"), std::string(""));
        CHECK_EQ(AdifBand(""), std::string(""));

        CheckIn check_in;
        Station station;
        check_in.callsign = "K4ABC";
        check_in.checked_in_at = 1790000000;  // 2026-09-21 14:13:20 UTC.
        check_in.signal_report = "59";
        check_in.remarks = "Mobile";
        station.name = "Dupont, Ren\xC3\xA9";
        station.city = "Chattanooga";
        station.state = "TN";
        station.county = "Hamilton";
        station.grid_square = "EM75";
        AdifContact contact;
        contact.check_in = &check_in;
        contact.station = &station;
        std::string adif = BuildAdif({contact}, "DMR", "443.500", "W4KWK", "2026-09-21", 1790000000);
        CHECK(adif.find("<ADIF_VER:5>3.1.4 ") != std::string::npos);
        CHECK(adif.find("<EOH>") != std::string::npos);
        CHECK(adif.find("<CALL:5>K4ABC <QSO_DATE:8>20260921 <TIME_ON:6>141320 ") != std::string::npos);
        CHECK(adif.find("<FREQ:7>443.500 <BAND:4>70cm <MODE:12>DIGITALVOICE <SUBMODE:3>DMR ") != std::string::npos);
        CHECK(adif.find("<STATION_CALLSIGN:5>W4KWK <RST_RCVD:2>59 <NAME:11>Rene Dupont ") != std::string::npos);
        CHECK(adif.find("<CNTY:11>TN,Hamilton <GRIDSQUARE:4>EM75 <COMMENT:6>Mobile <EOR>") != std::string::npos);
        CHECK(adif.find("NOTES") == std::string::npos);  // Empty fields are left out.

        // No mode, no frequency and no check-in time: those fields are left
        // out, and the date is the session's.
        check_in.checked_in_at = 0;
        adif = BuildAdif({contact}, "", "", "W4KWK", "2026-09-21", 1790000000);
        CHECK(adif.find("<QSO_DATE:8>20260921 <STATION_CALLSIGN") != std::string::npos);
        CHECK(adif.find("<MODE") == std::string::npos);
        CHECK(adif.find("<FREQ") == std::string::npos);
    }

    // ---- update check ----------------------------------------------------------

    QL_TEST(TheLatestReleasesVersionIsReadFromGitHubsReply)
    {
        CHECK_EQ(ReleaseVersionFromJson("{\"url\": \"x\", \"tag_name\": \"v1.8.0\", \"name\": \"QuickLogger 1.8.0\"}"),
                 std::string("1.8.0"));
        CHECK_EQ(ReleaseVersionFromJson("{\"tag_name\":\"1.7.10\"}"), std::string("1.7.10"));
        CHECK_EQ(ReleaseVersionFromJson("{\"message\": \"Not Found\"}"), std::string(""));
        CHECK_EQ(ReleaseVersionFromJson("{\"tag_name\": "), std::string(""));
    }

    QL_TEST(VersionsCompareByNumberNotText)
    {
        CHECK(IsNewerVersion("1.7.10", "1.7.9"));
        CHECK(!IsNewerVersion("1.7.9", "1.7.10"));
        CHECK(IsNewerVersion("1.8.0", "1.7.9"));
        CHECK(IsNewerVersion("2.0.0", "1.9.9"));
        CHECK(IsNewerVersion("1.8.1", "1.8"));
        CHECK(!IsNewerVersion("1.8", "1.8.0"));
        CHECK(!IsNewerVersion("1.7.9", "1.7.9"));
        // Anything that isn't a plain version is never newer.
        CHECK(!IsNewerVersion("1.8.0-beta", "1.7.9"));
        CHECK(!IsNewerVersion("", "1.7.9"));
        CHECK(!IsNewerVersion("1..8", "1.7.9"));
    }

    QL_TEST(TurningTheUpdateCheckOffHidesWhatItFound)
    {
        SetUpdateCheckEnabled(true);
        SetAvailableUpdate("9.9.9");
        CHECK_EQ(AvailableUpdate(), std::string("9.9.9"));
        SetUpdateCheckEnabled(false);
        CHECK_EQ(AvailableUpdate(), std::string(""));
        SetAvailableUpdate("");
        SetUpdateCheckEnabled(true);
    }

    // ---- version ---------------------------------------------------------------

    QL_TEST(VersionComesFromTheBuild)
    {
        std::string version = QuickLoggerVersion();
        CHECK(version != "unknown");
        // major.minor.patch
        CHECK_EQ(std::count(version.begin(), version.end(), '.'), std::ptrdiff_t{2});
        CHECK(version.find_first_not_of("0123456789.") == std::string::npos);
    }

    // ---- text_utils ------------------------------------------------------------

    QL_TEST(ToUpperAsciiUppercasesOnlyAsciiLetters)
    {
        CHECK_EQ(ToUpperAscii("w4kwk"), std::string("W4KWK"));
        CHECK_EQ(ToUpperAscii("Mixed Case 123"), std::string("MIXED CASE 123"));
        CHECK_EQ(ToUpperAscii(""), std::string(""));
        // Non-ASCII bytes pass through untouched rather than being mangled.
        CHECK_EQ(ToUpperAscii("lo\xC3\xADza"), std::string("LO\xC3\xADZA"));
    }

    QL_TEST(NetNamesAreTheSameIgnoringCapitalsAndSpaces)
    {
        CHECK(NetNamesAreTheSame("Skywarn", "SKYWARN"));
        CHECK(NetNamesAreTheSame(" Skywarn ", "skywarn"));
        CHECK(NetNamesAreTheSame("TAG  Skywarn", "tag skywarn"));
        CHECK(!NetNamesAreTheSame("TAG Skywarn", "TAGSkywarn"));
        CHECK(!NetNamesAreTheSame("Hamilton Co. ARES", "Hamilton County ARES"));
    }

    QL_TEST(NetNamesLookAlikeIsGenerous)
    {
        // The same net, written differently.
        CHECK(NetNamesLookAlike("TAG Skywarn", "TAG Skywarn"));
        CHECK(NetNamesLookAlike("TAG Skywarn", "tag skywarn net"));
        CHECK(NetNamesLookAlike("Hamilton Co. ARES Net", "Hamilton County ARES"));
        CHECK(NetNamesLookAlike("220 EOR net", "220EOR"));
        CHECK(NetNamesLookAlike("TAG Skywarn", "Sky Warn"));
        // Only one word in common.
        CHECK(NetNamesLookAlike("TAG Skywarn", "Skywarn Weekly Net"));
        CHECK(NetNamesLookAlike("Hamilton Co. ARES Net", "Bradley County ARES"));
        CHECK(NetNamesLookAlike("Dixie Traders Net", "Dixie Swap Net"));
        // Typos, and a shortened word.
        CHECK(NetNamesLookAlike("TAG Skywarn", "TAG Skywran"));
        CHECK(NetNamesLookAlike("Hamilton Co. ARES Net", "Hamliton ARES"));
        CHECK(NetNamesLookAlike("Dixie Traders Net", "Dixie Trader"));
        // Acronyms.
        CHECK(NetNamesLookAlike("TAG Skywarn", "Tennessee Alabama Georgia Net"));
        CHECK(NetNamesLookAlike("CARC Net", "Chattanooga Amateur Radio Club"));
        CHECK(NetNamesLookAlike("Amateur Radio Emergency Service", "ARES"));
        // Accents don't matter.
        CHECK(NetNamesLookAlike("Réseau Québec", "Reseau Quebec"));
        // Nothing but common words: no way to tell, so alike.
        CHECK(NetNamesLookAlike("Weekly Net", "TAG Skywarn"));
        CHECK(!NetNamesLookAlike("Weekly Net", "TAG Skywarn", false));
        // Unless they're the same.
        CHECK(NetNamesLookAlike("Weekly Net", "weekly  net", false));

        // Different nets.
        CHECK(!NetNamesLookAlike("TAG Skywarn", "Hamilton County ARES"));
        CHECK(!NetNamesLookAlike("TAG Skywarn", "Dixie Traders Net"));
        CHECK(!NetNamesLookAlike("220 EOR net", "Hamilton Co. ARES Net"));
        CHECK(!NetNamesLookAlike("220 EOR net", "440 Net"));
        CHECK(!NetNamesLookAlike("Hamilton County ARES", "Bradley County RACES"));
    }

    QL_TEST(ExtractZipCodeFindsTheZipInFreeText)
    {
        CHECK_EQ(ExtractZipCode("37415"), std::string("37415"));
        CHECK_EQ(ExtractZipCode("Chattanooga, TN 37415"), std::string("37415"));
        CHECK_EQ(ExtractZipCode("37415-2623"), std::string("37415"));
        CHECK_EQ(ExtractZipCode("374152623"), std::string("37415"));
        CHECK_EQ(ExtractZipCode("12345 Main St, Chattanooga 37415"), std::string("37415"));
        CHECK_EQ(ExtractZipCode("1234 Main St"), std::string(""));
        CHECK_EQ(ExtractZipCode("Hamilton County"), std::string(""));
        CHECK_EQ(ExtractZipCode("TN"), std::string(""));
        CHECK_EQ(ExtractZipCode(""), std::string(""));
    }

    QL_TEST(CallsignRulesAcceptUsAndCanadianFormats)
    {
        CHECK(IsValidCallsign("K1A"));
        CHECK(IsValidCallsign("W0Z"));
        CHECK(IsValidCallsign("N5Y"));
        CHECK(IsValidCallsign("W1AW"));
        CHECK(IsValidCallsign("N1NJA"));
        CHECK(IsValidCallsign("AB0C"));
        CHECK(IsValidCallsign("AL7A"));
        CHECK(IsValidCallsign("AK6SE"));
        CHECK(IsValidCallsign("NZ9WA"));
        CHECK(IsValidCallsign("KA2DOG"));
        CHECK(IsValidCallsign("WB4XYZ"));
        CHECK(IsValidCallsign("KL7ABC"));
        CHECK(IsValidCallsign("NH6ABC"));
        CHECK(IsValidCallsign("NP4ABC"));
        CHECK(IsValidCallsign("KP4AB"));
        CHECK(IsValidCallsign("KH6ABC"));
        CHECK(IsValidCallsign("WP4ABC"));
        CHECK(IsValidCallsign("AF1EMA"));
        CHECK(IsValidCallsign("NF9EMA"));
        CHECK(IsValidCallsign("VE3ABC"));
        CHECK(IsValidCallsign("VA3XY"));
        CHECK(IsValidCallsign("VE0ABC"));
        CHECK(IsValidCallsign("VO1ZZ"));
        CHECK(IsValidCallsign("VY2ZZZ"));
        CHECK(IsValidCallsign("VY0AB"));
        CHECK(IsValidCallsign("VY9AA"));
        CHECK(IsValidCallsign("CY0AAA"));
        CHECK(IsValidCallsign("CY9SS"));
        CHECK(IsValidCallsign("CG3A"));
        CHECK(IsValidCallsign("XM3DEF"));
        CHECK(IsValidCallsign("VC3ABCDE"));
        CHECK(IsValidCallsign("VE2008VQ"));
        CHECK(IsValidCallsign("CG200I"));
        CHECK(IsValidCallsign("W4KWK/M"));
        CHECK(IsValidCallsign("W4KWK/P"));
        CHECK(IsValidCallsign("W4KWK/MM"));
        CHECK(IsValidCallsign("W4KWK/AE"));
        CHECK(IsValidCallsign("W4KWK/QRP"));
        CHECK(IsValidCallsign("W4KWK/4"));
        CHECK(IsValidCallsign("VE3/W4KWK"));
        CHECK(IsValidCallsign("KH6/VE3ABC"));
        CHECK(IsValidCallsign("VE3ABC/W4"));
        CHECK(IsValidCallsign("W4KWK/VE3"));
        CHECK(IsValidCallsign("VE3/W4KWK/P"));
    }

    QL_TEST(CallsignRulesRejectEverythingElse)
    {
        CHECK(!IsValidCallsign(""));
        CHECK(!IsValidCallsign("W4"));
        CHECK(!IsValidCallsign("4KWK"));
        CHECK(!IsValidCallsign("W4KWK4"));
        CHECK(!IsValidCallsign("X1AB"));
        CHECK(!IsValidCallsign("G4ABC"));
        CHECK(!IsValidCallsign("DL1ABC"));
        CHECK(!IsValidCallsign("K1X"));
        CHECK(!IsValidCallsign("KWK"));
        CHECK(!IsValidCallsign("W44KWK"));
        CHECK(!IsValidCallsign("AM1AB"));
        CHECK(!IsValidCallsign("W1ABCD"));
        CHECK(!IsValidCallsign("KA1ABCD"));
        CHECK(!IsValidCallsign("VE3"));
        CHECK(!IsValidCallsign("VA8ABC"));
        CHECK(!IsValidCallsign("VO3AB"));
        CHECK(!IsValidCallsign("VY3AB"));
        CHECK(!IsValidCallsign("CG0AB"));
        CHECK(!IsValidCallsign("VE3ABCDEF"));
        CHECK(!IsValidCallsign("VE12345A"));
        CHECK(!IsValidCallsign("W4KWK/"));
        CHECK(!IsValidCallsign("/W4KWK"));
        CHECK(!IsValidCallsign("W4KWK/QRPP"));
        CHECK(!IsValidCallsign("W4KWK/12"));
        CHECK(!IsValidCallsign("ZZ3/W4KWK"));
        CHECK(!IsValidCallsign("W4KWK/M/P/X"));
        CHECK(!IsValidCallsign("W4-KWK"));
    }

    QL_TEST(BaseCallsignDropsPortableIndicators)
    {
        CHECK_EQ(BaseCallsign("W4KWK"), std::string("W4KWK"));
        CHECK_EQ(BaseCallsign("W4KWK/M"), std::string("W4KWK"));
        CHECK_EQ(BaseCallsign("W4KWK/QRP"), std::string("W4KWK"));
        CHECK_EQ(BaseCallsign("VE3/W4KWK"), std::string("W4KWK"));
        CHECK_EQ(BaseCallsign("VE3ABC/W4"), std::string("VE3ABC"));
        CHECK_EQ(BaseCallsign("KH6/VE3ABC/P"), std::string("VE3ABC"));
        CHECK_EQ(BaseCallsign("NOT/A/CALL/AT/ALL"), std::string("A/CALL/AT"));
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

    QL_TEST(TimesCanBeShownOnThe24HourClock)
    {
        std::int64_t evening = 1790000000;  // Some moment; compare against strftime.
        std::tm local = LocalTime(static_cast<std::time_t>(evening));
        char expected[8];
        std::strftime(expected, sizeof(expected), "%H:%M", &local);

        SetUse24HourClock(true);
        std::string time = FormatLocalTimeOfDay(evening);
        std::string date_time = FormatLocalDateTime(evening);
        CHECK(Use24HourClock());
        SetUse24HourClock(false);  // Back to the default for other tests.

        CHECK_EQ(time, std::string(expected));
        CHECK_EQ(date_time, FormatLocalDate(evening) + " " + expected);
        CHECK_EQ(FormatLocalTimeOfDay(evening).size(), std::size_t{8});  // "hh:mm AM"
        CHECK_EQ(FormatLocalDateTime(0), std::string(""));
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

    QL_TEST(PublicKeysAreDescribedAsSshKeygenDoes)
    {
        // Each as `ssh-keygen -lf` prints it.
        PublicKeyDescription ed = DescribePublicKey(kEd25519Key);
        CHECK_EQ(ed.type, std::string("ED25519"));
        CHECK_EQ(ed.fingerprint, std::string("SHA256:zSpp/AdOije1VljAESUMw2HrLA8m9pAYYD3YqHP8Y9A"));
        CHECK_EQ(ed.comment, std::string("test@quicklogger"));
        PublicKeyDescription rsa = DescribePublicKey(kRsaKey);
        CHECK_EQ(rsa.type, std::string("RSA"));
        CHECK_EQ(rsa.fingerprint, std::string("SHA256:+A3eCzoJ5Tzphu/8vW+qS23AobtDrXKaK1e7HYdIsUs"));
        PublicKeyDescription ecdsa = DescribePublicKey(kEcdsaKey);
        CHECK_EQ(ecdsa.type, std::string("ECDSA"));
        CHECK_EQ(ecdsa.fingerprint, std::string("SHA256:19j6mG1JJyDgu5DJDnVUXpAVtv9x7pNW6gkRARUmDVk"));
        CHECK_EQ(ecdsa.comment, std::string("ec@test"));

        std::string bare(kEd25519Key);
        bare = bare.substr(0, bare.rfind(' '));
        CHECK(DescribePublicKey(bare).comment.empty());
        CHECK(SamePublicKey(bare, kEd25519Key));
        CHECK(SamePublicKey(bare + " other@comment", kEd25519Key));
        CHECK(!SamePublicKey(kRsaKey, kEd25519Key));
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
        CHECK(KeyError("-----BEGIN OPENSSH PRIVATE KEY----- b3BlbnNzaC1rZXktdjE=").find("private key") !=
              std::string::npos);
        CHECK(KeyError("---- BEGIN SSH2 PUBLIC KEY ---- AAAAB3Nza").find("ssh-keygen -i") != std::string::npos);

        std::string ed(kEd25519Key);
        std::string data = ed.substr(ed.find(' ') + 1);
        CHECK(KeyError(data).find("key type is missing") != std::string::npos);
        CHECK(KeyError("ssh-dss " + data).find("isn't an SSH key type") != std::string::npos);
        CHECK(KeyError("ssh-ed25519").find("missing") != std::string::npos);

        // Cut short while copying (still a whole number of base64 groups).
        std::string key_data = data.substr(0, data.find(' '));
        CHECK(KeyError("ssh-ed25519 " + key_data.substr(0, key_data.size() - 4)).find("cut off") != std::string::npos);
        // Cut mid-group, and a stray character.
        CHECK(KeyError("ssh-ed25519 " + key_data.substr(0, key_data.size() - 3)) != "(accepted)");
        CHECK(KeyError("ssh-ed25519 " + key_data.substr(0, 10) + "!" + key_data.substr(11)) != "(accepted)");
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

    QL_TEST(PostalCodesAreAcceptedWhereZipsAre)
    {
        CHECK(IsZipOrPostalCode("37415"));
        CHECK(IsZipOrPostalCode("K1A 0B1"));
        CHECK(IsZipOrPostalCode("k1a0b1"));
        CHECK(IsZipOrPostalCode("K1A"));
        CHECK(!IsZipOrPostalCode("3741"));
        CHECK(!IsZipOrPostalCode("K1A 0B"));
        CHECK(!IsZipOrPostalCode("1A1 0B1"));
        CHECK(!IsZipOrPostalCode("374152623"));
        CHECK_EQ(NormalizeZipOrPostalCode("k1a0b1"), std::string("K1A 0B1"));
        CHECK_EQ(NormalizeZipOrPostalCode("m5v 2t6"), std::string("M5V 2T6"));
        CHECK_EQ(NormalizeZipOrPostalCode("37415"), std::string("37415"));
        CHECK_EQ(ExtractZipCode("Ottawa ON K1A0B1"), std::string("K1A 0B1"));
        CHECK_EQ(ExtractZipCode("Ottawa, ON  k1a 0b1 Canada"), std::string("K1A 0B1"));
        CHECK_EQ(ExtractZipCode("Chattanooga, TN 37415-2623"), std::string("37415"));
        CHECK_EQ(ExtractZipCode("Hamilton County"), std::string());
    }

    QL_TEST(ZipCentroidKeyIsAZipOrAnFsa)
    {
        CHECK_EQ(ZipCentroidKey("37415"), std::string("37415"));
        CHECK_EQ(ZipCentroidKey("374152623"), std::string("37415"));
        CHECK_EQ(ZipCentroidKey("K1A 0B1"), std::string("K1A"));
        CHECK_EQ(ZipCentroidKey("m5v2t6"), std::string("M5V"));
        CHECK_EQ(ZipCentroidKey("K1A"), std::string("K1A"));
        CHECK_EQ(ZipCentroidKey("3741"), std::string());
        CHECK_EQ(ZipCentroidKey("1A1 0B1"), std::string());
        CHECK_EQ(ZipCentroidKey(""), std::string());
    }

    QL_TEST(MaidenheadGrid4MatchesKnownLocations)
    {
        // Well-known reference points.
        CHECK_EQ(MaidenheadGrid4(35.0456, -85.3097), std::string("EM75"));   // Chattanooga
        CHECK_EQ(MaidenheadGrid4(41.7145, -72.7278), std::string("FN31"));   // ARRL, Newington
        CHECK_EQ(MaidenheadGrid4(34.0522, -118.2437), std::string("DM04"));  // Los Angeles
        CHECK_EQ(MaidenheadGrid4(47.6062, -122.3321), std::string("CN87"));  // Seattle
        CHECK_EQ(MaidenheadGrid4(21.3069, -157.8583), std::string("BL11"));  // Honolulu
        CHECK_EQ(MaidenheadGrid4(-33.8688, 151.2093), std::string("QF56"));  // Sydney
        CHECK_EQ(MaidenheadGrid4(51.5074, -0.1278), std::string("IO91"));    // London
        // The corners of the world.
        CHECK_EQ(MaidenheadGrid4(-90.0, -180.0), std::string("AA00"));
        CHECK_EQ(MaidenheadGrid4(90.0, 180.0), std::string("RR99"));
        // Off the globe.
        CHECK_EQ(MaidenheadGrid4(91.0, 0.0), std::string());
        CHECK_EQ(MaidenheadGrid4(0.0, 181.0), std::string());
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

        std::vector<std::string> prefixes = NearbyZip3Prefixes(35.05, -85.31, 70.0, centroids);
        CHECK_EQ(prefixes.size(), std::size_t{2});
        CHECK(std::find(prefixes.begin(), prefixes.end(), "374") != prefixes.end());
        CHECK(std::find(prefixes.begin(), prefixes.end(), "307") != prefixes.end());
        CHECK(std::find(prefixes.begin(), prefixes.end(), "902") == prefixes.end());
    }

    QL_TEST(NearbyZipsAreWithinRangeAndNearestFirst)
    {
        std::vector<ZipCentroid> centroids = {
            {"30752", 34.87, -85.51}, {"37415", 35.10, -85.28}, {"90210", 34.09, -118.40}, {"37402", 35.05, -85.31}};
        std::vector<NearbyZip> nearby = NearbyZips(35.10, -85.28, 70.0, centroids);
        REQUIRE(nearby.size() == 3);
        CHECK_EQ(nearby[0].zip, std::string("37415"));
        CHECK(nearby[0].miles < 0.01);
        CHECK_EQ(nearby[1].zip, std::string("37402"));
        CHECK_EQ(nearby[2].zip, std::string("30752"));
        CHECK(nearby[1].miles < nearby[2].miles);

        // A smaller radius leaves out Trenton, about 17 miles away.
        CHECK_EQ(NearbyZips(35.10, -85.28, 10.0, centroids).size(), std::size_t{2});
    }

    // ---- frequencies -----------------------------------------------------------

    QL_TEST(AmateurFrequenciesAreInUsOrCanadianBands)
    {
        for (const char* good : {"146.940", "145.39", "7.235", "3.940", "14.3", "28", "50.125", "223.400", "446.000",
                                 "1296.1", "5.3305", "0.1375", "10368"})
        {
            CHECK(IsAmateurFrequency(good));
        }
        for (const char* bad :
             {"", "162.550", "27.185", "7.301", "148.001", "600", "146.9400001", "146..9", ".5", "146.940 MHz", "abc"})
        {
            CHECK(!IsAmateurFrequency(bad));
        }
        // The band edges themselves count.
        CHECK(IsAmateurFrequency("144.000"));
        CHECK(IsAmateurFrequency("148"));
        CHECK(IsAmateurFrequency("1.8"));
        CHECK(IsAmateurFrequency("146"));  // Whole MHz is fine.
    }

    QL_TEST(FrequencyProblemsSayWhatsWrong)
    {
        CHECK(FrequencyProblem("").empty());
        CHECK(FrequencyProblem("146.940").empty());
        CHECK(FrequencyProblem("162.550").find("isn't in a US or Canadian amateur band") != std::string::npos);
        CHECK(FrequencyProblem("146.9.4").find("in MHz") != std::string::npos);
    }

    QL_TEST(OffsetsAreSignedMhz)
    {
        for (const char* good : {"", "-0.6", "+0.6", "+5", "-5.000", "-1.6", "-0.5", "-12"})
        {
            CHECK(OffsetProblem(good, "").empty());
        }
        for (const char* bad : {"-", "+", "0.6", "600", "-0", "+0.000", "-.6", "- 0.6"})
        {
            CHECK(!OffsetProblem(bad, "").empty());
        }
        // kHz gets the MHz to type instead.
        CHECK(OffsetProblem("-600", "").find("for 600 kHz, type -0.6") != std::string::npos);
        CHECK(OffsetProblem("-1600", "").find("type -1.6") != std::string::npos);
        CHECK(OffsetProblem("+5000", "").find("type +5.") != std::string::npos);
        // Where the repeater listens must be amateur too.
        CHECK(OffsetProblem("-0.6", "146.940").empty());
        CHECK(OffsetProblem("+5", "444.100").empty());
        CHECK(OffsetProblem("+0.6", "147.900").find("outside the amateur bands") != std::string::npos);
        CHECK(OffsetProblem("-0.6", "144.300").find("144.300 MHz -0.6") != std::string::npos);
    }

    QL_TEST(PlTonesAreStandardCtcssTones)
    {
        for (const char* good : {"67.0", "67", "100.0", "100", "88.5", "254.1", "150.0"})
        {
            CHECK(IsCtcssTone(good));
        }
        for (const char* bad : {"", "100.5", "66.9", "255.0", "88.50", "D023", "1000", ".5"})
        {
            CHECK(!IsCtcssTone(bad));
        }
        CHECK(ToneProblem("").empty());
        CHECK(ToneProblem("101").find("standard CTCSS tone") != std::string::npos);
        CHECK_EQ(NormalizeTone("100"), std::string("100.0"));
        CHECK_EQ(NormalizeTone("88.5"), std::string("88.5"));
    }

    QL_TEST(AnOldFreeTextFrequencyMovesToTheComments)
    {
        std::string frequency = "146.940 -600 PL 100";
        std::string comments;
        CHECK(MoveBadFrequencyToComments(&frequency, &comments));
        CHECK_EQ(frequency, std::string("146.940"));
        CHECK_EQ(comments, std::string("Frequency: 146.940 -600 PL 100"));

        frequency = "Repeater 7";
        comments = "Meets weekly";
        CHECK(MoveBadFrequencyToComments(&frequency, &comments));
        CHECK(frequency.empty());
        CHECK_EQ(comments, std::string("Meets weekly; Frequency: Repeater 7"));

        frequency = "147.000";
        CHECK(!MoveBadFrequencyToComments(&frequency, &comments));
        CHECK_EQ(frequency, std::string("147.000"));
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
        CHECK(!loaded.use_24_hour_clock);  // 12-hour unless chosen.
        CHECK_EQ(loaded.nearby_radius_miles, 70);
        CHECK(loaded.check_for_updates);  // On unless turned off.

        settings.use_24_hour_clock = true;
        settings.nearby_radius_miles = 120;
        settings.check_for_updates = false;
        SaveSettings(dir.File("settings.txt"), settings);
        CHECK(ReadTextFile(dir.File("settings.txt")).find("time_format=24h") != std::string::npos);
        loaded = LoadSettings(dir.File("settings.txt"));
        CHECK(loaded.use_24_hour_clock);
        CHECK_EQ(loaded.nearby_radius_miles, 120);
        CHECK(!loaded.check_for_updates);
        CHECK(ReadTextFile(dir.File("settings.txt")).find("update_check=off") != std::string::npos);
    }

    QL_TEST(ANearbyRadiusOutOfRangeIsClampedWhenRead)
    {
        TempDir dir;
        WriteTextFile(dir.File("settings.txt"), "nearby_radius_miles=0\n");
        CHECK_EQ(LoadSettings(dir.File("settings.txt")).nearby_radius_miles, 1);
        WriteTextFile(dir.File("settings.txt"), "nearby_radius_miles=9999999\n");
        CHECK_EQ(LoadSettings(dir.File("settings.txt")).nearby_radius_miles, 250);
        WriteTextFile(dir.File("settings.txt"), "nearby_radius_miles=far\n");
        CHECK_EQ(LoadSettings(dir.File("settings.txt")).nearby_radius_miles, 70);
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

    QL_TEST(WriteExportFileReplacesTheFileWhole)
    {
        TempDir dir;
        std::string path = dir.File("log.txt");
        std::string error;
        CHECK(WriteExportFile(path, {"a much longer first version", "with two lines"}, &error));
        CHECK(WriteExportFile(path, {"short"}, &error));
        CHECK_EQ(ReadTextFile(path), std::string("short\n"));
        // Nothing left behind from building it.
        std::size_t files = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(std::filesystem::path(path).parent_path()))
        {
            files += entry.is_regular_file() ? 1 : 0;
        }
        CHECK_EQ(files, std::size_t{1});
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

    // ---- ZMODEM ------------------------------------------------------------

#if !defined(_WIN32)
    // ZMODEM is QuickLogger's own (zmodem_protocol.hpp): available wherever
    // there is a terminal, Alpine included, with nothing to install.
    QL_TEST(ZmodemIsBuiltInOnEveryPosixSystem)
    {
        CHECK(!NoZmodemOnThisSystem());
        CHECK(ZmodemSendAvailable());
        CHECK(ZmodemReceiveAvailable());
    }
#endif

}  // namespace ql
