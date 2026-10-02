#include "ssh_server.hpp"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

// openpty()/login_tty() live in a different header (and, on some platforms,
// a different library) per OS -- verified against each platform's own
// login_tty(3)/openpty(3) man page and headers, not assumed. On Linux they're
// split across two headers: openpty() is in <pty.h> but login_tty() is in
// <utmp.h> (glibc and musl alike).
#if defined(__linux__)
#include <pty.h>
#include <utmp.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <util.h>
#endif

#include <libssh/callbacks.h>
#include <libssh/server.h>

#include "db/database.hpp"
#include "interactive_session.hpp"
#include "mosh_bridge.hpp"
#include "remote_command.hpp"
#include "scp_server.hpp"
#include "sftp_server.hpp"

namespace ql
{

    static std::string HostKeyPath()
    {
        return "ssh_host_ed25519_key";
    }

    // Generates the host key on first run and locks its permissions
    // down, or does nothing if one already exists. `ssh_bind_options_set`
    // fails loudly (logged, listener never starts) if the resulting file
    // is missing or unreadable, rather than this function trying to
    // pre-validate its contents.
    static bool EnsureHostKeyExists(const std::string& path)
    {
        struct stat existing
        {
        };
        if (::stat(path.c_str(), &existing) == 0)
        {
            return true;
        }

        ssh_key key = nullptr;
        // The `parameter` argument only matters for variable-size key
        // types (e.g. RSA bit length); ed25519 keys are a fixed size, so
        // it's unused here.
        //
        // ssh_pki_generate is deprecated in newer libssh in favor of
        // ssh_pki_generate_key(type, ssh_pki_ctx, ...), but that
        // replacement is a newer addition (introduced alongside FIDO2/
        // security-key support) that may not exist in the libssh
        // version an older distro/OS release ships -- deliberately kept
        // on the older, universally-available function for broad
        // Mac/FreeBSD/Linux compatibility, and just silencing the
        // warning here rather than trading portability for a quieter
        // build log.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        bool generated = ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) == SSH_OK;
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        if (!generated)
        {
            std::fprintf(stderr, "SSH: failed to generate a host key.\n");
            return false;
        }
        int exported = ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, path.c_str());
        ssh_key_free(key);
        if (exported != SSH_OK)
        {
            std::fprintf(stderr, "SSH: failed to write host key to %s.\n", path.c_str());
            return false;
        }
        ::chmod(path.c_str(), 0600);
        return true;
    }

    // Where a given SSH user's own AppSettings live (file_export.hpp's
    // SshUserSettingsPath names the same file, for renaming a user) -- see
    // settings.hpp's own doc comment on why this stays a flat file
    // rather than a users-table column: it must never be reachable by a
    // future shared-database export the way it isn't today. Ensures the
    // containing directory exists first, since std::ofstream (what
    // SaveSettings uses) can't create missing parent directories itself.
    static std::string PerUserSettingsPath(const std::string& username)
    {
        ::mkdir("settings", 0700);
        return "settings/" + username + ".txt";
    }

    // Parses a single OpenSSH authorized_keys-style line ("ssh-ed25519
    // AAAA... comment") into an ssh_key for comparison. Returns nullptr
    // on any parse failure or unrecognized key type -- callers treat
    // that the same as "key doesn't match."
    static ssh_key ParsePublicKeyLine(const std::string& line)
    {
        std::istringstream stream(line);
        std::string type_name;
        std::string base64_blob;
        if (!(stream >> type_name >> base64_blob))
        {
            return nullptr;
        }

        enum ssh_keytypes_e type = ssh_key_type_from_name(type_name.c_str());
        if (type == SSH_KEYTYPE_UNKNOWN)
        {
            return nullptr;
        }

        ssh_key key = nullptr;
        if (ssh_pki_import_pubkey_base64(base64_blob.c_str(), type, &key) != SSH_OK)
        {
            return nullptr;
        }
        return key;
    }

    // Everything one accepted connection's forked process needs to carry
    // between libssh's callbacks -- they're plain C function pointers
    // (no captures), so shared state has to travel via the `userdata`
    // pointer libssh threads through every one of them, pointing at one
    // of these per connection.
    struct ConnectionState
    {
        Database* db = nullptr;

        bool authenticated = false;
        std::string username;
        // From the key the user logged in with (see User::view_only).
        bool view_only = false;

        ssh_channel channel = nullptr;

        int pty_master_fd = -1;
        int pty_slave_fd = -1;
        bool shell_started = false;
        pid_t child_pid = -1;

        // The client asked for the SFTP subsystem instead of a shell (see
        // SubsystemRequestCallback).
        bool sftp_requested = false;

        // The client asked to run a command instead (see
        // ExecRequestCallback), and which.
        bool exec_requested = false;
        std::string exec_command;
    };

    static int AuthPubkeyCallback(ssh_session session, const char* user, ssh_key pubkey, char signature_state,
                                  void* userdata)
    {
        (void)session;
        ConnectionState* state = static_cast<ConnectionState*>(userdata);

        // Any of the username's keys will do.
        std::int64_t matched_key_id = 0;
        std::string matched_username;
        bool matched_view_only = false;
        for (const User& key : state->db->GetUserKeys(user))
        {
            ssh_key stored_key = ParsePublicKeyLine(key.public_key);
            if (stored_key == nullptr)
            {
                continue;
            }
            bool matches = ssh_key_cmp(pubkey, stored_key, SSH_KEY_CMP_PUBLIC) == 0;
            ssh_key_free(stored_key);
            if (matches)
            {
                matched_key_id = key.id;
                matched_username = key.username;
                matched_view_only = key.view_only;
                break;
            }
        }
        if (matched_key_id == 0)
        {
            return SSH_AUTH_DENIED;
        }

        // A client probes an offered key (signature_state == NONE)
        // before actually signing with it -- accept the probe so the
        // client knows to send a real signed request, but only mark the
        // connection authenticated once that signed request arrives.
        if (signature_state == SSH_PUBLICKEY_STATE_VALID)
        {
            state->authenticated = true;
            // As Manage Users has it, whatever case it was typed in.
            state->username = matched_username;
            state->view_only = matched_view_only;
            state->db->UpdateUserLastLogin(matched_key_id, static_cast<std::int64_t>(std::time(nullptr)));
        }
        return SSH_AUTH_SUCCESS;
    }

    static ssh_channel ChannelOpenCallback(ssh_session session, void* userdata)
    {
        ConnectionState* state = static_cast<ConnectionState*>(userdata);
        // The SSH protocol itself never delivers a channel-open request
        // before authentication succeeds, but check anyway rather than
        // trust that solely -- costs nothing.
        if (!state->authenticated)
        {
            return nullptr;
        }
        state->channel = ssh_channel_new(session);
        return state->channel;
    }

    static int PtyRequestCallback(ssh_session session, ssh_channel channel, const char* term, int width, int height,
                                  int pxwidth, int pwheight, void* userdata)
    {
        (void)session;
        (void)channel;
        (void)term;
        ConnectionState* state = static_cast<ConnectionState*>(userdata);

        struct winsize window_size
        {
        };
        window_size.ws_col = static_cast<unsigned short>(width);
        window_size.ws_row = static_cast<unsigned short>(height);
        window_size.ws_xpixel = static_cast<unsigned short>(pxwidth);
        window_size.ws_ypixel = static_cast<unsigned short>(pwheight);

        if (::openpty(&state->pty_master_fd, &state->pty_slave_fd, nullptr, nullptr, &window_size) != 0)
        {
            return -1;
        }
        return 0;
    }

    static int PtyWindowChangeCallback(ssh_session session, ssh_channel channel, int width, int height, int pxwidth,
                                       int pwheight, void* userdata)
    {
        (void)session;
        (void)channel;
        ConnectionState* state = static_cast<ConnectionState*>(userdata);
        if (state->pty_master_fd < 0)
        {
            return -1;
        }
        struct winsize window_size
        {
        };
        window_size.ws_col = static_cast<unsigned short>(width);
        window_size.ws_row = static_cast<unsigned short>(height);
        window_size.ws_xpixel = static_cast<unsigned short>(pxwidth);
        window_size.ws_ypixel = static_cast<unsigned short>(pwheight);
        ::ioctl(state->pty_master_fd, TIOCSWINSZ, &window_size);
        return 0;
    }

    // Forks the grandchild that becomes a full QuickLogger session,
    // exactly like a login shell would: closes everything it doesn't
    // need (the listening socket, the network side of the SSH
    // connection, the pty master -- it only needs the pty slave),
    // `login_tty()`s onto the pty slave (handles setsid()/TIOCSCTTY/
    // dup2 of fds 0/1/2 in one call), then calls straight into
    // RunInteractiveSession -- the same function main() calls for a
    // local console launch, just is_console_session=false and a
    // per-username settings path (see PerUserSettingsPath).
    static int ShellRequestCallback(ssh_session session, ssh_channel channel, void* userdata)
    {
        (void)channel;
        ConnectionState* state = static_cast<ConnectionState*>(userdata);
        // One or the other per connection: a channel already given to SFTP
        // gets no shell.
        if (state->pty_slave_fd < 0 || state->sftp_requested || state->exec_requested)
        {
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            return 1;
        }
        if (pid == 0)
        {
            ::close(ssh_get_fd(session));
            ::close(state->pty_master_fd);

            ::login_tty(state->pty_slave_fd);
            // The listener ignores SIGCHLD (see SshAcceptLoop), and that's
            // inherited. The session waits for the programs it runs (sz and
            // rz for ZMODEM) and needs their exit status; with SIGCHLD
            // ignored they're reaped before it can, so it waited out its
            // whole timeout and reported "timed out" after every transfer.
            std::signal(SIGCHLD, SIG_DFL);
            std::string username = state->username;
            RunInteractiveSession(PerUserSettingsPath(username), /*is_console_session=*/false, username);
            _exit(0);
        }

        ::close(state->pty_slave_fd);
        state->pty_slave_fd = -1;
        state->child_pid = pid;
        state->shell_started = true;
        return 0;
    }

    // Accepts the "sftp" subsystem (OpenSSH's sftp, and its scp from 9.0
    // on) on a channel that hasn't started a shell; HandleConnection then
    // serves it with RunSftpSession. Any other subsystem is refused.
    static int SubsystemRequestCallback(ssh_session session, ssh_channel channel, const char* subsystem, void* userdata)
    {
        (void)session;
        (void)channel;
        ConnectionState* state = static_cast<ConnectionState*>(userdata);
        if (state->shell_started || state->sftp_requested || state->exec_requested ||
            std::string_view(subsystem) != "sftp")
        {
            return -1;
        }
        state->sftp_requested = true;
        return 0;
    }

    // Accepts a command (`ssh user@host <command>`) on a channel that
    // hasn't started anything else. It's never given to a shell or run as
    // a program: HandleConnection hands it to RunScpCommand (an scp client
    // that doesn't speak SFTP) or RunRemoteCommand, which knows only
    // QuickLogger's own commands and refuses everything else.
    static int ExecRequestCallback(ssh_session session, ssh_channel channel, const char* command, void* userdata)
    {
        (void)session;
        (void)channel;
        ConnectionState* state = static_cast<ConnectionState*>(userdata);
        if (state->shell_started || state->sftp_requested || state->exec_requested)
        {
            return -1;
        }
        state->exec_requested = true;
        state->exec_command = command;
        return 0;
    }

    // Runs the command an exec request asked for (see ExecRequestCallback),
    // writes its result to the channel and returns its exit status. Opens
    // the database itself: this process closed its own before the channel
    // was set up, and forks nothing after this (THE FORK RULE).
    // An SSH channel as RunScpCommand reads and writes it, with libssh's
    // blocking calls.
    class SshScpChannel : public ScpChannel
    {
    public:
        explicit SshScpChannel(ssh_channel channel) : channel_(channel) {}

        bool Read(char* data, std::size_t size) override
        {
            std::size_t done = 0;
            while (done < size)
            {
                int count = ssh_channel_read(channel_, data + done, static_cast<uint32_t>(size - done), 0);
                if (count <= 0)
                {
                    return false;
                }
                done += static_cast<std::size_t>(count);
            }
            return true;
        }

        bool Write(const char* data, std::size_t size) override
        {
            return ssh_channel_write(channel_, data, static_cast<uint32_t>(size)) == static_cast<int>(size);
        }

    private:
        ssh_channel channel_;
    };

    // mosh-server's path, from PATH or where packages put it; "" if it
    // isn't installed.
    static std::string FindMoshServer()
    {
        std::vector<std::string> dirs;
        const char* path = std::getenv("PATH");
        std::string remaining = path != nullptr ? path : "";
        std::string::size_type start = 0;
        while (start <= remaining.size())
        {
            std::string::size_type colon = remaining.find(':', start);
            std::string dir = remaining.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
            if (!dir.empty())
            {
                dirs.push_back(dir);
            }
            if (colon == std::string::npos)
            {
                break;
            }
            start = colon + 1;
        }
        for (const char* dir : {"/usr/bin", "/usr/local/bin", "/opt/homebrew/bin", "/opt/local/bin"})
        {
            dirs.emplace_back(dir);
        }
        for (const std::string& dir : dirs)
        {
            std::string candidate = dir + "/mosh-server";
            if (::access(candidate.c_str(), X_OK) == 0)
            {
                return candidate;
            }
        }
        return "";
    }

    // This program's own path, for mosh-server to start; "" if it can't
    // be found.
    static std::string SelfExecutablePath()
    {
        char buffer[4096];
#if defined(__linux__)
        ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (length <= 0)
        {
            return "";
        }
        buffer[length] = '\0';
        return buffer;
#elif defined(__FreeBSD__)
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
        std::size_t length = sizeof(buffer);
        if (::sysctl(mib, 4, buffer, &length, nullptr, 0) != 0)
        {
            return "";
        }
        return buffer;
#elif defined(__APPLE__)
        uint32_t length = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &length) != 0)
        {
            return "";
        }
        char resolved[PATH_MAX];
        return ::realpath(buffer, resolved) != nullptr ? std::string(resolved) : std::string(buffer);
#else
        return "";
#endif
    }

    // "client-address client-port server-address server-port", as sshd
    // sets SSH_CONNECTION (mosh-server -s binds to the server address).
    static std::string SshConnectionString(ssh_session session)
    {
        int fd = ssh_get_fd(session);
        struct sockaddr_storage peer
        {
        };
        struct sockaddr_storage local
        {
        };
        socklen_t peer_size = sizeof(peer);
        socklen_t local_size = sizeof(local);
        if (::getpeername(fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_size) != 0 ||
            ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &local_size) != 0)
        {
            return "";
        }
        char peer_host[NI_MAXHOST];
        char peer_port[NI_MAXSERV];
        char local_host[NI_MAXHOST];
        char local_port[NI_MAXSERV];
        if (::getnameinfo(reinterpret_cast<struct sockaddr*>(&peer), peer_size, peer_host, sizeof(peer_host), peer_port,
                          sizeof(peer_port), NI_NUMERICHOST | NI_NUMERICSERV) != 0 ||
            ::getnameinfo(reinterpret_cast<struct sockaddr*>(&local), local_size, local_host, sizeof(local_host),
                          local_port, sizeof(local_port), NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        {
            return "";
        }
        std::string connection = std::string(peer_host) + " " + peer_port + " " + local_host + " " + local_port;
        // An IPv4 client on a dual-stack socket: as sshd gives it.
        std::string mapped = "::ffff:";
        std::string::size_type at = 0;
        while ((at = connection.find(mapped, at)) != std::string::npos)
        {
            connection.erase(at, mapped.size());
        }
        return connection;
    }

    static void WriteChannelText(ssh_channel channel, const std::string& text)
    {
        ssh_channel_write(channel, text.data(), static_cast<uint32_t>(text.size()));
    }

    // Runs mosh-server for a mosh client (see mosh_bridge.hpp): its own
    // options, but QuickLogger --mosh-session as the program, logged in as
    // this connection's user by a one-time token. Relays what mosh-server
    // prints (the "MOSH CONNECT" line the client is waiting for) and
    // returns its exit status. mosh-server then carries on by itself, over
    // UDP, after this connection has gone.
    static int RunMoshServerCommand(ssh_session session, ssh_channel channel, const ConnectionState& state)
    {
        MoshServerRequest request;
        std::string error;
        if (!ParseMoshServerCommand(state.exec_command, &request, &error))
        {
            WriteChannelText(channel, "QuickLogger: " + error + "\n");
            return 1;
        }
        std::string mosh_server = FindMoshServer();
        if (mosh_server.empty())
        {
            WriteChannelText(channel,
                             "QuickLogger: mosh-server isn't installed on this server, so Mosh can't be "
                             "used here. Connect with ssh instead.\n");
            return 127;
        }
        std::string self = SelfExecutablePath();
        char cwd[4096];
        if (self.empty() || ::getcwd(cwd, sizeof(cwd)) == nullptr)
        {
            WriteChannelText(channel, "QuickLogger: couldn't find its own program to start under Mosh.\n");
            return 1;
        }
        std::string token = CreateMoshToken(cwd, state.username, static_cast<std::int64_t>(std::time(nullptr)), &error);
        if (token.empty())
        {
            WriteChannelText(channel, "QuickLogger: " + error + "\n");
            return 1;
        }
        std::string connection = SshConnectionString(session);
        if (request.report_ssh_connection && !connection.empty())
        {
            WriteChannelText(channel, "\nMOSH SSH_CONNECTION " + connection + "\n");
        }

        // A clean environment: mosh-server sets TERM and the locale itself.
        // HOME is the data folder, where mosh-server starts its program.
        // A session nobody comes back to ends after a day, rather than
        // waiting forever for its client.
        std::vector<std::string> environment{
            "PATH=/usr/bin:/bin:/usr/local/bin", "HOME=" + std::string(cwd), "QUICKLOGGER_MOSH_DIR=" + std::string(cwd),
            "QUICKLOGGER_MOSH_TOKEN=" + token, "MOSH_SERVER_NETWORK_TMOUT=" + std::to_string(kMoshIdleSeconds)};
        if (!connection.empty())
        {
            environment.push_back("SSH_CONNECTION=" + connection);
        }
        std::vector<std::string> arguments = MoshServerArgv(mosh_server, request, self);
        std::vector<char*> argv;
        for (std::string& argument : arguments)
        {
            argv.push_back(&argument[0]);
        }
        argv.push_back(nullptr);
        std::vector<char*> envp;
        for (std::string& variable : environment)
        {
            envp.push_back(&variable[0]);
        }
        envp.push_back(nullptr);

        int output[2];
        if (::pipe(output) != 0)
        {
            WriteChannelText(channel, "QuickLogger: couldn't start mosh-server.\n");
            return 1;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, output[1], 1);
        posix_spawn_file_actions_adddup2(&actions, output[1], 2);
        posix_spawn_file_actions_addclose(&actions, output[0]);
        posix_spawn_file_actions_addclose(&actions, output[1]);
        // Not the SSH connection's socket: mosh-server lives on after it,
        // for up to a day, and would hold it open all that time.
        posix_spawn_file_actions_addclose(&actions, ssh_get_fd(session));
        // In a session of its own, so it outlives whatever ends the
        // listener's: the terminal QuickLogger was started from closing,
        // say. That's what Mosh is for.
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#else
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);
#endif
        // Its exit status is wanted (SIGCHLD is ignored, see SshAcceptLoop).
        std::signal(SIGCHLD, SIG_DFL);
        pid_t pid = -1;
        int spawned = posix_spawn(&pid, mosh_server.c_str(), &actions, &attributes, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        ::close(output[1]);
        if (spawned != 0)
        {
            ::close(output[0]);
            WriteChannelText(channel, "QuickLogger: couldn't start mosh-server.\n");
            return 1;
        }

        // Until mosh-server has detached and its output ends: a few
        // seconds at most.
        std::time_t deadline = std::time(nullptr) + 20;
        int status = 0;
        bool exited = false;
        while (std::time(nullptr) < deadline)
        {
            struct pollfd readable
            {
            };
            readable.fd = output[0];
            readable.events = POLLIN;
            int ready = ::poll(&readable, 1, exited ? 500 : 200);
            if (ready > 0)
            {
                char buffer[1024];
                ssize_t count = ::read(output[0], buffer, sizeof(buffer));
                if (count <= 0)
                {
                    break;
                }
                ssh_channel_write(channel, buffer, static_cast<uint32_t>(count));
                continue;
            }
            if (exited)
            {
                break;  // Quiet since it exited: its detached half keeps the pipe.
            }
            exited = ::waitpid(pid, &status, WNOHANG) == pid;
        }
        ::close(output[0]);
        if (!exited && ::waitpid(pid, &status, WNOHANG) != pid)
        {
            return 1;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    static int RunExecCommand(ssh_session session, ssh_channel channel, const std::string& db_path,
                              const ConnectionState& state)
    {
        if (IsMoshServerCommandLine(state.exec_command))
        {
            return RunMoshServerCommand(session, channel, state);
        }
        // An scp client that doesn't speak SFTP. Never the database: the
        // files are all it needs.
        if (IsScpCommandLine(state.exec_command))
        {
            ScpCommand scp;
            std::string scp_error;
            if (!ParseScpCommand(state.exec_command, &scp, &scp_error))
            {
                std::string message = "\x02scp: " + scp_error + "\n";
                ssh_channel_write(channel, message.data(), static_cast<uint32_t>(message.size()));
                return 1;
            }
            SshScpChannel scp_channel(channel);
            return RunScpCommand(scp, &scp_channel, db_path, state.username, state.view_only);
        }

        RemoteCommandResult result;
        RemoteCommand command;
        std::string error;
        // A view-only user is refused whatever the command; RunRemoteCommand
        // says so before looking at it.
        if (!state.view_only && !ParseRemoteCommand(state.exec_command, &command, &error))
        {
            result = RemoteCommandParseError(error);
        }
        else
        {
            try
            {
                Database db(db_path);
                result = RunRemoteCommand(command, &db, db_path, state.username, state.view_only,
                                          static_cast<std::int64_t>(std::time(nullptr)));
            }
            catch (const std::exception& e)
            {
                result = RemoteCommandParseError(std::string("The database couldn't be opened: ") + e.what());
            }
        }
        ssh_channel_write(channel, result.output.data(), static_cast<uint32_t>(result.output.size()));
        return result.exit_status;
    }

    // Ends a channel that ran SFTP or a command with `exit_status`, and the
    // connection with it. scp counts a copy as failed unless ssh exits 0,
    // and a command's caller reads ssh's exit status as the command's:
    // that takes an exit status and the client closing the connection
    // itself, since a disconnect from this end is "closed by remote host"
    // (exit 255). So close the channel and give the client a few seconds to
    // go.
    static void EndChannel(ssh_session session, ssh_channel channel, int exit_status)
    {
        ssh_channel_request_send_exit_status(channel, exit_status);
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_event closing = ssh_event_new();
        ssh_event_add_session(closing, session);
        std::time_t closing_deadline = std::time(nullptr) + 5;
        while (ssh_is_connected(session) && std::time(nullptr) < closing_deadline)
        {
            ssh_event_dopoll(closing, 200);
        }
        ssh_event_free(closing);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
    }

    static int ChannelDataCallback(ssh_session session, ssh_channel channel, void* data, uint32_t len, int is_stderr,
                                   void* userdata)
    {
        (void)session;
        (void)channel;
        (void)is_stderr;
        ConnectionState* state = static_cast<ConnectionState*>(userdata);
        // SFTP and scp read the channel themselves (RunSftpSession,
        // RunScpCommand): leave their data in the channel's buffer.
        if (state->sftp_requested || state->exec_requested)
        {
            return 0;
        }
        if (state->pty_master_fd < 0)
        {
            return static_cast<int>(len);
        }
        ssize_t written = ::write(state->pty_master_fd, data, len);
        return written > 0 ? static_cast<int>(written) : 0;
    }

    // Registered on the pty master fd via ssh_event_add_fd -- called
    // whenever the child's session has produced output, relaying it
    // into the SSH channel (the reverse direction of ChannelDataCallback).
    static int PtyMasterReadableCallback(socket_t fd, int revents, void* userdata)
    {
        ConnectionState* state = static_cast<ConnectionState*>(userdata);
        if ((static_cast<unsigned int>(revents) & static_cast<unsigned int>(POLLIN)) == 0)
        {
            return 0;
        }
        // A whole frame at once (the session writes each in one go), so it
        // goes out as one SSH packet, not several.
        char buffer[32768];
        ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count <= 0)
        {
            return 0;
        }
        ssh_channel_write(state->channel, buffer, static_cast<uint32_t>(count));
        return static_cast<int>(count);
    }

    // True once `pid` has exited (or was already reaped: SIGCHLD is ignored
    // process-wide, see SshAcceptLoop, so the kernel may have done it).
    static bool ChildHasExited(pid_t pid)
    {
        pid_t reaped = waitpid(pid, nullptr, WNOHANG);
        return reaped == pid || (reaped == -1 && errno == ECHILD);
    }

    // Ends a session's child process. Asks first (SIGTERM), then insists
    // (SIGKILL) if it hasn't gone within a couple of seconds -- a child that
    // is stuck, e.g. blocked writing a screen update nobody will ever read,
    // must never keep its connection's process waiting forever.
    static void StopSessionChild(pid_t pid)
    {
        if (ChildHasExited(pid))
        {
            return;
        }
        kill(pid, SIGTERM);
        for (int i = 0; i < 40; ++i)
        {
            if (ChildHasExited(pid))
            {
                return;
            }
            ::usleep(50000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }

    // Runs the whole lifecycle of one already-accepted connection: key
    // exchange, auth, channel/pty/shell setup, then relays bytes between
    // the pty and the SSH channel until either side goes away. Called in
    // a process forked solely for this one connection (see
    // SshAcceptLoop) -- returning from this function means that process
    // is done and should exit.
    static void HandleConnection(ssh_session session, const std::string& db_path)
    {
        // A connection gets this long to finish key exchange, log in and
        // start its shell, or this process is ended (SIGALRM's default).
        // Key exchange has no timeout of its own, so without this a client
        // that connects and then stalls -- the internet's port scanners do
        // it all day on port 22 -- would hold this process open forever.
        // Cancelled once the shell is running.
        ::alarm(90);

        // A client that vanishes without closing the connection (its
        // network dropped, a laptop slept) is noticed within a few minutes,
        // and this process ends, rather than waiting on it for ever.
        int keepalive = 1;
        int fd = ssh_get_fd(session);
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
        int idle = 60;
        int interval = 15;
        int count = 4;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif

        ConnectionState state;

        // Installed *before* key exchange, as libssh's own server
        // examples do -- not after. A client's SERVICE_REQUEST often
        // arrives in the same TCP read as its NEWKEYS, i.e. while
        // ssh_handle_key_exchange is still running; with no server
        // callbacks installed yet, libssh doesn't answer it (it appears
        // to be queued for the message API, which this server doesn't
        // use) and the client then waits forever for its SERVICE_ACCEPT
        // -- observed live via libssh's packet log. That was
        // the second, timing-dependent cause of the old "intermittent
        // SSH connection failure" (the other being THE FORK RULE, in
        // ssh_server.hpp).
        struct ssh_server_callbacks_struct server_callbacks
        {
        };
        server_callbacks.userdata = &state;
        server_callbacks.auth_pubkey_function = AuthPubkeyCallback;
        server_callbacks.channel_open_request_session_function = ChannelOpenCallback;
        ssh_callbacks_init(&server_callbacks);
        ssh_set_server_callbacks(session, &server_callbacks);

        ssh_event event = nullptr;
        std::time_t deadline = 0;
        bool key_exchange_succeeded = false;

        // Scoped so this connection's own database connection is fully
        // closed (sqlite3_close, releasing every fd/lock it holds)
        // before ShellRequestCallback forks below. SQLite's fcntl-based
        // advisory locking is scoped per *process*, not per file
        // descriptor -- a child that inherits this still-open
        // connection's fd via fork(), then opens its *own* independent
        // connection to the same file (RunInteractiveSession does
        // exactly that), corrupts SQLite's view of its own lock state
        // ("Failed to create schema: locking protocol", confirmed by
        // hitting this for real against a live SSH client before this
        // scoping was added; see THE FORK RULE in ssh_server.hpp).
        // Opened before key exchange rather than after so `state.db` is
        // never null whenever an auth callback can fire. Nothing past
        // this scope needs `db` -- ChannelOpenCallback only checks
        // state.authenticated.
        {
            Database db(db_path);
            state.db = &db;

            key_exchange_succeeded = ssh_handle_key_exchange(session) == SSH_OK;
            if (key_exchange_succeeded)
            {
                event = ssh_event_new();
                ssh_event_add_session(event, session);

                deadline = std::time(nullptr) + 60;
                while (!state.authenticated && ssh_is_connected(session) && std::time(nullptr) < deadline)
                {
                    ssh_event_dopoll(event, 200);
                }
            }
            state.db = nullptr;
        }

        if (!key_exchange_succeeded)
        {
            ssh_free(session);
            return;
        }

        if (!state.authenticated)
        {
            // The client gave up, was denied (unknown user / wrong
            // key), or never authenticated within the deadline.
            std::fprintf(stderr, "SSH: a connection ended without authenticating.\n");
            ssh_event_free(event);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }

        // Poll until a channel exists (the client opens one once it
        // sees auth succeeded) and, once channel callbacks are wired up
        // below, until a pty+shell, SFTP or a command has been asked for.
        struct ssh_channel_callbacks_struct channel_callbacks
        {
        };
        channel_callbacks.userdata = &state;
        channel_callbacks.channel_pty_request_function = PtyRequestCallback;
        channel_callbacks.channel_pty_window_change_function = PtyWindowChangeCallback;
        channel_callbacks.channel_shell_request_function = ShellRequestCallback;
        channel_callbacks.channel_subsystem_request_function = SubsystemRequestCallback;
        channel_callbacks.channel_exec_request_function = ExecRequestCallback;
        channel_callbacks.channel_data_function = ChannelDataCallback;
        ssh_callbacks_init(&channel_callbacks);
        bool channel_callbacks_registered = false;

        while (!state.shell_started && !state.sftp_requested && !state.exec_requested && ssh_is_connected(session) &&
               std::time(nullptr) < deadline)
        {
            ssh_event_dopoll(event, 200);
            if (state.channel != nullptr && !channel_callbacks_registered)
            {
                ssh_set_channel_callbacks(state.channel, &channel_callbacks);
                channel_callbacks_registered = true;
            }
        }

        if (state.sftp_requested || state.exec_requested)
        {
            ::alarm(0);
            // A pty asked for before the subsystem or command has no use.
            if (state.pty_master_fd >= 0)
            {
                ::close(state.pty_master_fd);
                ::close(state.pty_slave_fd);
            }
            // RunSftpSession reads the channel with libssh's blocking
            // calls, which poll the session themselves (as does writing a
            // command's result): take it back out of the event loop and the
            // callbacks first.
            ssh_remove_channel_callbacks(state.channel, &channel_callbacks);
            ssh_event_remove_session(event, session);
            ssh_event_free(event);
            int exit_status = 0;
            if (state.sftp_requested)
            {
                RunSftpSession(session, state.channel, db_path, state.username, state.view_only);
            }
            else
            {
                exit_status = RunExecCommand(session, state.channel, db_path, state);
            }
            EndChannel(session, state.channel, exit_status);
            return;
        }

        if (!state.shell_started)
        {
            ssh_event_free(event);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
        ::alarm(0);

        ssh_event_add_fd(event, state.pty_master_fd, POLLIN, PtyMasterReadableCallback, &state);

        while (true)
        {
            ssh_event_dopoll(event, 200);

            if (ChildHasExited(state.child_pid))
            {
                break;
            }
            if (ssh_channel_is_eof(state.channel))
            {
                break;
            }
            // Catches an abrupt client-side disconnect (network drop,
            // client killed rather than exiting cleanly) that never
            // sends a channel EOF -- without this, the session's child
            // process (running RunInteractiveSession, oblivious that
            // its pty's far end vanished) would run forever as an
            // orphan. Confirmed live: killing the ssh client process
            // outright left all three of this connection's processes
            // running until this check was added.
            if (!ssh_is_connected(session))
            {
                break;
            }
        }

        // Close the pty before stopping the child, not after. Once the
        // client is gone nothing reads the pty any more, so a child in the
        // middle of drawing a screen can be blocked writing to it; closing
        // our end fails that write and hangs up its terminal. Stopping it
        // first instead left both processes waiting on each other forever
        // (seen under load: 43 of ~500 dropped connections leaked this way).
        ssh_event_remove_fd(event, state.pty_master_fd);
        ::close(state.pty_master_fd);
        StopSessionChild(state.child_pid);
        ssh_event_free(event);

        ssh_channel_send_eof(state.channel);
        ssh_channel_close(state.channel);
        ssh_channel_free(state.channel);
        ssh_disconnect(session);
        ssh_free(session);
    }

    // The accept loop: binds once, then loops forever accepting
    // connections and forking a dedicated process for each one (see
    // HandleConnection). Runs either as the whole main thread of a
    // headless process (RunSshServer) or as the whole of a dedicated
    // listener process (StartSshServerProcess) -- never inside a process
    // that has opened the database itself, since every connection it
    // forks would inherit that process's SQLite state (see THE FORK RULE
    // in ssh_server.hpp). A named class rather than a lambda, per this
    // codebase's convention.
    class SshAcceptLoop
    {
    public:
        // A `parent_pid` > 0 makes the loop return once that process is
        // no longer this process's parent (the console session that
        // started this listener quit or died -- a reparented process
        // gets a different parent pid); <= 0 means run forever.
        SshAcceptLoop(std::string db_path, int port, pid_t parent_pid)
            : db_path_(std::move(db_path)), port_(port), parent_pid_(parent_pid)
        {
        }

        void operator()() const
        {
            // Ignoring SIGCHLD (rather than leaving the default
            // disposition) auto-reaps every descendant at the kernel
            // level, so nothing here needs its own zombie-reaping logic.
            // Set before any forking happens so it's inherited by every
            // descendant this process creates, not just the ones forked
            // after this line.
            std::signal(SIGCHLD, SIG_IGN);

            if (!EnsureHostKeyExists(HostKeyPath()))
            {
                return;
            }

            ssh_bind sshbind = ssh_bind_new();
            std::string host_key_path = HostKeyPath();
            ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_HOSTKEY, host_key_path.c_str());
            ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT, &port_);

            if (ssh_bind_listen(sshbind) < 0)
            {
                std::fprintf(stderr, "SSH: failed to listen on port %d: %s\n", port_, ssh_get_error(sshbind));
                ssh_bind_free(sshbind);
                return;
            }
            int listen_fd = ssh_bind_get_fd(sshbind);

            while (parent_pid_ <= 0 || ::getppid() == parent_pid_)
            {
                // Wakes up once a second even with no connection waiting,
                // purely so the parent-still-alive check above gets to
                // run.
                struct pollfd listen_poll
                {
                };
                listen_poll.fd = listen_fd;
                listen_poll.events = POLLIN;
                if (::poll(&listen_poll, 1, 1000) <= 0)
                {
                    continue;
                }

                ssh_session session = ssh_new();
                if (ssh_bind_accept(sshbind, session) != SSH_OK)
                {
                    ssh_free(session);
                    continue;
                }

                pid_t pid = fork();
                if (pid < 0)
                {
                    ssh_free(session);
                    continue;
                }
                if (pid == 0)
                {
                    // The listening socket keeps accepting in the
                    // parent; this child only needs this one
                    // connection's own session. Closed here rather than
                    // held for the connection's lifetime, so a restarted
                    // listener (a new version being deployed) can bind
                    // the port again while connections are still open.
                    ::close(listen_fd);
                    HandleConnection(session, db_path_);
                    _exit(0);
                }

                // The parent doesn't touch this connection again --
                // HandleConnection took full ownership of `session` in
                // the child's own address space. Matches libssh's own
                // ssh_server_fork.c example exactly (ssh_disconnect then
                // ssh_free).
                ssh_disconnect(session);
                ssh_free(session);
            }

            ssh_bind_free(sshbind);
        }

    private:
        std::string db_path_;
        int port_;
        pid_t parent_pid_;
    };

    int RunMoshSession()
    {
        // Taken out of the environment, so the session's own programs
        // (sz, rz) don't see them.
        const char* dir = std::getenv("QUICKLOGGER_MOSH_DIR");
        const char* token = std::getenv("QUICKLOGGER_MOSH_TOKEN");
        std::string dir_value = dir != nullptr ? dir : "";
        std::string token_value = token != nullptr ? token : "";
        ::unsetenv("QUICKLOGGER_MOSH_DIR");
        ::unsetenv("QUICKLOGGER_MOSH_TOKEN");
        std::string username;
        if (dir_value.empty() || ::chdir(dir_value.c_str()) != 0 ||
            !ConsumeMoshToken(dir_value, token_value, static_cast<std::int64_t>(std::time(nullptr)), &username))
        {
            std::printf(
                "QuickLogger: this Mosh session wasn't started by QuickLogger's SSH server, so it can't "
                "log in. Connect with mosh to the server's SSH port.\n");
            return 1;
        }
        RunInteractiveSession(PerUserSettingsPath(username), /*is_console_session=*/false, username,
                              /*over_mosh=*/true);
        return 0;
    }

    void RunSshServer(const std::string& db_path, int port)
    {
        SshAcceptLoop accept_loop(db_path, port, /*parent_pid=*/-1);
        accept_loop();
    }

    pid_t StartSshServerProcess(const std::string& db_path, int port)
    {
        pid_t parent_pid = ::getpid();
        pid_t pid = fork();
        if (pid < 0)
        {
            std::fprintf(stderr, "SSH: failed to start the listener process.\n");
            return -1;
        }
        if (pid == 0)
        {
            SshAcceptLoop accept_loop(db_path, port, parent_pid);
            accept_loop();
            _exit(0);
        }
        return pid;
    }

    void StopSshServerProcess(pid_t listener_pid)
    {
        if (listener_pid <= 0)
        {
            return;
        }
        ::kill(listener_pid, SIGTERM);
        ::waitpid(listener_pid, nullptr, 0);
    }

}  // namespace ql
