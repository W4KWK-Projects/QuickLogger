cmake_minimum_required(VERSION 3.16)
project(QuickLogger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG        v5.0.0
)
FetchContent_MakeAvailable(ftxui)

find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(libssh REQUIRED)

add_executable(QuickLogger
    src/main.cpp
    src/interactive_session.cpp
    src/ssh_server.cpp
    src/date_utils.cpp
    src/settings.cpp
    src/text_utils.cpp
    src/geo_utils.cpp
    src/file_export.cpp
    src/net_slice.cpp
    src/zmodem_send.cpp
    src/uls_import.cpp
    src/db/database.cpp
    src/db/sqlite_statement.cpp
    src/ui/app_state.cpp
    src/ui/chrome.cpp
    src/ui/handlers.cpp
    src/ui/pages.cpp
)
target_link_libraries(QuickLogger
    PRIVATE
        ftxui::component
        ftxui::dom
        ftxui::screen
        SQLite3::SQLite3
        CURL::libcurl
        Threads::Threads
        ssh
)
# openpty()/login_tty() (ssh_server.cpp) live in libutil on Linux and
# FreeBSD, but are part of libSystem on macOS -- there's no separate
# libutil to link there, and trying to would fail.
if(NOT APPLE)
    target_link_libraries(QuickLogger PRIVATE util)
endif()
