#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ql
{

    // The directory plain-text exports (net logs, saved-station lists, and
    // eventually database-slice exports) get written into: a sibling
    // "exports" directory next to `db_path`, mirroring how uls_import.cpp's
    // UlsCacheDir sits next to the database rather than under the current
    // working directory specifically -- both should move together if the app
    // is ever run from a different directory than where its data lives.
    std::string ExportsDir(const std::string& db_path);

    // The directory a net-slice file (see net_slice.hpp) received from
    // another QuickLogger user is expected to be placed into before it can
    // be imported -- a sibling "imports" directory next to `db_path`, same
    // rationale as ExportsDir.
    std::string ImportsDir(const std::string& db_path);

    // An SSH user's own exports and received imports: "ssh-users/<username>"
    // inside ExportsDir / ImportsDir, apart from the console's files and
    // each other's. With a blank `ssh_username` (the console), just
    // ExportsDir / ImportsDir. An SSH user's files are only staging for a
    // ZMODEM transfer, so they're cleaned up after a while (see
    // RemoveOldSshUserFiles); the console's are kept for good.
    std::string SessionExportsDir(const std::string& db_path, const std::string& ssh_username);
    std::string SessionImportsDir(const std::string& db_path, const std::string& ssh_username);

    // An SSH user's own settings file: "settings/<username>.txt" next to
    // `db_path` (the same file ssh_server.cpp's PerUserSettingsPath opens).
    std::string SshUserSettingsPath(const std::string& db_path, const std::string& username);

    // Moves an SSH user's settings file and their export and import
    // directories (see SessionExportsDir) from `old_username`'s names to
    // `new_username`'s, when the user is renamed. Anything already under
    // the new names (left by a user removed earlier) is replaced. What
    // doesn't exist is skipped. Returns false, with `error` set, if
    // something couldn't be moved.
    bool MoveSshUserFiles(const std::string& db_path, const std::string& old_username,
                          const std::string& new_username, std::string* error);

    // How long an SSH user's exported and received files are kept.
    constexpr std::int64_t kSshUserFileMaxAgeSeconds = 7 * 24 * 60 * 60;

    // Deletes every file under the SSH users' export and import directories
    // (see SessionExportsDir) last changed more than `max_age_seconds` ago,
    // and any user directory left empty. The console's own files, directly
    // in ExportsDir / ImportsDir, are never touched. Returns how many files
    // were deleted.
    int RemoveOldSshUserFiles(const std::string& db_path, std::int64_t max_age_seconds);

    // Lists the files directly inside `dir` whose name ends in `extension`
    // (e.g. ".qlnet"), sorted alphabetically. Returns an empty list (not an
    // error) if `dir` doesn't exist yet -- callers should treat "no files"
    // and "no directory" the same way, since ImportsDir isn't created until
    // the first file lands there.
    std::vector<std::string> ListFilesWithExtension(const std::string& dir,
                                                    const std::string& extension);

    // Creates `dir` and any missing parents (like `mkdir -p`); succeeds if it
    // already exists. Done with std::filesystem rather than shelling out to
    // `mkdir`, which isn't a command on Windows and would run whatever a
    // stray quote in `dir` happened to smuggle into the shell everywhere
    // else. Returns false if the directory doesn't exist afterward.
    bool EnsureDirectory(const std::string& dir);

    // A not-yet-existing path next to `path` to build a file at before
    // moving it into place with ReplaceWithFile, so that two sessions
    // exporting the same file at once (the same net's log on the same day)
    // can't interleave their writes: each writes its own temporary file and
    // the last move wins, whole.
    std::string TemporaryPathFor(const std::string& path);

    // Moves `temp_path` to `path`, replacing any file there. On failure,
    // removes `temp_path`, sets `error` and returns false.
    bool ReplaceWithFile(const std::string& temp_path, const std::string& path, std::string* error);

    // Writes `lines` to `path`, one per line (LF-terminated), overwriting
    // any existing file there (via TemporaryPathFor/ReplaceWithFile, so a
    // reader never sees it half-written). Creates `path`'s parent directory
    // first (see EnsureDirectory) so callers don't need the exports
    // directory to already exist. Returns true on success; on failure,
    // `error` is set to a short message.
    bool WriteExportFile(const std::string& path, const std::vector<std::string>& lines,
                         std::string* error);

    // Builds a filesystem-safe file name component from free-form text (a
    // net name might contain spaces, slashes, or other punctuation a real
    // path can't/shouldn't contain): keeps letters, digits, '.', '_', '-',
    // and collapses every run of anything else into a single '_'. Never
    // returns an empty string -- falls back to "export" if `text` had
    // nothing safe to keep (e.g. it was empty or pure punctuation).
    std::string SanitizeFilenameComponent(const std::string& text);

}  // namespace ql
