#include <curl/curl.h>

#include "interactive_session.hpp"

int main()
{
    // Must happen before any thread (including a background ULS import, or
    // -- once the SSH listener is wired in -- an accepted connection's
    // forked child) could call into libcurl.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    ql::RunInteractiveSession("settings.txt", /*is_console_session=*/true);
    return 0;
}
