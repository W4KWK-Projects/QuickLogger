#pragma once

#include <string>

namespace ql
{

    // Where the newest release is described: GitHub's "latest release",
    // which never includes drafts or pre-releases.
    constexpr const char* kLatestReleaseUrl =
        "https://api.github.com/repos/W4KWK-Projects/QuickLogger/releases/latest";
    // Where to get it, as shown to the operator.
    constexpr const char* kReleasesPageUrl =
        "https://github.com/W4KWK-Projects/QuickLogger/releases/latest";

    // The version in a GitHub release's JSON (its "tag_name", without the
    // leading "v"), or "" if there's none.
    std::string ReleaseVersionFromJson(const std::string& json);

    // True if `candidate` is a later version than `current`, comparing each
    // dot-separated part as a number ("1.7.10" is later than "1.7.9"). False
    // if either isn't plain digits and dots.
    bool IsNewerVersion(const std::string& candidate, const std::string& current);

    // Fetches the latest release's version from `url` (kLatestReleaseUrl).
    // Returns false, with `error` set, if it couldn't be found.
    bool FetchLatestReleaseVersion(const std::string& url, std::string* version,
                                   std::string* error);

    // The newer version this console session has found, or "" if none
    // (or the check is turned off). Set by the update checker's thread,
    // read when the top bar and Settings are drawn.
    std::string AvailableUpdate();
    void SetAvailableUpdate(const std::string& version);

    // Whether the console operator wants the check (Settings' Update Check;
    // AppSettings::check_for_updates). Turning it off also clears any
    // version already found.
    bool UpdateCheckEnabled();
    void SetUpdateCheckEnabled(bool enabled);

}  // namespace ql
