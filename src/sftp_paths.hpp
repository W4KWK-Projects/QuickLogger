#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ql
{

    // The little file system an SSH user sees over SFTP (sftp_server.hpp):
    // "/" holds just two folders, "/exports" and "/imports", which are that
    // user's own SessionExportsDir and SessionImportsDir (file_export.hpp).
    // Nothing else on the server is reachable. Both folders are flat: only
    // files directly inside them, no subfolders.
    //
    // Kept apart from the libssh code so the rules can be tested in a build
    // without the SSH server.

    enum class SftpArea
    {
        kRoot,
        kExports,
        kImports,
    };

    // A client's path, resolved.
    struct SftpPath
    {
        SftpArea area = SftpArea::kRoot;
        // The file's name inside `area`; blank for the folder itself (and
        // always for kRoot).
        std::string name;
    };

    // The largest file an upload to /imports may grow to.
    constexpr std::uint64_t kSftpMaxUploadBytes = 25 * 1024 * 1024;

    // The most a user's /imports may hold altogether.
    constexpr std::uint64_t kSftpMaxImportsBytes = 100 * 1024 * 1024;

    // The size of the files directly in `dir`, hidden ones included (an
    // upload in progress counts), except `except_name`, which an upload is
    // about to replace. 0 if `dir` doesn't exist yet.
    std::uint64_t SftpImportsBytesUsed(const std::string& dir, std::string_view except_name);

    // How large a new upload may grow when /imports already holds
    // `used_bytes`: kSftpMaxUploadBytes, or less if that would take it
    // over kSftpMaxImportsBytes. Two uploads at once can each have the
    // whole of what's left, so the total can briefly run over by one
    // file's worth; the next upload then gets nothing.
    std::uint64_t SftpUploadLimit(std::uint64_t used_bytes);

    // Resolves `client_path` (absolute, or relative to "/"; "." and ".."
    // and repeated slashes allowed, as clients send them) into `out`.
    // Returns false for anything outside the tree above: another top-level
    // name, a path below a file, or a file name that's hidden (starts with
    // '.', as a half-written upload's temporary file does) -- callers
    // answer "no such file".
    bool ResolveSftpPath(std::string_view client_path, SftpPath* out);

    // `path` as an absolute path again: "/", "/imports",
    // "/imports/Net.qlnet". What REALPATH answers.
    std::string SftpPathString(const SftpPath& path);

    // True if `name` may be uploaded to (or removed from) /imports: a
    // .qlnet or .qlsession file whose name SanitizeFilenameComponent would
    // leave as it is, so it can't be mistaken for anything else on disk.
    bool IsAllowedImportName(std::string_view name);

    // An `ls -l` style line for a directory listing, which OpenSSH's sftp
    // shows for `ls -l`: permissions, link count, owner and group (both
    // `owner`), size, modification time and name. `permissions` includes
    // the file-type bits (0040000 for a folder).
    std::string SftpLongName(std::string_view name, std::uint32_t permissions, std::uint64_t size, std::int64_t mtime,
                             std::string_view owner);

}  // namespace ql
