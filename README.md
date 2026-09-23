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

Launched normally, QuickLogger starts its SSH listener as a small separate
helper process alongside the console session — you'll see two `QuickLogger`
entries in `ps`. That's intentional (a process that has the database open
can't safely fork off SSH sessions; SQLite doesn't allow it), not a stray.
The helper exits on its own when the console session quits or dies. With
`--headless` there's no console session, so a single process does both jobs.

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
