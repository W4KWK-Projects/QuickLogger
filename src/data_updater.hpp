#pragma once

#include <string>

namespace ql
{

    // The data updater keeps the shared station data (see uls_import.hpp)
    // loaded and current for everyone, independent of any session: it checks
    // every few seconds whether anything is due -- first run, weekly FCC
    // refresh, a retry after a failure, or a refresh someone asked for from
    // the console's Settings page -- and if so claims the job (only one
    // updater does the work, even with several QuickLogger instances on one
    // database) and runs it, writing progress where every session can read
    // it.
    //
    // On macOS/Linux/FreeBSD it runs as its own small process, forked by
    // main() -- like the SSH listener, and for the same reason (THE FORK RULE
    // in ssh_server.hpp): it must be forked before the calling process opens
    // the database or starts a thread. It stops on its own if the process
    // that started it goes away. Each refresh runs in a further short-lived
    // child of the updater, so the memory a refresh needs is returned as soon
    // as it's done. On Windows there's no fork(), and no SSH sessions to
    // outlive, so it runs as a thread of the console process instead.
    //
    // Only one updater per process; Start a second time is a no-op.
    void StartDataUpdater(const std::string& db_path);

    // Stops the updater, abandoning a refresh in progress (it's picked up
    // again next time the app starts) and waiting for it to wind down. Safe
    // to call if it was never started.
    void StopDataUpdater();

}  // namespace ql
