// The server address the sftp and scp commands show (server_address.hpp):
// reading what the admin typed, the defaults, and the settings file keys.

#include <string>

#include "../src/server_address.hpp"
#include "../src/settings.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

namespace ql
{

    QL_TEST(AServerAddressIsAHostWithAnOptionalPort)
    {
        ServerAddress address;
        REQUIRE(ParseServerAddress("net.example.org", &address));
        CHECK_EQ(address.host, std::string("net.example.org"));
        CHECK_EQ(address.port, 0);
        REQUIRE(ParseServerAddress("  192.0.2.7:2200 ", &address));
        CHECK_EQ(address.host, std::string("192.0.2.7"));
        CHECK_EQ(address.port, 2200);
        // A port alone leaves the host to be guessed.
        REQUIRE(ParseServerAddress(":2200", &address));
        CHECK_EQ(address.host, std::string(""));
        CHECK_EQ(address.port, 2200);
        REQUIRE(ParseServerAddress("   ", &address));
        CHECK_EQ(address.host, std::string(""));
        CHECK_EQ(address.port, 0);
    }

    QL_TEST(ABadServerAddressIsRefusedAndLeavesTheOldOne)
    {
        ServerAddress address;
        address.host = "keep.example.org";
        address.port = 22;
        CHECK(!ParseServerAddress("host:", &address));
        CHECK(!ParseServerAddress("host:0", &address));
        CHECK(!ParseServerAddress("host:65536", &address));
        CHECK(!ParseServerAddress("host:22x", &address));
        CHECK(!ParseServerAddress("two words", &address));
        CHECK(!ParseServerAddress("a;b", &address));
        CHECK_EQ(address.host, std::string("keep.example.org"));
        CHECK_EQ(address.port, 22);
    }

    QL_TEST(AServerAddressIsShownAsHostAndPort)
    {
        ServerAddress address;
        address.host = "net.example.org";
        CHECK_EQ(FormatServerAddress(address), std::string("net.example.org"));
        address.port = 2200;
        CHECK_EQ(FormatServerAddress(address), std::string("net.example.org:2200"));
    }

    QL_TEST(TheServerAddressFallsBackToTheGuessAndTheLaunchedPort)
    {
        SetLaunchedSshPort(2345);
        ServerAddress set = ResolveServerAddress("net.example.org", 2200, "");
        CHECK_EQ(set.host, std::string("net.example.org"));
        CHECK_EQ(set.port, 2200);
        // A client's connected-to address that has no name is used as it is.
        ServerAddress guessed = ResolveServerAddress("", 0, "192.0.2.1");
        CHECK(!guessed.host.empty());
        CHECK_EQ(guessed.port, 2345);
        // The console has no connection: this computer's own name.
        CHECK(!ResolveServerAddress("", 0, "").host.empty());
        SetLaunchedSshPort(2222);
    }

    QL_TEST(TheServerAddressIsKeptInTheSettingsFile)
    {
        TempDir dir;
        AppSettings settings;
        settings.server_address = "net.example.org";
        settings.server_port = 2200;
        SaveSettings(dir.File("settings.txt"), settings);
        AppSettings loaded = LoadSettings(dir.File("settings.txt"));
        CHECK_EQ(loaded.server_address, std::string("net.example.org"));
        CHECK_EQ(loaded.server_port, 2200);
        // An old file without them, or a bad port, is "not set".
        AppSettings blank = LoadSettings(dir.File("does-not-exist.txt"));
        CHECK_EQ(blank.server_address, std::string(""));
        CHECK_EQ(blank.server_port, 0);
        settings.server_port = 0;
        SaveSettings(dir.File("settings.txt"), settings);
        CHECK_EQ(LoadSettings(dir.File("settings.txt")).server_port, 0);
    }

}  // namespace ql
