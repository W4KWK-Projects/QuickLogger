# QuickLogger

A terminal (TUI) net-logging application for ham radio operators, with a
built-in SSH server so a group of operators can log into a shared,
always-running instance directly — no separate "SSH into the host, then
launch the app" step, and no OS user accounts to provision per operator.

This README has two halves: **[Running QuickLogger](#running-quicklogger)**
(what a machine needs in order to run it) and
**[Building QuickLogger](#building-quicklogger)** (what it takes to compile
it), each broken out by platform. The [built-in SSH
server](#built-in-ssh-server) is documented after both.

## Supported platforms

| Platform | Support | How well verified |
|---|---|---|
| **macOS** | Full (console + SSH server) | Built and run natively (Apple Silicon) |
| **Linux** (glibc and musl) | Full (console + SSH server) | Every source file compiles cleanly against real Ubuntu 22.04 and 24.04 headers/libstdc++ (libssh 0.9 and 0.10) and against musl; not yet linked or run on a Linux machine |
| **FreeBSD** 14 / 15 | Full (console + SSH server) | Every source file compiles cleanly against the real FreeBSD 14.4 and 15.1 base-system headers; not yet linked or run on a FreeBSD machine |
| **Windows** | Console only — no SSH server, no ZMODEM | Every source file compiles cleanly with mingw-w64; not yet linked or run; MSVC not tried |

"Compiles cleanly" means the source was compiled (not linked) for that
target with a Clang-based cross-compiler. The libraries QuickLogger depends
on are standard and available on all of these systems, but until it has been
built and run on a given platform, treat that platform as new — bug reports
welcome.

---

# Running QuickLogger

## What you need to run it

On every platform:

- **A terminal that handles UTF-8 and colors** — any modern terminal
  emulator. (On Windows: Windows Terminal, or the console in Windows 10 or
  later.) QuickLogger takes over the whole terminal window.
- **Internet access** for the first launch, which downloads the FCC's amateur
  license database (about 200 MB) and some Census ZIP-code and county files
  (about 30 MB, once), and refreshes the FCC data about weekly. Without it
  the rest of QuickLogger still works, but callsign, ZIP and county lookups
  have no data to draw on.
- **Write access to the directory you launch it from** — it keeps its data
  there (see [Files it creates](#files-it-creates)).

The runtime libraries QuickLogger is linked against, by platform (installing
the [build packages](#what-you-need-to-build-it) instead also covers these):

| Platform | Install |
|---|---|
| **macOS** | `brew install libssh` (SQLite, curl and zlib are part of macOS) |
| **Debian / Ubuntu** | `sudo apt install libssh-4 libsqlite3-0 libcurl4` — on Ubuntu 24.04+ and Debian 13 the last one is named `libcurl4t64` |
| **Fedora** | `sudo dnf install libssh libcurl sqlite-libs` |
| **FreeBSD** | `sudo pkg install libssh curl sqlite3` (zlib is part of the base system) |
| **Windows** | Nothing to install, as long as the DLLs the build depends on (`sqlite3`, `libcurl`, `zlib` — vcpkg copies them next to the `.exe`) stay alongside `QuickLogger.exe` |

(SQLite 3.24 or newer is required. Package names checked against the Ubuntu
22.04/24.04, Debian 13, Fedora 43 and FreeBSD 14/15 package lists, September
2026.)

QuickLogger does **not** need `unzip`, `mkdir`, or any other command-line
tool to do its normal work — archive extraction and file handling are done
inside the program.

### Optional: `lrzsz` (ZMODEM file transfer)

The net-log/database-slice export and import features can push and pull
files over the terminal connection using the ZMODEM protocol, via the
`sz`/`rz` command-line tools (the `lrzsz` package). It's needed on `PATH`
only on the machine running QuickLogger, only for that one feature, and only
where someone is connected through a ZMODEM-capable terminal (typically over
SSH). If it's missing, QuickLogger still runs fine; it just skips the ZMODEM
offer and tells you so.

- macOS: `brew install lrzsz`
- Debian/Ubuntu: `sudo apt install lrzsz`
- Fedora: `sudo dnf install lrzsz`
- FreeBSD: `sudo pkg install lrzsz`
- Windows: not supported

## Starting it

```
./QuickLogger
```

(On Windows: `QuickLogger.exe`.)

On first run, you'll be taken straight to Settings to set your callsign
and home ZIP code (required before anything else is usable). After that,
you land on the Recurring Nets list.

### Saving stations to a net

On a net's **Edit Net** page (F7 from the Recurring Nets list), the station
form sits below the net's own details and its list of saved stations. You
don't have to Tab down to it: press **F3** (Save Station) while the form's
callsign is empty and the cursor jumps straight to the Callsign field (**F6**,
Add Station, does the same and also clears the form). Type a callsign, pick
a match, fill in anything else, and press **F3** again to save it — the
cursor returns to Callsign, ready for the next station.

### Station data

QuickLogger looks callsigns up in its own copy of the FCC's amateur license
database, and fills in each station's county from Census data. It downloads
and refreshes all of this by itself, in the background, for everyone using
the instance — nobody needs to (or can) start it by hand:

- The first download starts as soon as QuickLogger launches and takes a
  minute or two. Until it's done, a yellow **Loading station data NN%**
  notice shows at the top of every screen, local or over SSH, and callsign
  lookups won't find anyone yet. The rest of the app works meanwhile.
- After that, the FCC data is refreshed about weekly (a yellow **Updating
  station data** notice shows while it runs; lookups keep working from the
  previous copy). A failed download is retried automatically every hour.
- **Settings** shows when the data was last updated. At the local console
  only, **F3** there refreshes it right away.
- Quitting the console session in the middle of a download is fine: an
  unfinished first download or weekly refresh starts over the next time
  QuickLogger runs. (A refresh asked for with F3 just waits for its regular
  turn.)

**How county is worked out.** FCC records have no county, so QuickLogger
uses the station's ZIP code. Most ZIPs lie in one county. For the few
thousand that cross a county line, the station's city decides when it names
a town inside that ZIP (a Newton address in 02467 is Middlesex, a Boston one
Suffolk); otherwise the ZIP counts as being in whichever county most of its
residents live in.

### Files it creates

QuickLogger creates and uses these files/directories, all as siblings of
wherever you launch it from (not tied to your current shell's directory
beyond that):

- `quicklogger.db` — the shared SQLite database (nets, stations, check-in
  history, the SSH user roster — everything except personal settings)
- `settings.txt` — your own callsign and home ZIP (local console session
  only; never included in any export)
- `settings/` — one settings file per SSH login user (see below)
- `exports/`, `imports/` — where "download"/"upload" style features
  (net-slice export/import, ZMODEM) read and write files
- `uls_cache/` — downloaded FCC and Census files (see
  [Station data](#station-data)); safe to delete while QuickLogger isn't
  running
- `ssh_host_ed25519_key` — the SSH server's host key (see below)

### Running on Windows

The Windows build is a **console-only** program: everything works in a
local terminal window, but there is no built-in SSH server (it depends on
`fork()` and pseudo-terminals, which Windows doesn't have), no ZMODEM, and
`--headless`, `--ssh-port` and `--no-ssh` don't apply. To host a shared
instance that other operators SSH into, run QuickLogger on a macOS, Linux or
FreeBSD machine — or, on a Windows machine, in WSL2, where the Linux build
should behave like any other Linux system.

---

# Building QuickLogger

## What you need to build it

- **A C++17 compiler** with `std::filesystem` — GCC 9+, Clang 9+ or MSVC
  2019+. (Verified with Clang; GCC hasn't been tried.)
- **CMake 3.16 or newer.**
- **git, and network access on the first build** — CMake downloads and builds
  FTXUI (the terminal UI library) itself, so there's nothing to install for
  it. See [Building offline](#building-offline) if you can't.
- **Development files** (headers + libraries) for:
  - **SQLite 3** (3.24+)
  - **libcurl**
  - **zlib**
  - **libssh** — only for builds with the SSH server (everything but
    Windows). This is `libssh`, not `libssh2`; they're unrelated projects and
    only `libssh` has server support.

CMake will stop with a clear "not found" error naming whichever of these is
missing.

### macOS

Install the Xcode command line tools (`xcode-select --install`), which
provide the compiler and git, then:

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

If CMake can't find libssh even though it's installed, point it at the
install with `-Dlibssh_DIR=<directory containing libssh-config.cmake>`; a
plain `libssh.pc` (pkg-config) is used as a fallback if the CMake package
is missing.

### Building offline

FTXUI v5.0.0 is downloaded from GitHub the first time you configure. To
build without network access, clone [FTXUI](https://github.com/ArthurSonzogni/FTXUI)
at tag `v5.0.0` somewhere ahead of time and pass its location:

```
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_FTXUI=/path/to/FTXUI
```

---

## Built-in SSH server

QuickLogger can accept SSH connections directly — `ssh <username>@host`
drops you straight into the app, no separate login+launch step. This is
**on by default** on macOS, Linux and FreeBSD. It isn't part of the Windows
build (see [Running on Windows](#running-on-windows)), or of any build made
with `-DQUICKLOGGER_ENABLE_SSH=OFF`; those are console-only.

### First-time setup: adding a user

There's no OS user account involved. Instead, QuickLogger keeps its own
small roster of usernames and public keys, and the only way to manage it
is from the **local console session** (by design — see below). To add
the first SSH user:

1. Run `./QuickLogger` directly at the machine's own console.
2. Go to **Settings** (F4 from the Recurring Nets list).
3. Press **F4 (Manage Users)**.
4. Fill in the two fields at the bottom of the page — **Username** (the
   name they'll type in `ssh <username>@host`; matched exactly, including
   case, so a simple lowercase name is easiest) and **Public Key** (that
   person's whole public-key line, e.g. `ssh-ed25519 AAAA... their-comment`).
   Then press **F2 (Add)**. If they don't have a key yet, see
   [Creating your SSH key](#creating-your-ssh-key) below.

The key isn't checked when you add it — a mangled or truncated paste is
accepted and then simply never lets that person in. If someone can't log
in, remove the entry (F3) and re-add it with a fresh paste.

That person can now connect:

```
ssh -p 2222 <username>@<host>
```

Public-key authentication only — there's no password option.

### Creating your SSH key

Anyone who wants to connect needs an SSH key pair, created on the machine
they'll be connecting **from**. The private half never leaves that machine;
only the public half (the `.pub` file) gets handed to whoever is adding
you in Manage Users.

**1. See whether you already have a key.**

```
ls ~/.ssh/*.pub
```

If that lists `id_ed25519.pub`, skip to step 3. A "No such file or
directory" error just means you've never made one — normal on a new
account, and step 2 fixes it.

**2. Create one.** (Same command on macOS, Linux, and Windows PowerShell —
Windows 10+ ships with the OpenSSH client.)

```
ssh-keygen -t ed25519 -C "your-name-or-callsign"
```

Press Enter to accept the default file location. When it asks for a
passphrase, choose one (recommended) or press Enter twice for none. This
creates `~/.ssh/` if it doesn't exist, plus two files in it:

- `id_ed25519` — your **private** key. Never share it, email it, or paste
  it anywhere.
- `id_ed25519.pub` — your **public** key. This is the one you give out.

**3. Print your public key and send it to the person running
QuickLogger.**

```
cat ~/.ssh/id_ed25519.pub
```

(On Windows PowerShell: `type $env:USERPROFILE\.ssh\id_ed25519.pub`. On
macOS you can copy it straight to the clipboard with
`pbcopy < ~/.ssh/id_ed25519.pub`.)

The output is a single line that looks like this:

```
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI... your-name-or-callsign
```

That whole line — starting with `ssh-ed25519`, on one line with no line
breaks — is what gets pasted into Manage Users. It's a public key, so
sending it by email or chat is fine.

**If you have more than one key**, tell `ssh` which one to offer so it
doesn't pick the wrong one:

```
ssh -i ~/.ssh/id_ed25519 -p 2222 <username>@<host>
```

### Slow or metered connections: turn on compression

QuickLogger redraws the whole screen whenever anything changes — every
keypress, and once a minute for the clock in the top bar. Uncompressed, one
redraw is about 3 KB on an 80×24 terminal (most of it blank space and border
characters), which is fine on a LAN but adds up over a slow, metered or
radio-linked connection.

Ask your SSH client to compress the session and that drops to roughly 0.2 KB
per redraw — over 90% less. No server setup is needed. It's off by default in
OpenSSH, so opt in with `-C`:

```
ssh -C -p 2222 <username>@<host>
```

or once and for all in `~/.ssh/config`:

```
Host quicklogger
    HostName <host>
    Port 2222
    User <username>
    Compression yes
```

after which `ssh quicklogger` is all you type. Most other SSH clients have an
equivalent "enable compression" setting.

### Why Manage Users is console-only

Adding/removing SSH users is deliberately only reachable from whoever is
physically at the machine's own console — never over SSH, regardless of
whose key the SSH session authenticated with. This isn't a missing
feature; it's a deliberate simplification: it means there's no
admin/permission system to build, and no way for a compromised or
malicious SSH session to ever add itself another account, no matter what.
The trade-off is that adding a new operator always needs someone at the
console in that moment.

### Command-line flags

- `--ssh-port=N` — listen on port `N` instead of the default **2222**.
  QuickLogger does not default to port 22, since that's almost always
  already the machine's real system `sshd`. If you specifically want
  QuickLogger on port 22 instead (replacing the system's own sshd, or on
  a machine with no other sshd), you'll need to run it with permission to
  bind that privileged port (root, or `setcap cap_net_bind_service` on
  Linux) — pass `--ssh-port=22` once you've arranged that.
- `--no-ssh` — disable the SSH listener entirely; local console use only.
- `--headless` — skip the local console session entirely and just run the
  SSH listener, for an unattended server with no one sitting at it. (Note:
  since Manage Users is console-only, you'll need to add users *before*
  switching to headless-only operation, or run a normal console session
  briefly whenever a new user needs adding.)

### How it runs

Launched normally, QuickLogger starts two small helper processes alongside
the console session — the SSH listener and the station-data updater — so
you'll see three `QuickLogger` entries in `ps` (two with `--headless`, where
there's no console session and the main process is the listener). That's
intentional, not strays: a process that has the database open can't safely
fork off others (SQLite doesn't allow it), and the updater has to outlive any
one session. Both helpers exit on their own when the process that started
them quits or dies.

### Firewall / networking

Whatever port QuickLogger's SSH listener uses needs to actually be
reachable — opening it in your OS's firewall, and/or forwarding it
through a router/NAT if you're connecting from outside the local network,
is on you to set up. CMake and the app itself have no way to do this for
you.

### Host key

A dedicated ed25519 host key (`ssh_host_ed25519_key`, separate from the
machine's own real SSH host keys) is generated automatically the first
time the SSH listener starts, and reused after that. It's `chmod 600`'d
and gitignored — treat it like any other private key. Delete it (while
QuickLogger isn't running) to force a fresh one to be generated on next
launch; clients that connected before will see a "host key changed"
warning afterward, same as with any SSH server.
