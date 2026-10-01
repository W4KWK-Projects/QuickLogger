#pragma once

#include <string>
#include <vector>

namespace ql
{

    // The modes a net can be on, in the order the Mode choice shows them.
    // Each maps directly to an ADIF MODE (and SUBMODE); see AdifMode.
    const std::vector<std::string>& NetModes();

    // `text` as one of NetModes(), or "" if it isn't one. Case, spaces,
    // hyphens and slashes are ignored, and a few other names for the same
    // mode are taken ("USB"/"LSB" for SSB, "DSTAR", "YSF"/"C4FM" for
    // Fusion). For a mode typed before 1.8.0, when Mode was free text, and
    // for a net imported from a file one of those versions wrote.
    std::string NormalizeMode(const std::string& text);

    // `mode`'s index in NetModes(), or -1 if it isn't one.
    int NetModeIndex(const std::string& mode);

    // `mode` (one of NetModes()) as ADIF's MODE and SUBMODE: FM, SSB, AM and
    // CW as they are, and the digital voice modes as MODE DIGITALVOICE with
    // SUBMODE DSTAR, DMR or C4FM. Both are "" for a blank or unknown mode.
    void AdifMode(const std::string& mode, std::string* adif_mode, std::string* adif_submode);

}  // namespace ql
