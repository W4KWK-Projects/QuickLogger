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

### Running a net

On **Recurring Nets**, highlight a net and press **F3** (or Enter), pick
your role, confirm your callsign, and the net starts with you logged as
check-in #1. **F2** opens the New Station window:

| Key | What it does |
|---|---|
| F2 | Log this station and clear the form for the next one |
| F3 | Log this station and close the window |
| Esc | Close the window without logging anything |

**F4** closes the net when you're done. It asks first, since a closed
session can't be reopened for logging; it stays in **History** (F6), where
you can still view and export it.

**Picking up where you left off.** If a session ends without F4 — an SSH
connection drops, a terminal is closed — the session stays open, and the
net shows *session open* in the list. Starting that net again offers to
**Resume** it (F2/Enter), carrying on the same log, or to **close it and
start a new session** (F3). The same applies if someone else is logging
that net right now: resuming joins their log.

**Telling nets apart.** The list shows when each net was created, or, for
one brought in with **F9 (Import Net)**, when it was imported — so if you
import a net with the same name as one of yours, you can tell which is
which and delete the one you don't want. (Nets created before this was
recorded show no date.)

### Editing and deleting by number

Wherever a list has an edit or delete key, pressing it numbers every row.
Type the row's number and press **Enter**. An edit opens that row straight
away. A delete (or remove) first shows what it's about to delete and asks
you to confirm with **F2/Enter**, or **Esc** to back out. While you're
choosing, **Up/Down** move the highlight instead (Enter with no number
typed picks the highlighted row), **Backspace** erases a digit, and **Esc**
cancels. Check-ins already show their number in the **#** column, so that's
the number to type for them.

| Page | Key | What it does |
|---|---|---|
| Recurring Nets | F7 | Edit a net |
| Active net (while logging) | F3 / F5 | Edit / delete a check-in |
| Edit Net | F9 | Load a saved station into the form below |
| Edit Net | F4 | Remove a saved station from this net |
| History | F4 | Delete one check-in from the highlighted session's log |
| History | F5 | Delete a closed net session and its log |
| Manage Users | F3 | Remove an SSH user |

On **History**, the top list is the net's past sessions and the bottom list
is the check-ins of whichever session is highlighted. To delete one check-in
from a past log, first highlight its session with **Up/Down**. Then press
**F4**; the bottom list's **#** column holds the numbers to type. The
confirmation names the station and the session's date. If that check-in
held the session's Net Control, Alternate NC or Logger role, the role is
cleared from the session too.

Deleting a whole recurring net is still **F8** on its Edit Net page, which
asks to confirm first.

**What happens to a station's details.** A station's details (name, member
ID, address, county, and so on) are kept for as long as something uses
them: a net it's saved to, or a check-in in some net's log. When the last
of those goes — you remove it from the only net it was saved to and it has
never checked in, or you delete its last check-in — its details are deleted
too, and it stops coming up in autocomplete. That's also how a mistyped
callsign gets cleaned up: remove it and it's gone. The confirmation tells
you beforehand which of the two will happen.

### Callsign autocomplete

Wherever you enter a station's callsign — the New Station window while
logging a net (F2 on the active net), and the station form on a net's Edit
Net page — matches appear below the field as you type. You can type any part
of the callsign, in upper or lower case: `kwk` finds W4KWK. Matches are
listed in this order:

1. Stations known to **this net** (they've checked in before, or are saved
   to it), marked *(this net)*.
2. Stations known to **other nets**, marked *(other net)*.
3. **Licensed stations near you** from the FCC data (within about 70 miles
   of the home ZIP in Settings), marked *(ULS, ~N mi)*.

Up to 8 matches show at once; the FCC ones fill whatever room the first two
groups leave. The match marked **>** is the one Enter picks. Press
**Up/Down** to move the marker. You stay in the Callsign field, so you can
keep typing to narrow the list. Picking a match fills in the rest of the
station's details (name, address, county, and so on).

If nothing matches, just type the whole callsign. Pressing Enter then looks
it up exactly, including in the FCC data at any distance.

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

### How much disk space and memory it needs

The numbers below come from real runs: the actual FCC and Census data
(September 2026), plus simulated net history for two example
installations, measured after SQLite `VACUUM`. They're for planning only.
Real sizes depend mostly on how much text goes into each check-in's
remarks and comments.

**Two example installations**

| | A: small club | B: large group |
|---|---|---|
| Users (logins) | 10 | 50 |
| Nets | 5, all weekly | 15: 10 weekly, 5 daily |
| Check-ins per net | 25 | 50 |
| Net sessions per year | 260 | 2,345 |
| Check-ins per year | 6,500 | 117,250 |

**Size of `quicklogger.db`**

| | A | B |
|---|---|---|
| New install (station data only) | 98 MB | 98 MB |
| After 1 year | about 105 MB | about 110 MB |
| After 5 years | about 125 MB | about 155 MB |
| After 10 years | about 150 MB | about 215 MB |

The number of users hardly matters. Each login adds a settings file of
about 50 bytes and one small row. What grows the database is:

- **Check-in history**, about 60 bytes per check-in including its net
  session: roughly 0.4 MB a year for A and 7 MB a year for B.
- **The FCC station data**, about 95 MB of the starting size. It grows as
  new callsigns are issued, because licenses that lapse are kept rather than
  removed. That's an estimate of about 5 MB a year, and it's most of A's
  growth. A new-license rate at the high end would put 10 years at up to
  about 185 MB for A and 250 MB for B.

**Everything else on disk**

| | Size |
|---|---|
| `uls_cache/` (downloaded FCC and Census files, kept between refreshes) | about 760 MB, not growing |
| `quicklogger.db-wal` (SQLite's write log) | a few MB; up to about 15 MB during the first data load |
| The QuickLogger program | about 1–2 MB |
| `exports/` | a few KB per exported log; only grows if you export |

So even installation B needs only about **1 GB after 10 years**, and most of
that is `uls_cache/`. You can delete `uls_cache/` while QuickLogger isn't
running to get that space back. It fills up again at the next weekly
refresh.

**Memory.** Each session, local or over SSH, uses about 10 MB. The
station-data updater idles at about 2 MB. It imports the FCC data (at first
launch, then for under a minute once a week) in a separate short-lived
process that peaks at about 275 MB and returns all of it to the system when
it finishes. (On Windows the import runs inside the QuickLogger process, so
that memory stays in use until QuickLogger exits.)

**Recommendation.**

- **Disk:** anything with 2 GB free is plenty for either example for a
  decade. An SSD or a good-quality SD card is fine; the weekly refresh
  writes a few hundred MB.
- **Memory:** 1 GB of RAM covers the weekly import plus several simultaneous
  SSH users, so hardware in the Raspberry Pi 3 class (1 GB) or better should
  handle installation B. QuickLogger hasn't been run on a Pi yet.
- **Backups:** back up `quicklogger.db`. It's the only file that can't be
  downloaded again. Copy it while QuickLogger isn't running, or use SQLite's
  `.backup` command while it is.

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
| `-DQUICKLOGGER_BUILD_TESTS=OFF` | `ON` | Skip building the test suite (see below). |

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

### Running the tests

The build also produces `quicklogger_tests`, which checks the database,
logging, numbered edit/delete, autocomplete, county lookup, the station-data
refresh, ZIP extraction and net import/export. Each test works in its own
temporary directory and never touches your `quicklogger.db` or the network
(the FCC and Census downloads are stood in for by small local files).

```
ctest --test-dir build --output-on-failure
```

Or run `build/quicklogger_tests` directly; give it part of a test's name
(e.g. `build/quicklogger_tests Autocomplete`) to run only the matching tests.

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

The key is checked when you add it. A private key, a PuTTY-format key, a
line missing its `ssh-ed25519` (or other type) at the start, or one cut
short while copying is refused with a message saying what's wrong and what
the line should look like. Extra spaces or a trailing line break from the
paste are tidied up. If someone still can't log in, check they're offering
the key you added (see *If you have more than one key* below).

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
there's no console session and the main process is the listener). While
station data is being refreshed there's briefly one more, the refresh
itself. That's intentional, not strays: a process that has the database
open can't safely fork off others (SQLite doesn't allow it), and the updater
has to outlive any one session. The helpers exit on their own when the
process that started them quits or dies.

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
