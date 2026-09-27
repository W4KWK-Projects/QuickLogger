#pragma once

#include <string>

namespace ql
{

    // Builds and runs one full interactive QuickLogger session: opens the
    // shared quicklogger.db, loads `settings_path` (per-session -- see
    // AppState::is_console_session/settings.hpp for why this varies while
    // the database itself never does), builds every page, and blocks in
    // ScreenInteractive::Fullscreen()'s event loop until the operator quits
    // or the terminal goes away.
    //
    // Called exactly once by main() for the normal local/console launch,
    // and once per accepted connection by ssh_server.cpp's forked child --
    // an SSH session is just another interactive session, differing only in
    // which settings file it opens and `is_console_session`. This is the
    // whole reason ssh_server.hpp needed no changes to any page/handler:
    // by the time this function is reached, it has no idea whether its
    // controlling terminal is a real console or a pty an SSH connection was
    // just wired onto.
    //
    // `ssh_username` is who logged in over SSH (blank at the console); a
    // view-only user's session can change nothing but its own settings
    // (see AppState::view_only_user).
    void RunInteractiveSession(const std::string& settings_path, bool is_console_session,
                               const std::string& ssh_username);

}  // namespace ql
