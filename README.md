# QuickLogger

A terminal (TUI) net-logging application for ham radio operators, with a
built-in SSH server so a group of operators can log into a shared,
always-running instance directly — no separate "SSH into the host, then
launch the app" step, and no OS user accounts to provision per operator.

## Build prerequisites

CMake resolves everything it can on its own via `find_package`, but the
libraries themselves have to already be installed on the build machine —
CMake will fail with a clear "not found" error naming whichever one is
missing. You'll need:

- A C++23 compiler (a recent clang or gcc)
- CMake 3.16+
- SQLite3 (dev headers + library)
- libcurl (dev headers + library)
- **libssh** (dev headers + library) — this is `libssh`, not `libssh2`;
  they're unrelated projects and only `libssh` has server support, which
  this app needs

FTXUI (the terminal UI library) is fetched and built automatically by
CMake (`FetchContent`) — nothing to install for that one.

### macOS (Homebrew)

```
brew install cmake sqlite3 curl libssh
```

### Debian / Ubuntu

```
sudo apt install build-essential cmake libsqlite3-dev libcurl4-openssl-dev libssh-dev
```

### Fedora

```
sudo dnf install cmake sqlite-devel libcurl-devel libssh-devel
```

### FreeBSD

```
pkg install cmake sqlite3 curl libssh
```

(Package names above were confirmed against each platform's real package
repository at the time this was written — if a name has since changed,
your package manager's search will find the current one.)

### Runtime-only prerequisite (optional): `lrzsz`

The net-log/database-slice export and import features can push and pull
files over the terminal connection using the ZMODEM protocol, via the
`sz`/`rz` command-line tools (the `lrzsz` package). This is **not** a
build-time dependency — it's only needed on `PATH` at runtime, and only
for that one feature. If it's missing, QuickLogger still runs fine; it
just skips the ZMODEM offer and tells you so.

- macOS: `brew install lrzsz`
- Debian/Ubuntu: `sudo apt install lrzsz`
- Fedora: `sudo dnf install lrzsz`
- FreeBSD: `pkg install lrzsz`

## Building

```
git clone <this repo>
cd QuickLogger
cmake -S . -B build
cmake --build build -j
```

The resulting binary is `build/QuickLogger`.

## Running locally

```
./QuickLogger
```

On first run, you'll be taken straight to Settings to set your callsign
and home ZIP code (required before anything else is usable). After that,
you land on the Recurring Nets list.

QuickLogger creates and uses these files/directories, all as siblings of
wherever you launch it from (not tied to your current shell's directory
beyond that):

- `quicklogger.db` — the shared SQLite database (nets, stations, check-in
  history, the SSH user roster — everything except personal settings)
- `settings.txt` — your own callsign/ZIP/QRZ credentials (local console
  session only; never included in any export)
- `settings/` — one settings file per SSH login user (see below)
- `exports/`, `imports/` — where "download"/"upload" style features
  (net-slice export/import, ZMODEM) read and write files
- `uls_cache/` — a cache for the FCC ULS station-database import
- `ssh_host_ed25519_key` — the SSH server's host key (see below)

## Built-in SSH server

QuickLogger can accept SSH connections directly — `ssh <username>@host`
drops you straight into the app, no separate login+launch step. This is
**on by default**.

### First-time setup: adding a user

There's no OS user account involved. Instead, QuickLogger keeps its own
small roster of usernames and public keys, and the only way to manage it
is from the **local console session** (by design — see below). To add
the first SSH user:

1. Run `./QuickLogger` directly at the machine's own console.
2. Go to **Settings** (F4 from the Recurring Nets list).
3. Press **F4 (Manage Users)**.
4. Press **F2 (Add)**, type a username, then paste that person's public
   key as a single line — the exact format already in their own
   `~/.ssh/id_ed25519.pub` (e.g. `ssh-ed25519 AAAA... their-comment`).

That person can now connect:

```
ssh -p 2222 <username>@<host>
```

Public-key authentication only — there's no password option.

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

### A known limitation

Roughly 30-60% of individual SSH connection *attempts* can intermittently
fail during authentication (the client sees "connection closed"; nothing
crashes, no data is affected). If a connection is refused, just try
again — a retry has a good chance of succeeding, and once a connection
authenticates, the session itself is fully reliable. This is a real,
currently-unresolved issue in the connection-acceptance path, not
something you're doing wrong.
