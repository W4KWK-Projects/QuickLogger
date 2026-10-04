#include "server_address.hpp"

#include <cctype>
#include <cstdlib>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ql
{

    static int g_launched_ssh_port = 2222;

    void SetLaunchedSshPort(int port)
    {
        g_launched_ssh_port = port;
    }

    int LaunchedSshPort()
    {
        return g_launched_ssh_port;
    }

    static bool IsHostChar(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '_';
    }

    bool ParseServerAddress(const std::string& text, ServerAddress* out)
    {
        std::string::size_type start = text.find_first_not_of(" \t");
        if (start == std::string::npos)
        {
            *out = ServerAddress();
            return true;
        }
        std::string::size_type end = text.find_last_not_of(" \t");
        std::string trimmed = text.substr(start, end - start + 1);

        ServerAddress parsed;
        std::string::size_type colon = trimmed.find(':');
        std::string host = trimmed.substr(0, colon);
        if (colon != std::string::npos)
        {
            std::string port_text = trimmed.substr(colon + 1);
            if (port_text.empty() || port_text.size() > 5 ||
                port_text.find_first_not_of("0123456789") != std::string::npos)
            {
                return false;
            }
            parsed.port = std::stoi(port_text);
            if (parsed.port < 1 || parsed.port > 65535)
            {
                return false;
            }
        }
        // A port alone (":2200") keeps the host blank, to be guessed.
        for (char c : host)
        {
            if (!IsHostChar(c))
            {
                return false;
            }
        }
        if (host.size() > 253)
        {
            return false;
        }
        parsed.host = host;
        *out = parsed;
        return true;
    }

    std::string FormatServerAddress(const ServerAddress& address)
    {
        if (address.port == 0)
        {
            return address.host;
        }
        return address.host + ":" + std::to_string(address.port);
    }

    std::string GuessServerHost(const std::string& connected_to)
    {
#if defined(_WIN32)
        // No SSH server here to connect to: the computer's name.
        const char* computer = std::getenv("COMPUTERNAME");
        return connected_to.empty() ? (computer != nullptr ? computer : "") : connected_to;
#else
        if (!connected_to.empty())
        {
            struct sockaddr_in v4{};
            if (::inet_pton(AF_INET, connected_to.c_str(), &v4.sin_addr) == 1)
            {
                v4.sin_family = AF_INET;
                char name[NI_MAXHOST];
                if (::getnameinfo(reinterpret_cast<struct sockaddr*>(&v4), sizeof(v4), name, sizeof(name), nullptr, 0,
                                  NI_NAMEREQD) == 0 &&
                    name[0] != '\0')
                {
                    return name;
                }
            }
            return connected_to;
        }
        char name[256];
        if (::gethostname(name, sizeof(name)) == 0 && name[0] != '\0')
        {
            name[sizeof(name) - 1] = '\0';
            return name;
        }
        return "";
#endif
    }

    ServerAddress ResolveServerAddress(const std::string& set_host, int set_port, const std::string& connected_to)
    {
        ServerAddress address;
        address.host = set_host.empty() ? GuessServerHost(connected_to) : set_host;
        address.port = set_port > 0 ? set_port : LaunchedSshPort();
        return address;
    }

}  // namespace ql
