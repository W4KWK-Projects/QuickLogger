# Shared server: the built-in SSH server

QuickLogger can accept SSH connections directly — `ssh <username>@host` drops you straight into the app, no separate login+launch step. This is **on by default** on macOS, Linux and FreeBSD. It isn't part of the Windows build (see [Running on Windows](INSTALL.md#running-on-windows)), or of any build made with `-DQUICKLOGGER_ENABLE_SSH=OFF`; those are console-only.

## First-time setup: adding a user

There's no OS user account involved. Instead, QuickLogger keeps its own small roster of usernames and public keys, and the only way to manage it is from the **local console session** (by design — see [Why Manage Users is console-only](#why-manage-users-is-console-only)). To add the first SSH user:

1. Run `./QuickLogger` directly at the machine's own console.
2. Go to **Settings** (F4 from the Recurring Nets list).
3. Press **F4 (Manage Users)**.
4. Fill in the fields at the bottom of the page — **Username** (what they type in `ssh <username>@host`, in either case: 1 to 32 letters, digits, dots, hyphens and underscores, starting with a letter or digit), **Amateur Call** and **GMRS Call** (that person's call signs, at least one of them; an amateur one is US or Canadian, without /M, /P or the like), **Public Key** (that person's whole public-key line, e.g. `ssh-ed25519 AAAA... their-comment`) and **Access** (see [View-only users](#view-only-users)). Then press **F2 (Add)**. Their call signs are the ones they log under: their Settings page shows them, and they can't change them. Without a GMRS call sign they can only watch GMRS nets, and likewise for amateur nets. If they don't have a key yet, see [Creating your SSH key](CONNECTING.md#creating-your-ssh-key) below.

The key is checked when you add it. A private key, a PuTTY-format key, a line missing its `ssh-ed25519` (or other type) at the start, or one cut short while copying is refused with a message saying what's wrong and what the line should look like. Extra spaces or a trailing line break from the paste are tidied up. If someone still can't log in, check they're offering the key you added (see *If you have more than one key* below).

Someone who connects from more than one computer can have a key for each. Manage Users lists each user once, with their call signs, access and how many keys they have. **F4 (Edit)** and the user's row number, or **Enter** on a user, opens their **Edit User** window. Its **Username**, call signs and **Access** are saved together with **F2**, from the user's next login. Renaming them renames their settings file and their export and import folders too. Below those is each of their keys on its own row, told apart by its type, fingerprint and comment (the same fingerprint `ssh-keygen -lf ~/.ssh/id_ed25519.pub` prints, so they can check which key is which). **F4** adds a pasted key and **F3** removes one, straight away; removing their last key removes the user. Adding the same username again on the main page also adds the key to that user, and adding a key they already have only updates its comment. **F3 (Remove)** on the main page removes a user and all their keys.

That person can now connect:

```
ssh -p 2222 <username>@<host>
```

Public-key authentication only — there's no password option.

The same address, port and key work for `scp` and `sftp`, which reach only that user's own exports and imports (see the [User Guide](USER_GUIDE.md)).

Besides copying files with scp, the server runs seven commands of its own: `import-session` and `import-net` (for another QuickLogger pushing a session or a net to it), `discard-upload` (to withdraw an upload), `list-nets`, `export-net` and `export-sessions` (for another QuickLogger pulling nets and sessions from it, which view-only users may do too) and `version`; it never runs a shell or any other program. See [Upstream commands](IMPORT_SESSION.md).
## View-only users

A user can be **view-only**: they can watch open net sessions, look at and export History, and change their own settings, and nothing else. They can't create, edit, import or start nets, log or edit check-ins, save stations to a net, or delete anything. Their key bars and Help show only the keys they can use. Choose **Access** (Full access or View-only) when adding a user in Manage Users, or change an existing user's Access in their Edit User window (**F4 (Edit)**). It applies to all of that user's keys, and takes effect from their next login. Users added before 1.6.0 have full access until you change them. (Users added before GMRS support have their username as their amateur call sign; change it in their Edit User window if it isn't one.)

## Why Manage Users is console-only

Adding/removing SSH users is deliberately only reachable from whoever is physically at the machine's own console — never over SSH, regardless of whose key the SSH session authenticated with. This isn't a missing feature; it's a deliberate simplification: it means there's no admin/permission system to build, and no way for a compromised or malicious SSH session to ever add itself another account, no matter what. The trade-off is that adding a new operator always needs someone at the console in that moment.

## Command-line flags

- `--ssh-port=N` — listen on port `N` instead of the default **2222**. QuickLogger does not default to port 22, since that's almost always already the machine's real system `sshd`. If you specifically want QuickLogger on port 22 instead (replacing the system's own sshd, or on a machine with no other sshd), you'll need to run it with permission to bind that privileged port (root, or `setcap cap_net_bind_service` on Linux) — pass `--ssh-port=22` once you've arranged that.
- `--no-ssh` — disable the SSH listener entirely; local console use only.
- `--version` — print QuickLogger's version and exit. (The version is also shown in the top bar of every page.)
- `--headless` — skip the local console session entirely and just run the SSH listener, for an unattended server with no one sitting at it. (Note: since Manage Users is console-only, you'll need to add users *before* switching to headless-only operation, or run a normal console session briefly whenever a new user needs adding.)

## How it runs

Launched normally, QuickLogger starts two small helper processes alongside the console session — the SSH listener and the station-data updater — so you'll see three `QuickLogger` entries in `ps` (two with `--headless`, where there's no console session and the main process is the listener). While station data is being refreshed there's briefly one more, the refresh itself. That's intentional, not strays: a process that has the database open can't safely fork off others (SQLite doesn't allow it), and the updater has to outlive any one session. The helpers exit on their own when the process that started them quits or dies.

## Firewall / networking

Whatever port QuickLogger's SSH listener uses needs to actually be reachable (and, for [Mosh](CONNECTING.md#mosh), UDP ports 60000 to 61000) — opening it in your OS's firewall, and/or forwarding it through a router/NAT if you're connecting from outside the local network, is on you to set up. CMake and the app itself have no way to do this for you.

## Host key

A dedicated ed25519 host key (`ssh_host_ed25519_key`, separate from the machine's own real SSH host keys) is generated automatically the first time the SSH listener starts, and reused after that. It's `chmod 600`'d and gitignored — treat it like any other private key. Delete it (while QuickLogger isn't running) to force a fresh one to be generated on next launch; clients that connected before will see a "host key changed" warning afterward, same as with any SSH server.
