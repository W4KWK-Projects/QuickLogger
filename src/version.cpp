#include "version.hpp"

// Defined for this file only, from CMakeLists.txt's project() version, so
// changing the version rebuilds just this file.
#ifndef QUICKLOGGER_VERSION
#define QUICKLOGGER_VERSION "unknown"
#endif

namespace ql
{

    const char* QuickLoggerVersion()
    {
        return QUICKLOGGER_VERSION;
    }

}  // namespace ql
