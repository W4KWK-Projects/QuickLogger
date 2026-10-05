#pragma once

#include <string>
#include <vector>

namespace ftxui
{
    class ScreenInteractive;
}

namespace ql
{

    // How long a send waits for a person to start their terminal's receive.
    constexpr int kZmodemWaitSeconds = 25;
    // How long it waits when it only asks whether the terminal speaks ZMODEM
    // (a terminal that auto-detects it answers in a fraction of a second).
    constexpr int kZmodemProbeSeconds = 4;

    // True if the `sz` command (from the lrzsz package; `lsz` on FreeBSD)
    // needed to send a file via ZMODEM is installed, on PATH or in one of
    // the usual package directories. Checked before attempting a
    // transfer, so a missing tool produces a clear message instead of a
    // doomed exec().
    bool ZmodemSendAvailable();

    // True if `rz` (the receiving half of the same lrzsz package; `lrz` on
    // FreeBSD) is installed, found the same way. lrzsz always installs both together, but this
    // is checked independently rather than assumed from ZmodemSendAvailable
    // in case that ever stops being true on some platform.
    bool ZmodemReceiveAvailable();

    // True where ZMODEM can't be had at all, so it's never offered: Windows
    // (no SSH server). Everywhere else it's built in (zmodem_protocol.hpp),
    // Alpine Linux included, with nothing to install.
    bool NoZmodemOnThisSystem();

    // Sends `paths` (one ZMODEM batch) to whatever's on the other end of the real terminal via
    // the ZMODEM protocol, so a ZMODEM-aware terminal client (e.g. one with
    // auto-detect/receive enabled, like ZOC) can offer to save it locally --
    // this is how a *remote*, SSH'd-in user gets a copy of a file that only
    // ever otherwise existed on the machine QuickLogger runs on.
    //
    // Temporarily uninstalls `screen`'s terminal hooks (raw mode/alternate
    // screen buffer -- see ScreenInteractive::WithRestoredIO) so `sz`'s raw
    // protocol bytes go straight to the real terminal instead of being
    // interleaved with FTXUI's own output, forks and execs `sz paths...`
    // inheriting the real stdin/stdout, waits for it to finish (killing it
    // and giving up after a bounded timeout if no receiver ever responds --
    // `sz` otherwise retries indefinitely), then restores the screen.
    //
    // Blocks the calling thread for as long as the transfer (or the
    // timeout) takes -- meant to be called directly from a key handler on
    // the UI thread, the same way any use of WithRestoredIO is meant to be.
    // Returns true if `sz` exited zero; on failure or timeout, `error` is
    // set to a short message and the caller should still treat `paths` as
    // having been written successfully (this only concerns whether they
    // also reached a remote client).
    // `start_timeout_seconds` is how long to wait for the terminal to answer
    // the opening request: a short one (see kZmodemProbeSeconds) asks "does
    // this terminal speak ZMODEM?"; the long default leaves time for a
    // person to start their terminal's receive. With no terminal at all
    // (`screen` null, or standard input not a terminal) it fails at once,
    // as a terminal that never answers would.
    bool SendFilesViaZmodem(ftxui::ScreenInteractive* screen, const std::vector<std::string>& paths, std::string* error,
                            int start_timeout_seconds = kZmodemWaitSeconds);

    // The receiving mirror of SendFilesViaZmodem: suspends `screen`'s
    // terminal hooks the same way, forks and execs `rz` with its working
    // directory set to `dest_dir` (whatever file the sending client offers
    // lands there, under whatever name it sends -- `rz` doesn't take a
    // destination filename the way `sz` takes a source one) and the real
    // stdin/stdout inherited, waits for it with the same bounded timeout,
    // then restores the screen. Blocks the calling thread the same way
    // SendFilesViaZmodem does. Returns true if `rz` exited zero; on failure
    // or timeout, `error` is set to a short message.
    bool ReceiveFileViaZmodem(ftxui::ScreenInteractive* screen, const std::string& dest_dir, std::string* error);

}  // namespace ql
