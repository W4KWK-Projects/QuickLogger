#pragma once

#include <string>
#include <vector>

namespace ql
{

    // True when QuickLogger is running in a terminal on the machine the
    // operator is sitting at: the console session (not one handed off from
    // QuickLogger's own SSH server), and not inside an ordinary SSH login
    // either. There, ZMODEM has nobody on the other end to send to; the
    // files are already on the operator's own computer.
    bool IsLocalTerminal(bool is_console_session);

    // True if this machine has a desktop to open a file manager window on:
    // always on macOS and Windows, and on Linux/FreeBSD when a graphical
    // session is running (DISPLAY or WAYLAND_DISPLAY is set).
    bool CanShowInFileManager();

    // Opens the folder holding `paths` (all in one folder) in the desktop's
    // file manager (Finder, File Explorer, or whichever the Linux/FreeBSD
    // desktop uses), with every one of them selected where the file
    // manager supports that. Doesn't wait for it; the file manager's output
    // is discarded so it can't mark up the screen. On failure to start it,
    // says why in `error`.
    bool ShowInFileManager(const std::vector<std::string>& paths, std::string* error);

}  // namespace ql
