#pragma once

#include <string>

namespace ql
{

    // Starts QuickLogger's built-in SSH server: generates (on first run) or
    // loads a host key, binds to `port` on every local address, and runs a
    // blocking accept loop forever on a detached background thread -- safe
    // to call once from main() alongside the normal local console session,
    // which keeps running independently in the caller's own thread.
    //
    // Each accepted connection is handled in its own forked process (never
    // a thread -- see interactive_session.hpp's doc comment for why), which
    // authenticates the client against Database::GetUserByUsername via
    // public-key signature verification (no passwords), then on a pty+shell
    // request forks again, `login_tty()`s the new pty's slave side onto the
    // grandchild, and calls RunInteractiveSession(..., /*is_console_session=*/false)
    // -- an SSH session ends up being handled by the exact same code as a
    // local console session, just with a different settings path and a pty
    // that isn't a real terminal. Returns immediately; does nothing if the
    // host key can't be created/loaded (logged to stderr) or the port can't
    // be bound.
    void StartSshServer(const std::string& db_path, int port);

}  // namespace ql
