#pragma once

#include <string>

namespace ftxui
{
    class ScreenInteractive;
}

namespace ql
{

    // True if the `sz` command (from the lrzsz package) needed to send a
    // file via ZMODEM is installed and on PATH. Checked before attempting a
    // transfer, so a missing tool produces a clear message instead of a
    // doomed exec().
    bool ZmodemSendAvailable();

    // True if `rz` (the receiving half of the same lrzsz package) is
    // installed and on PATH. lrzsz always installs both together, but this
    // is checked independently rather than assumed from ZmodemSendAvailable
    // in case that ever stops being true on some platform.
    bool ZmodemReceiveAvailable();

    // Sends `path` to whatever's on the other end of the real terminal via
    // the ZMODEM protocol, so a ZMODEM-aware terminal client (e.g. one with
    // auto-detect/receive enabled, like ZOC) can offer to save it locally --
    // this is how a *remote*, SSH'd-in user gets a copy of a file that only
    // ever otherwise existed on the machine QuickLogger runs on.
    //
    // Temporarily uninstalls `screen`'s terminal hooks (raw mode/alternate
    // screen buffer -- see ScreenInteractive::WithRestoredIO) so `sz`'s raw
    // protocol bytes go straight to the real terminal instead of being
    // interleaved with FTXUI's own output, forks and execs `sz path`
    // inheriting the real stdin/stdout, waits for it to finish (killing it
    // and giving up after a bounded timeout if no receiver ever responds --
    // `sz` otherwise retries indefinitely), then restores the screen.
    //
    // Blocks the calling thread for as long as the transfer (or the
    // timeout) takes -- meant to be called directly from a key handler on
    // the UI thread, the same way any use of WithRestoredIO is meant to be.
    // Returns true if `sz` exited zero; on failure or timeout, `error` is
    // set to a short message and the caller should still treat `path` as
    // having been written successfully (this only concerns whether it also
    // reached a remote client).
    bool SendFileViaZmodem(ftxui::ScreenInteractive* screen, const std::string& path,
                           std::string* error);

    // The receiving mirror of SendFileViaZmodem: suspends `screen`'s
    // terminal hooks the same way, forks and execs `rz` with its working
    // directory set to `dest_dir` (whatever file the sending client offers
    // lands there, under whatever name it sends -- `rz` doesn't take a
    // destination filename the way `sz` takes a source one) and the real
    // stdin/stdout inherited, waits for it with the same bounded timeout,
    // then restores the screen. Blocks the calling thread the same way
    // SendFileViaZmodem does. Returns true if `rz` exited zero; on failure
    // or timeout, `error` is set to a short message.
    bool ReceiveFileViaZmodem(ftxui::ScreenInteractive* screen, const std::string& dest_dir,
                              std::string* error);

}  // namespace ql
