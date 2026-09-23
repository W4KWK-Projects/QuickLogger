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

add_executable(QuickLogger
    src/main.cpp
    src/date_utils.cpp
    src/settings.cpp
    src/text_utils.cpp
    src/geo_utils.cpp
    src/file_export.cpp
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
)
