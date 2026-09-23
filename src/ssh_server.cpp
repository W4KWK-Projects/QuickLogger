#include "ssh_server.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <sstream>
#include <thread>

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// openpty()/login_tty() live in a different header (and, on some platforms,
// a different library) per OS -- verified against each platform's own
// login_tty(3)/openpty(3) man page, not assumed.
#if defined(__linux__)
#include <pty.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <util.h>
#endif

#include <libssh/callbacks.h>
#include <libssh/server.h>

#include "db/database.hpp"
#include "interactive_session.hpp"

namespace ql
{

    namespace
    {

        std::string HostKeyPath()
        {
            return "ssh_host_ed25519_key";
        }

        // Generates the host key on first run and locks its permissions
        // down, or does nothing if one already exists. `ssh_bind_options_set`
        // fails loudly (logged, listener never starts) if the resulting file
        // is missing or unreadable, rather than this function trying to
        // pre-validate its contents.
        bool EnsureHostKeyExists(const std::string& path)
        {
            struct stat existing;
            if (::stat(path.c_str(), &existing) == 0)
            {
                return true;
            }

            ssh_key key = nullptr;
            // The `parameter` argument only matters for variable-size key
            // types (e.g. RSA bit length); ed25519 keys are a fixed size, so
            // it's unused here.
            if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK)
            {
                std::fprintf(stderr, "SSH: failed to generate a host key.\n");
                return false;
            }
            int exported =
                ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, path.c_str());
            ssh_key_free(key);
            if (exported != SSH_OK)
            {
                std::fprintf(stderr, "SSH: failed to write host key to %s.\n", path.c_str());
                return false;
            }
            ::chmod(path.c_str(), 0600);
            return true;
        }

        // Where a given SSH user's own AppSettings live -- see
        // settings.hpp's own doc comment on why this stays a flat file
        // rather than a users-table column: it must never be reachable by a
        // future shared-database export the way it isn't today. Ensures the
        // containing directory exists first, since std::ofstream (what
        // SaveSettings uses) can't create missing parent directories itself.
        std::string PerUserSettingsPath(const std::string& username)
        {
            ::mkdir("settings", 0700);
            return "settings/" + username + ".txt";
        }

        // Parses a single OpenSSH authorized_keys-style line ("ssh-ed25519
        // AAAA... comment") into an ssh_key for comparison. Returns nullptr
        // on any parse failure or unrecognized key type -- callers treat
        // that the same as "key doesn't match."
        ssh_key ParsePublicKeyLine(const std::string& line)
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
            int listen_fd = -1;

            bool authenticated = false;
            std::string username;

            ssh_channel channel = nullptr;

            int pty_master_fd = -1;
            int pty_slave_fd = -1;
            bool shell_started = false;
            pid_t child_pid = -1;
        };

        int AuthPubkeyCallback(ssh_session session, const char* user, ssh_key pubkey,
                               char signature_state, void* userdata)
        {
            (void)session;
            ConnectionState* state = static_cast<ConnectionState*>(userdata);

            std::optional<User> found = state->db->GetUserByUsername(user);
            if (!found.has_value())
            {
                return SSH_AUTH_DENIED;
            }

            ssh_key stored_key = ParsePublicKeyLine(found->public_key);
            if (stored_key == nullptr)
            {
                return SSH_AUTH_DENIED;
            }
            bool matches = ssh_key_cmp(pubkey, stored_key, SSH_KEY_CMP_PUBLIC) == 0;
            ssh_key_free(stored_key);
            if (!matches)
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
                state->username = user;
                state->db->UpdateUserLastLogin(user, static_cast<std::int64_t>(std::time(nullptr)));
            }
            return SSH_AUTH_SUCCESS;
        }

        ssh_channel ChannelOpenCallback(ssh_session session, void* userdata)
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

        int PtyRequestCallback(ssh_session session, ssh_channel channel, const char* term,
                               int width, int height, int pxwidth, int pwheight, void* userdata)
        {
            (void)session;
            (void)channel;
            (void)term;
            ConnectionState* state = static_cast<ConnectionState*>(userdata);

            struct winsize window_size;
            std::memset(&window_size, 0, sizeof(window_size));
            window_size.ws_col = static_cast<unsigned short>(width);
            window_size.ws_row = static_cast<unsigned short>(height);
            window_size.ws_xpixel = static_cast<unsigned short>(pxwidth);
            window_size.ws_ypixel = static_cast<unsigned short>(pwheight);

            if (::openpty(&state->pty_master_fd, &state->pty_slave_fd, nullptr, nullptr,
                          &window_size) != 0)
            {
                return -1;
            }
            return 0;
        }

        int PtyWindowChangeCallback(ssh_session session, ssh_channel channel, int width, int height,
                                    int pxwidth, int pwheight, void* userdata)
        {
            (void)session;
            (void)channel;
            ConnectionState* state = static_cast<ConnectionState*>(userdata);
            if (state->pty_master_fd < 0)
            {
                return -1;
            }
            struct winsize window_size;
            std::memset(&window_size, 0, sizeof(window_size));
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
        int ShellRequestCallback(ssh_session session, ssh_channel channel, void* userdata)
        {
            (void)channel;
            ConnectionState* state = static_cast<ConnectionState*>(userdata);
            if (state->pty_slave_fd < 0)
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
                if (state->listen_fd >= 0)
                {
                    ::close(state->listen_fd);
                }
                ::close(ssh_get_fd(session));
                ::close(state->pty_master_fd);

                ::login_tty(state->pty_slave_fd);
                std::string username = state->username;
                RunInteractiveSession(PerUserSettingsPath(username), /*is_console_session=*/false);
                _exit(0);
            }

            ::close(state->pty_slave_fd);
            state->pty_slave_fd = -1;
            state->child_pid = pid;
            state->shell_started = true;
            return 0;
        }

        int ChannelDataCallback(ssh_session session, ssh_channel channel, void* data, uint32_t len,
                                int is_stderr, void* userdata)
        {
            (void)session;
            (void)channel;
            (void)is_stderr;
            ConnectionState* state = static_cast<ConnectionState*>(userdata);
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
        int PtyMasterReadableCallback(socket_t fd, int revents, void* userdata)
        {
            ConnectionState* state = static_cast<ConnectionState*>(userdata);
            if ((revents & POLLIN) == 0)
            {
                return 0;
            }
            char buffer[4096];
            ssize_t count = ::read(fd, buffer, sizeof(buffer));
            if (count <= 0)
            {
                return 0;
            }
            ssh_channel_write(state->channel, buffer, static_cast<uint32_t>(count));
            return static_cast<int>(count);
        }

        // Runs the whole lifecycle of one already-accepted connection: key
        // exchange, auth, channel/pty/shell setup, then relays bytes between
        // the pty and the SSH channel until either side goes away. Called in
        // a process forked solely for this one connection (see
        // SshAcceptLoop) -- returning from this function means that process
        // is done and should exit.
        void HandleConnection(ssh_session session, const std::string& db_path, int listen_fd)
        {
            ConnectionState state;
            state.listen_fd = listen_fd;

            if (ssh_handle_key_exchange(session) != SSH_OK)
            {
                ssh_free(session);
                return;
            }

            struct ssh_server_callbacks_struct server_callbacks;
            std::memset(&server_callbacks, 0, sizeof(server_callbacks));
            server_callbacks.userdata = &state;
            server_callbacks.auth_pubkey_function = AuthPubkeyCallback;
            server_callbacks.channel_open_request_session_function = ChannelOpenCallback;
            ssh_callbacks_init(&server_callbacks);
            ssh_set_server_callbacks(session, &server_callbacks);

            ssh_event event = ssh_event_new();
            ssh_event_add_session(event, session);

            std::time_t deadline = std::time(nullptr) + 60;

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
            // scoping was added). Nothing past this point needs `db` --
            // ChannelOpenCallback only checks state.authenticated.
            {
                Database db(db_path);
                state.db = &db;
                while (!state.authenticated && ssh_is_connected(session) &&
                       std::time(nullptr) < deadline)
                {
                    ssh_event_dopoll(event, 200);
                }
                state.db = nullptr;
            }

            if (!state.authenticated)
            {
                ssh_event_free(event);
                ssh_disconnect(session);
                ssh_free(session);
                return;
            }

            // Poll until a channel exists (the client opens one once it
            // sees auth succeeded) and, once channel callbacks are wired up
            // below, until a pty+shell has actually been started.
            struct ssh_channel_callbacks_struct channel_callbacks;
            std::memset(&channel_callbacks, 0, sizeof(channel_callbacks));
            channel_callbacks.userdata = &state;
            channel_callbacks.channel_pty_request_function = PtyRequestCallback;
            channel_callbacks.channel_pty_window_change_function = PtyWindowChangeCallback;
            channel_callbacks.channel_shell_request_function = ShellRequestCallback;
            channel_callbacks.channel_data_function = ChannelDataCallback;
            ssh_callbacks_init(&channel_callbacks);
            bool channel_callbacks_registered = false;

            while (!state.shell_started && ssh_is_connected(session) &&
                   std::time(nullptr) < deadline)
            {
                ssh_event_dopoll(event, 200);
                if (state.channel != nullptr && !channel_callbacks_registered)
                {
                    ssh_set_channel_callbacks(state.channel, &channel_callbacks);
                    channel_callbacks_registered = true;
                }
            }

            if (!state.shell_started)
            {
                ssh_event_free(event);
                ssh_disconnect(session);
                ssh_free(session);
                return;
            }

            ssh_event_add_fd(event, state.pty_master_fd, POLLIN, PtyMasterReadableCallback, &state);

            while (true)
            {
                ssh_event_dopoll(event, 200);

                int status = 0;
                pid_t reaped = waitpid(state.child_pid, &status, WNOHANG);
                if (reaped == state.child_pid)
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

            if (waitpid(state.child_pid, nullptr, WNOHANG) == 0)
            {
                kill(state.child_pid, SIGTERM);
                waitpid(state.child_pid, nullptr, 0);
            }

            ssh_event_remove_fd(event, state.pty_master_fd);
            ::close(state.pty_master_fd);
            ssh_event_free(event);

            ssh_channel_send_eof(state.channel);
            ssh_channel_close(state.channel);
            ssh_channel_free(state.channel);
            ssh_disconnect(session);
            ssh_free(session);
        }

        // Runs on a detached background thread for the lifetime of the
        // process: binds once, then loops forever accepting connections and
        // forking a dedicated process for each one (see HandleConnection).
        // A named class rather than a lambda, per this codebase's
        // convention -- std::thread needs a callable, and this one has real
        // state (the bound port) worth naming.
        class SshAcceptLoop
        {
        public:
            SshAcceptLoop(std::string db_path, int port) : db_path_(std::move(db_path)), port_(port)
            {
            }

            void operator()() const
            {
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
                    std::fprintf(stderr, "SSH: failed to listen on port %d: %s\n", port_,
                                 ssh_get_error(sshbind));
                    ssh_bind_free(sshbind);
                    return;
                }
                int listen_fd = ssh_bind_get_fd(sshbind);

                while (true)
                {
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
                        // connection's own session.
                        HandleConnection(session, db_path_, listen_fd);
                        _exit(0);
                    }

                    // The parent doesn't touch this connection again --
                    // HandleConnection took full ownership of `session` in
                    // the child's own address space.
                    ssh_free(session);
                }
            }

        private:
            std::string db_path_;
            int port_;
        };

    }  // namespace

    void StartSshServer(const std::string& db_path, int port)
    {
        std::thread(SshAcceptLoop(db_path, port)).detach();
    }

}  // namespace ql
