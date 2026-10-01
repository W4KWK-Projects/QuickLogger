#pragma once

#include <string>

#include <libssh/libssh.h>

namespace ql
{

    // Serves SFTP on one SSH channel (the client asked for the "sftp"
    // subsystem): an alternative to ZMODEM for terminals without it, run in
    // the connection's own process (see ssh_server.hpp) instead of a
    // session.
    //
    // libssh's ready-made SFTP server would hand out the real file system,
    // so this answers each request itself, against the tiny tree in
    // sftp_paths.hpp: the user's own /exports, read-only, and /imports.
    // Only listing, stat, realpath, open/read/write/close and removing an
    // upload are supported (and setstat, accepted but ignored, since scp
    // insists on it); everything else is refused.
    //
    // /imports is read-only for a view-only user. For everyone else it
    // takes new .qlnet and .qlsession files (IsAllowedImportName) of up to
    // kSftpMaxUploadBytes each and kSftpMaxImportsBytes in all, and they
    // can remove those. An upload is written
    // to a hidden temporary file and moved into place when it's closed, so
    // the Import page never lists half a file.
    //
    // Never opens the database. Returns once the client closes the channel
    // or the connection ends.
    void RunSftpSession(ssh_session session, ssh_channel channel, const std::string& db_path,
                        const std::string& username, bool view_only);

}  // namespace ql
