# Building QuickLogger

Compiling QuickLogger from source. If you just want to run it, see [Installing QuickLogger](INSTALL.md).

## What you need to build it

- **A C++17 compiler** with `std::filesystem` — GCC 9+, Clang 9+ or MSVC 2019+. (Clang, GCC and MSVC are all built with and tested.)
- **CMake 3.16 or newer.**
- **git, and network access on the first build** — CMake downloads and builds FTXUI (the terminal UI library) itself, so there's nothing to install for it. See [Building offline](#building-offline) if you can't.
- **Development files** (headers + libraries) for:
  - **SQLite 3** (3.24+)
  - **libcurl**
  - **zlib**
  - **libssh** — only for builds with the SSH server (everything but Windows). This is `libssh`, not `libssh2`; they're unrelated projects and only `libssh` has server support.

CMake will stop with a clear "not found" error naming whichever of these is missing.

### macOS

Install the Xcode command line tools (`xcode-select --install`), which provide the compiler and git, then:

```
brew install cmake libssh
```

SQLite, libcurl and zlib come with macOS, so there's nothing else to install.

### Debian / Ubuntu

```
sudo apt install build-essential cmake git libsqlite3-dev libcurl4-openssl-dev zlib1g-dev libssh-dev
```

### Fedora

```
sudo dnf install gcc-c++ cmake git sqlite-devel libcurl-devel zlib-devel libssh-devel
```

### FreeBSD

The compiler (clang) and zlib are part of the base system.

```
sudo pkg install cmake git sqlite3 curl libssh
```

### Windows (console-only build)

Either toolchain works; both need [Git](https://git-scm.com/download/win).

**Visual Studio 2019 or newer, with [vcpkg](https://vcpkg.io):**

```
vcpkg install sqlite3 curl zlib --triplet x64-windows
```

**MSYS2** (UCRT64 shell):

```
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-sqlite3 mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-zlib
```

libssh isn't needed: the SSH server is switched off by default on Windows.

## Build steps

macOS, Linux, FreeBSD (and MSYS2, from its UCRT64 shell):

```
git clone <this repo>
cd QuickLogger
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The resulting binary is `build/QuickLogger`.

Windows with Visual Studio and vcpkg (from a Developer Command Prompt):

```
git clone <this repo>
cd QuickLogger
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The resulting binary is `build\Release\QuickLogger.exe`.

### Build options

| Option | Default | Meaning |
|---|---|---|
| `-DQUICKLOGGER_ENABLE_SSH=OFF` | `ON` (`OFF` on Windows) | Leave out the built-in SSH server, and with it the libssh requirement. The result is a console-only QuickLogger. |
| `-DCMAKE_BUILD_TYPE=Release` | none | Optimized build (recommended). Not used by Visual Studio; pass `--config Release` at build time instead. |
| `-DFETCHCONTENT_SOURCE_DIR_FTXUI=<path>` | unset | Use a local FTXUI checkout instead of downloading one — see below. |
| `-DQUICKLOGGER_BUILD_TESTS=OFF` | `ON` | Skip building the test suite (see below). |

If CMake can't find libssh even though it's installed, point it at the install with `-Dlibssh_DIR=<directory containing libssh-config.cmake>`; a plain `libssh.pc` (pkg-config) is used as a fallback if the CMake package is missing.

### Building offline

FTXUI v5.0.0 is downloaded from GitHub the first time you configure. To build without network access, clone [FTXUI](https://github.com/ArthurSonzogni/FTXUI) at tag `v5.0.0` somewhere ahead of time and pass its location:

```
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_FTXUI=/path/to/FTXUI
```

### Running the tests

The build also produces `quicklogger_tests`, which checks the database, logging, numbered edit/delete, autocomplete, county lookup, the station-data refresh, ZIP extraction and net import/export. Each test works in its own temporary directory and never touches your `quicklogger.db` or the network (the FCC and Census downloads are stood in for by small local files).

```
ctest --test-dir build --output-on-failure
```

Or run `build/quicklogger_tests` directly; give it part of a test's name (e.g. `build/quicklogger_tests Autocomplete`) to run only the matching tests.
