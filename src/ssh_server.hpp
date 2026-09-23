#pragma once

#include <string>

#include <sys/types.h>

namespace ql
{

    // QuickLogger's built-in SSH server: generates (on first run) or loads a
    // host key, binds to `port` on every local address, and runs an accept
    // loop forever.
    //
    // Each accepted connection is handled in its own forked process (never
    // a thread -- see interactive_session.hpp's doc comment for why), which
    // authenticates the client against Database::GetUserByUsername via
    // public-key signature verification (no passwords), then on a pty+shell
    // request forks again, `login_tty()`s the new pty's slave side onto the
    // grandchild, and calls RunInteractiveSession(..., /*is_console_session=*/false)
    // -- an SSH session ends up being handled by the exact same code as a
    // local console session, just with a different settings path and a pty
    // that isn't a real terminal.
    //
    // THE FORK RULE: SQLite must never be opened in a fork()ed child of a
    // process that has (or had) a SQLite connection open. SQLite's per-
    // process lock bookkeeping is copied into the child by fork() but the
    // child's own POSIX locks are not, so the child's brand-new connection
    // to the same file fails with "locking protocol" / "disk I/O error"
    // (SQLite documents this; reproduced against this app -- it was the
    // long-standing "intermittent SSH connection failure" and in fact an
    // uncaught exception aborting the connection's process). Every process
    // that accepts or forks SSH connections must therefore be one that has
    // never opened the database itself -- which is why the console
    // session's listener is its own process (StartSshServerProcess) rather
    // than a thread of the console process.

    // Runs the accept loop in the *calling* process and blocks forever. For
    // `--headless`, where nothing else in the process ever opens the
    // database. Returns early only if the host key can't be created/loaded
    // or the port can't be bound (logged to stderr).
    void RunSshServer(const std::string& db_path, int port);

    // Forks a dedicated listener process running the same accept loop and
    // returns its pid, or -1 if the fork failed. For a normal console
    // launch, where the calling process goes on to open the database for
    // its own session. Must be called *before* the caller opens any SQLite
    // connection or starts any other thread (see THE FORK RULE above). The
    // listener exits by itself if the calling process goes away.
    pid_t StartSshServerProcess(const std::string& db_path, int port);

    // Stops (and reaps) a listener started by StartSshServerProcess. Safe
    // to call with -1. Sessions already established keep running -- each is
    // an independent process.
    void StopSshServerProcess(pid_t listener_pid);

}  // namespace ql
