#pragma once

#include <string>

namespace ql
{

    // The address people type to reach this server, as shown in the
    // sftp and scp commands: a host name or IP and a port. The local
    // console's Settings holds it (AppSettings::server_address and
    // server_port); blank host and port 0 mean "work it out".
    struct ServerAddress
    {
        std::string host;
        int port = 0;
    };

    // The SSH port the server was started with (--ssh-port=): what the port
    // is when none is set. Set once at startup, in the console process and
    // in the SSH listener's.
    void SetLaunchedSshPort(int port);
    int LaunchedSshPort();

    // Reads "host" or "host:port" (a host name or IPv4 address; spaces at
    // the ends are ignored). Blank is fine: host empty, port 0. On failure,
    // returns false and leaves `out` alone.
    bool ParseServerAddress(const std::string& text, ServerAddress* out);

    // The inverse: "host" or "host:port", whichever is set.
    std::string FormatServerAddress(const ServerAddress& address);

    // A host to offer when none is set. `connected_to` is the address a
    // client connected to (an SSH session knows it; the console passes
    // ""): its reverse-DNS name if it has one, else the address itself.
    // Without one, this computer's own host name.
    std::string GuessServerHost(const std::string& connected_to);

    // What to show: the set host, else the guess, and the set port, else the
    // launched SSH port.
    ServerAddress ResolveServerAddress(const std::string& set_host, int set_port, const std::string& connected_to);

}  // namespace ql
