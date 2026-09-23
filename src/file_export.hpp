#pragma once

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

    // Writes `lines` to `path`, one per line (LF-terminated), overwriting
    // any existing file there. Creates `path`'s parent directory first
    // (see EnsureDirectory) so callers don't need the exports directory to
    // already exist. Returns true on success; on failure, `error` is set to
    // a short message.
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
