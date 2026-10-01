#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ql
{

    // Writes a ZIP archive at `zip_path` holding each of `file_paths`,
    // deflated, under its base name alone (no folders), replacing any file
    // already there (via TemporaryPathFor/ReplaceWithFile). `modified_at`
    // is the entries' date and time (local, as ZIP keeps it). Small files
    // only: each is read into memory whole, and nothing is ZIP64. For the
    // session export, which ZMODEM sends as one file. Returns true on
    // success; otherwise `error` is set to a short message.
    bool WriteZipArchive(const std::string& zip_path, const std::vector<std::string>& file_paths,
                         std::int64_t modified_at, std::string* error);

}  // namespace ql
