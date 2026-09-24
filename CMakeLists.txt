cmake_minimum_required(VERSION 3.16)
# The version shown in the app (see src/version.hpp). A release is tagged
# "v" + this version, e.g. v1.0.1.
project(QuickLogger VERSION 1.0.1 LANGUAGES CXX)

# The code needs nothing newer than C++17 (checked by compiling every source
# file as C++17), so that's what it asks for -- a higher standard would only
# rule out older-but-still-supported toolchains (and CMake before 3.20 doesn't
# even know "23").
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The built-in SSH server is built on fork(), pseudo-terminals and libssh's
# server API, none of which exist on Windows -- so it's on everywhere except
# Windows by default, and can be switched off anywhere (a console-only build).
if(WIN32)
    set(quicklogger_ssh_default OFF)
else()
    set(quicklogger_ssh_default ON)
endif()
option(QUICKLOGGER_ENABLE_SSH
       "Build the built-in SSH server (needs libssh; not available on Windows)"
       ${quicklogger_ssh_default})
if(QUICKLOGGER_ENABLE_SSH AND WIN32)
    message(FATAL_ERROR
            "The built-in SSH server isn't supported on Windows. "
            "Configure with -DQUICKLOGGER_ENABLE_SSH=OFF.")
endif()

# FTXUI (the terminal UI library) is downloaded and built as part of this
# build, which needs `git` and network access the first time. To build
# offline, point FETCHCONTENT_SOURCE_DIR_FTXUI at an existing checkout of
# FTXUI v5.0.0 (cmake -DFETCHCONTENT_SOURCE_DIR_FTXUI=/path/to/FTXUI ...).
include(FetchContent)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG        v5.0.0
)
FetchContent_MakeAvailable(ftxui)

find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# Everything but main() goes into a library, so the test suite (tests/) can
# link exactly the code the app runs.
set(quicklogger_sources
    src/interactive_session.cpp
    src/date_utils.cpp
    src/settings.cpp
    src/text_utils.cpp
    src/version.cpp
    src/geo_utils.cpp
    src/file_export.cpp
    src/net_slice.cpp
    src/public_key.cpp
    src/zmodem_send.cpp
    src/zip_extract.cpp
    src/uls_import.cpp
    src/data_updater.cpp
    src/db/database.cpp
    src/db/sqlite_statement.cpp
    src/ui/app_state.cpp
    src/ui/chrome.cpp
    src/ui/handlers.cpp
    src/ui/pages.cpp
)

if(QUICKLOGGER_ENABLE_SSH)
    # libssh installs a CMake package that provides the imported target `ssh`
    # (Homebrew, Debian/Ubuntu, Fedora, ...); fall back to pkg-config for
    # installs that only ship libssh.pc. Note this is `libssh`, not `libssh2`
    # -- an unrelated project without server support.
    find_package(libssh CONFIG QUIET)
    if(TARGET ssh)
        set(quicklogger_libssh_target ssh)
    elseif(TARGET libssh::ssh)
        set(quicklogger_libssh_target libssh::ssh)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBSSH IMPORTED_TARGET libssh)
        endif()
        if(TARGET PkgConfig::LIBSSH)
            set(quicklogger_libssh_target PkgConfig::LIBSSH)
        endif()
    endif()
    if(NOT quicklogger_libssh_target)
        message(FATAL_ERROR
                "libssh (development files) not found. Install it (see the README), "
                "or configure with -DQUICKLOGGER_ENABLE_SSH=OFF for a build without "
                "the built-in SSH server.")
    endif()
    list(APPEND quicklogger_sources src/ssh_server.cpp)
endif()

add_library(quicklogger_core STATIC ${quicklogger_sources})
set_source_files_properties(src/version.cpp PROPERTIES
    COMPILE_DEFINITIONS "QUICKLOGGER_VERSION=\"${PROJECT_VERSION}\"")
add_executable(QuickLogger src/main.cpp)
target_link_libraries(QuickLogger PRIVATE quicklogger_core)

target_link_libraries(quicklogger_core
    PUBLIC
        ftxui::component
        ftxui::dom
        ftxui::screen
        SQLite3::SQLite3
        CURL::libcurl
        ZLIB::ZLIB
        Threads::Threads
)

if(QUICKLOGGER_ENABLE_SSH)
    target_compile_definitions(quicklogger_core PUBLIC QUICKLOGGER_WITH_SSH)
    target_link_libraries(quicklogger_core PUBLIC ${quicklogger_libssh_target})

    # openpty()/login_tty() (ssh_server.cpp) live in libutil on Linux and
    # FreeBSD, but are part of libSystem on macOS -- there's no separate
    # libutil to link there. On newer glibc (2.34+) libutil is an empty
    # stub, and a future release may drop it entirely, so it's only linked if
    # it's actually there.
    if(NOT APPLE)
        find_library(quicklogger_util_library util)
        if(quicklogger_util_library)
            target_link_libraries(quicklogger_core PUBLIC ${quicklogger_util_library})
        endif()
    endif()
endif()

if(WIN32)
    # NOMINMAX / WIN32_LEAN_AND_MEAN keep <windows.h> (pulled in by libcurl
    # and FTXUI) from defining min()/max() macros that break std::min/max.
    target_compile_definitions(quicklogger_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
                               _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        # The sources are UTF-8 (box-drawing characters, em dashes in
        # string literals); MSVC otherwise assumes the system code page.
        target_compile_options(quicklogger_core PUBLIC /utf-8)
    endif()
endif()

# The test suite: `cmake --build build && ctest --test-dir build`. It never
# touches a real quicklogger.db -- every test works in its own temporary
# directory -- and needs no network (the station-data tests download from
# local file:// fixtures). Off with -DQUICKLOGGER_BUILD_TESTS=OFF.
option(QUICKLOGGER_BUILD_TESTS "Build the test suite" ON)
if(QUICKLOGGER_BUILD_TESTS)
    enable_testing()
    add_executable(quicklogger_tests
        tests/test_main.cpp
        tests/test_helpers.cpp
        tests/test_utilities.cpp
        tests/test_zip_extract.cpp
        tests/test_database.cpp
        tests/test_station_data.cpp
        tests/test_net_slice.cpp
        tests/test_app_state.cpp
    )
    target_link_libraries(quicklogger_tests PRIVATE quicklogger_core)
    add_test(NAME quicklogger_tests COMMAND quicklogger_tests)
endif()
