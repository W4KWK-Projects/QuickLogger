#pragma once

#include <string>
#include <vector>

namespace ql
{

    // Extracts the entries of the ZIP archive at `zip_path` whose file names
    // are listed in `wanted_names` into `dest_dir`, overwriting any existing
    // files there. Entry names are matched, and files written, by their base
    // name alone -- any directory part inside the archive is ignored (like
    // `unzip -j`), which also means a hostile archive can't write outside
    // `dest_dir`.
    //
    // Done in-process with zlib rather than by running the `unzip` command:
    // that's one fewer thing an operator has to have installed (it isn't
    // there by default on many minimal Linux installs, and never on
    // Windows), and it works identically everywhere.
    //
    // Handles what the FCC/Census downloads actually use -- entries that are
    // stored or deflated, sizes below 4 GiB. Entries are streamed through a
    // small buffer (the FCC's are hundreds of megabytes each) and checked
    // against the archive's own CRC-32 and size. Returns true only if every
    // name in `wanted_names` was found and extracted intact; otherwise
    // `error` is set to a short message.
    bool ExtractZipEntries(const std::string& zip_path, const std::string& dest_dir,
                           const std::vector<std::string>& wanted_names, std::string* error);

}  // namespace ql
