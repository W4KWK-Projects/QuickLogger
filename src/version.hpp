#pragma once

namespace ql
{

    // QuickLogger's version, e.g. "1.0.1" -- set in one place, the
    // project() line in CMakeLists.txt, and shown in every page's top bar
    // and by --version. A release is tagged "v" + this (the release
    // workflow checks the two agree).
    const char* QuickLoggerVersion();

}  // namespace ql
