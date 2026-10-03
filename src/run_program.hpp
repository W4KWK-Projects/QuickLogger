#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace ql
{

    // Running another program (ssh and scp, for pushing a session upstream)
    // and collecting what it prints. Never through a shell: the program
    // gets `arguments` as they are, one argv entry each, so nothing in them
    // (a host or user name, say) can turn into another command.

    // The full path of the program `name` ("ssh"): the first found in the
    // directories on PATH, then where the system keeps its own (on Windows,
    // %SystemRoot%\System32\OpenSSH, where Windows 10 and 11 install
    // OpenSSH, and ".exe" is added to `name`). Empty if there's none.
    std::string FindProgramOnPath(const std::string& name);

    struct ProgramResult
    {
        // False if it couldn't be started at all (`error` says why).
        bool started = false;
        // Stopped by RunProgram: it ran past its time, or was cancelled.
        bool stopped = false;
        // Its exit status, once it ended on its own; -1 otherwise.
        int exit_status = -1;
        std::string output;  // Standard output.
        std::string errors;  // Standard error.
        std::string error;
    };

    // Runs the program at `path` with `arguments` (not including argv[0])
    // and waits for it, with nothing on its standard input. Each of its
    // outputs is kept up to 64 KB. Stopped if it's still running after
    // `timeout_seconds`, or as soon as `*cancel` (if given) becomes true.
    ProgramResult RunProgram(const std::string& path, const std::vector<std::string>& arguments, int timeout_seconds,
                             const std::atomic<bool>* cancel);

    // `arguments` as one Windows command line, each quoted so that the
    // program's C runtime splits it back into the same arguments
    // (CommandLineToArgvW's rules). Used by RunProgram on Windows; here for
    // tests everywhere.
    std::string WindowsCommandLine(const std::string& program, const std::vector<std::string>& arguments);

}  // namespace ql
