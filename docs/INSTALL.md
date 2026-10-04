# Installing QuickLogger

Downloads, what each platform needs to run it, first launch, and the files it creates. To run a shared net that others connect to, continue with the [Shared server](SHARED_SERVER.md) guide. For how much disk and memory it needs, see [Sizing](SIZING.md).

## Supported platforms and how well verified

| Platform | Support | How well verified |
|---|---|---|
| **macOS** | Full (console + SSH server) | Built and run natively (Apple Silicon) |
| **Linux** (glibc and musl) | Full (console + SSH server); no ZMODEM on Alpine | Built and the full test suite run on every change, following [Building QuickLogger](BUILDING.md): Ubuntu 22.04 and 24.04, Debian 13, Fedora and Alpine (musl), including CMake 3.16. The amd64 and arm64 release binaries have both been run on Debian (amd64 under WSL on Windows), with basic functionality confirmed |
| **FreeBSD** 14 / 15 | Full (console + SSH server) | Built and the full test suite run in FreeBSD 15 for every release (amd64 and arm64), and built on a FreeBSD machine by following [Building QuickLogger](BUILDING.md). The amd64 and arm64 release binaries have both been run on FreeBSD, with basic functionality confirmed |
| **Windows** | Console only — no SSH server, no ZMODEM | Built and the full test suite run for every release (x64 and arm64), and with both Visual Studio + vcpkg and MSYS2 (UCRT64) on demand, following [Building QuickLogger](BUILDING.md). The x64 and arm64 release binaries have both been run on Windows, with basic functionality confirmed |

## Download (macOS)

Each [release](https://github.com/W4KWK-Projects/QuickLogger/releases) has a ready-to-run macOS binary for Apple Silicon Macs, built for macOS 13 or later: `QuickLogger-<version>-macos-arm64.tar.gz`. For Linux, FreeBSD and Windows, see below; on other platforms, [build it](BUILDING.md).

1. Install its one outside library: `brew install libssh`.
2. Unpack it: `tar xzf QuickLogger-<version>-macos-arm64.tar.gz`.
3. The binary isn't signed by an Apple-registered developer, so macOS blocks a copy downloaded with a web browser. Clear that once with `xattr -d com.apple.quarantine QuickLogger-<version>-macos-arm64/QuickLogger` (a copy fetched with `curl` or `gh release download` isn't affected).
4. Move `QuickLogger` into the directory you want its data in, and start it there (see [Starting it](#starting-it)).

Each release also has a `.sha256` file for checking the download: `shasum -a 256 -c QuickLogger-<version>-macos-arm64.tar.gz.sha256`.

Releases also have FreeBSD 15 binaries for 64-bit PCs (`QuickLogger-<version>-freebsd-amd64.tar.gz`) and 64-bit ARM (`QuickLogger-<version>-freebsd-arm64.tar.gz`), built and tested on FreeBSD. They need `sudo pkg install libssh curl sqlite3`. To run it as an always-on server that installs each new release by itself, see [deploy/freebsd](../deploy/freebsd/README.md).

## Download (Linux)

Releases have Linux binaries for 64-bit PCs (`QuickLogger-<version>-linux-amd64.tar.gz`) and 64-bit ARM, such as a Raspberry Pi running a 64-bit OS (`QuickLogger-<version>-linux-arm64.tar.gz`). They're built on Ubuntu 22.04, run on it and anything newer, and are checked on Debian 12 and 13, Ubuntu 24.04 and Fedora before each release. Alpine and other musl-based systems need to [build it](BUILDING.md).

1. Install the libraries it uses:
   - Debian 12: `sudo apt install libssh-4 libcurl4 libsqlite3-0`
   - Debian 13, Ubuntu 24.04 and newer: `sudo apt install libssh-4 libcurl4t64 libsqlite3-0`
   - Ubuntu 22.04: `sudo apt install libssh-4 libcurl4 libsqlite3-0`
   - Fedora: `sudo dnf install libssh libcurl sqlite-libs`
2. Unpack it: `tar xzf QuickLogger-<version>-linux-amd64.tar.gz` (or `-arm64`).
3. Move `QuickLogger` into the directory you want its data in, and start it there (see [Starting it](#starting-it)).

On Windows under WSL, use a Debian or Ubuntu distribution, and keep QuickLogger in your Linux home directory, not a Windows folder under `/mnt/c/` (its database can't work there).

Check the download with `sha256sum -c QuickLogger-<version>-linux-amd64.tar.gz.sha256`.

To run it on a Debian or Ubuntu server as an always-on SSH server that installs each new release by itself, see [deploy/linux](../deploy/linux/README.md).

## Download (Windows)

Releases have Windows binaries for 64-bit PCs (`QuickLogger-<version>-windows-x64.zip`) and 64-bit ARM, such as Windows in a VM on an Apple Silicon Mac or a Snapdragon laptop (`QuickLogger-<version>-windows-arm64.zip`). Each is a single `QuickLogger.exe` with nothing else to install. It's console only (see [Running on Windows](#running-on-windows)).

1. Unzip it.
2. Move `QuickLogger.exe` into the folder you want its data in, and start it there from Windows Terminal (see [Starting it](#starting-it)).
3. The program isn't signed, so Windows SmartScreen may stop it the first time. Choose **More info**, then **Run anyway**.

Check the download in PowerShell with `Get-FileHash QuickLogger-<version>-windows-x64.zip`, and compare the result with the `.sha256` file.

## What you need to run it

On every platform:

- **A terminal that handles UTF-8 and colors** — any modern terminal emulator. (On Windows: Windows Terminal, or the console in Windows 10 or later.) QuickLogger takes over the whole terminal window.
- **Internet access** for the first launch, which downloads the FCC's amateur license database (about 200 MB) and its GMRS one, Canada's amateur one (ISED's, about 2 MB), some Census ZIP-code and county files (about 30 MB, once) and a small file of Canadian postal-code locations (about 40 KB, once), and refreshes the FCC and ISED data about weekly. The FCC files come from a weekly copy in this repository's [`fcc-data` release](https://github.com/W4KWK-Projects/QuickLogger/releases/tag/fcc-data), since fcc.gov refuses downloads from some cloud servers, and straight from the FCC if that copy can't be had. Without it the rest of QuickLogger still works, but callsign, ZIP and county lookups have no data to draw on.
- **Write access to the directory you launch it from** — it keeps its data there (see [Files it creates](#files-it-creates)).

The runtime libraries QuickLogger is linked against, by platform (installing the [build packages](BUILDING.md#what-you-need-to-build-it) instead also covers these):

| Platform | Install |
|---|---|
| **macOS** | `brew install libssh` (SQLite, curl and zlib are part of macOS) |
| **Debian / Ubuntu** | `sudo apt install libssh-4 libsqlite3-0 libcurl4` — on Ubuntu 24.04+ and Debian 13 the last one is named `libcurl4t64` |
| **Fedora** | `sudo dnf install libssh libcurl sqlite-libs` |
| **FreeBSD** | `sudo pkg install libssh curl sqlite3` (zlib is part of the base system) |
| **Windows** | Nothing to install for the release download. A build of your own with vcpkg needs the DLLs it depends on (`sqlite3`, `libcurl`, `zlib` — vcpkg copies them next to the `.exe`) to stay alongside `QuickLogger.exe` |

(SQLite 3.24 or newer is required. Package names checked against the Ubuntu 22.04/24.04, Debian 13, Fedora 43 and FreeBSD 14/15 package lists, September 2026.)

QuickLogger does **not** need `unzip`, `mkdir`, or any other command-line tool to do its normal work — archive extraction and file handling are done inside the program.

### Optional: `lrzsz` (ZMODEM file transfer)

The net-log/database-slice export and import features can push and pull files over the terminal connection using the ZMODEM protocol, via the `sz`/`rz` command-line tools (the `lrzsz` package). It's needed only on the machine running QuickLogger, only for that one feature, and only where someone is connected through a ZMODEM-capable terminal (typically over SSH). If it's missing, QuickLogger still runs fine; it just skips the ZMODEM offer and tells you so. SSH users can copy files with `scp` or `sftp` instead, with or without it (see the [User Guide](USER_GUIDE.md)).

- macOS: `brew install lrzsz`
- Debian/Ubuntu: `sudo apt install lrzsz`
- Fedora: `sudo dnf install lrzsz`
- FreeBSD: `sudo pkg install lrzsz` (it installs them as `lsz`/`lrz`, which QuickLogger finds too)
- Alpine: not supported (Alpine has no `lrzsz` package). Everything else works, SSH included; exports are saved in `exports/` and imports are read from `imports/`, but not sent or received over ZMODEM. SSH users can use `scp` or `sftp` instead.
- Windows: not supported

### Optional: `mosh` (Mosh connections)

With `mosh-server` installed where QuickLogger runs, SSH users can connect with [Mosh](https://mosh.org) as well as `ssh` (see [Mosh](CONNECTING.md#mosh)). Without it, Mosh just isn't offered.

- macOS: `brew install mosh`
- Debian/Ubuntu: `sudo apt install mosh`
- Fedora: `sudo dnf install mosh`
- FreeBSD: `sudo pkg install mosh`
- Alpine: `sudo apk add mosh`
- Windows: not supported (no SSH server)

## Starting it

```
./QuickLogger
```

(On Windows: `QuickLogger.exe`.)

On first run, you'll be taken straight to Settings to set your call sign (amateur, GMRS or both) and home ZIP code. After that, you land on the Recurring Nets list, and **F1** on any page explains its keys.

**How to use it** — running a net, logging check-ins, ad hoc nets, autocomplete, saved stations, History, exports and the rest — is in the **[User Guide](USER_GUIDE.md)**.

### Files it creates

QuickLogger creates and uses these files/directories, all as siblings of wherever you launch it from (not tied to your current shell's directory beyond that):

- `quicklogger.db` — the shared SQLite database (nets, stations, check-in history, the SSH user roster — everything except personal settings)
- `settings.txt` — your own call signs and home ZIP (local console session only; never included in any export)
- `settings/` — one settings file per SSH login user (see [Shared server](SHARED_SERVER.md))
- `exports/`, `imports/` — where "download"/"upload" style features (net-slice export/import, ZMODEM, SFTP) read and write files. The local console's files go directly in them and are kept for good. Each SSH user's go in their own `ssh-users/<username>/` folder inside them, where only that user sees them, and are deleted after 7 days; for an SSH user they're only a stop on the way to or from their own computer.
- `uls_cache/` — downloaded FCC, ISED and Census files (see [Station data](USER_GUIDE.md#station-data)); safe to delete while QuickLogger isn't running
- `ssh_host_ed25519_key` — the SSH server's host key (see [Host key](SHARED_SERVER.md#host-key))

## Running on Windows

The Windows build is a **console-only** program: everything works in a local terminal window, but there is no built-in SSH server (it depends on `fork()` and pseudo-terminals, which Windows doesn't have), no ZMODEM, and `--headless`, `--ssh-port` and `--no-ssh` don't apply. To host a shared instance that other operators SSH into, run QuickLogger on a macOS, Linux or FreeBSD machine — or, on a Windows machine, in WSL2, where the Linux build behaves like any other Linux system (see [Download (Linux)](#download-linux)).
