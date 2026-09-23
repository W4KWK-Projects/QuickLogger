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

    // Writes `lines` to `path`, one per line (LF-terminated), overwriting
    // any existing file there. Creates `path`'s parent directory first
    // (mkdir -p, same approach as UlsCacheDir's caller in uls_import.cpp)
    // so callers don't need the exports directory to already exist. Returns
    // true on success; on failure, `error` is set to a short message.
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
