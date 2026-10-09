# Shared server: the built-in SSH server

QuickLogger can accept SSH connections directly — `ssh <username>@host` drops you straight into the app, no separate login+launch step. This is **on by default** on macOS, Linux and FreeBSD. It isn't part of the Windows build (see [Running on Windows](INSTALL.md#running-on-windows)), or of any build made with `-DQUICKLOGGER_ENABLE_SSH=OFF`; those are console-only.

## First-time setup: adding a user

There's no OS user account involved. Instead, QuickLogger keeps its own small roster of usernames and public keys, and it is managed from the **local console session**, or over SSH by users you make [Admins](#admins). To add the first SSH user:

1. Run `./QuickLogger` directly at the machine's own console.
2. Go to **Settings** (F4 from the Recurring Nets list).
3. Press **F5 (Manage Users)**.
4. Fill in the fields at the bottom of the page — **Username** (what they type in `ssh <username>@host`, in either case: 1 to 32 letters, digits, dots, hyphens and underscores, starting with a letter or digit), **Amateur Call** and **GMRS Call** (that person's call signs, at least one of them; an amateur one is US or Canadian, without /M, /P or the like), **Public Key** (that person's whole public-key line, e.g. `ssh-ed25519 AAAA... their-comment`) and **Access** (see [View-only users](#view-only-users)). Then press **F2 (Add)**. Their call signs are the ones they log under: their Settings page shows them, and they can't change them. Without a GMRS call sign they can only watch GMRS nets, and likewise for amateur nets. If they don't have a key yet, see [Creating your SSH key](CONNECTING.md#creating-your-ssh-key) below.

The key is checked when you add it. A private key, a PuTTY-format key, a line missing its `ssh-ed25519` (or other type) at the start, or one cut short while copying is refused with a message saying what's wrong and what the line should look like. Extra spaces or a trailing line break from the paste are tidied up. If someone still can't log in, check they're offering the key you added (see *If you have more than one key* below).

Someone who connects from more than one computer can have a key for each. Manage Users lists each user once, with their call signs, access and how many keys they have. **F4 (Edit)** and the user's row number, or **Enter** on a user, opens their **Edit User** window. Its **Username**, call signs and **Access** are saved together with **F2**, from the user's next login. Renaming them renames their settings file and their export and import folders too. Below those is each of their keys on its own row, told apart by its type, fingerprint and comment (the same fingerprint `ssh-keygen -lf ~/.ssh/id_ed25519.pub` prints, so they can check which key is which). **F4** adds a pasted key and **F3** removes one, straight away; removing their last key removes the user. Adding the same username again on the main page also adds the key to that user, and adding a key they already have only updates its comment. **F3 (Remove)** on the main page removes a user and all their keys.

In the Edit User window, **F5** turns the highlighted key off (or back on) and **F6** turns all of the user's keys off (or back on). A key that is off stays on file, marked `[off]`, but can't log in; the main list shows a user whose keys are all off as **Disabled**, and "1/2" in Keys when one of two works. Each key's row also says when it was added and by whom, the admin or the user (on wide terminals in a column, on 80 columns on the line under the list). Turning a key off doesn't end a session that is already logged in.

**F5 (Own Keys)** on the main page lets users add and remove their own keys in My Keys (see the User Guide). It is off until you turn it on, and it applies to every user. A key a user adds is marked as added by the user. Users can't remove a key that is off, which stays yours to turn on or remove. **F6 (Key Log)** shows the last changes to who can log in, newest first: keys added, removed, turned off or on, users removed, and Own Keys turned on or off, with who did it and when. It shows the newest 100 and keeps the last 20,000. **F7** in the log saves all it keeps as `exports/key_log_DATE.csv`, oldest first, for a spreadsheet (over a remote terminal the file is sent to you like any export).

That person can now connect:

```
ssh -p 2222 <username>@<host>
```

Public-key authentication only — there's no password option.

The same address, port and key work for `scp` and `sftp`, which reach only that user's own exports and imports (see the [User Guide](USER_GUIDE.md)).

Besides copying files with scp, the server runs seven commands of its own: `import-session` and `import-net` (for another QuickLogger pushing a session or a net to it), `discard-upload` (to withdraw an upload), `list-nets`, `export-net` and `export-sessions` (for another QuickLogger pulling nets and sessions from it, which view-only users may do too) and `version`; it never runs a shell or any other program. See [Upstream commands](IMPORT_SESSION.md).
## View-only users

A user can be **view-only**: they can watch open net sessions, look at and export History, and change their own settings, and nothing else. They can't create, edit, import or start nets, log or edit check-ins, save stations to a net, or delete anything. Their key bars and Help show only the keys they can use. Choose **Access** (Full access, View-only, Net Admin or Admin) when adding a user in Manage Users, or change an existing user's Access in their Edit User window (**F4 (Edit)**). It applies to all of that user's keys, and takes effect from their next login. Users added before 1.6.0 have full access until you change them. (Users added before GMRS support have their username as their amateur call sign; change it in their Edit User window if it isn't one.)

## Admins

Manage Users is open to the local console and to SSH users whose **Access** is **Admin**. Choose Admin when adding a user, or in their Edit User window. Admins can do everything full users can, and open Manage Users with **F5** on Settings, as at the console. From it they add, edit and remove users and keys, turn keys off and on, switch Own Keys and read the Key Log, just as at the console. Every change they make is in the Key Log under their name.

An Admin can't remove, rename or demote themself, remove or turn off the key they are logged in with, or turn off all their own keys. A change to someone's Access applies from their next login, but an Admin who has been demoted is refused at once.

Settings that touch the machine itself (Server, Update Check, Refresh Data, Upstream Server) stay at the local console, which is also the way back in if every Admin is locked out.

Nobody is an Admin until you make them one; the console always is.

## Net Admins and restricted nets

By default every full user can log and change every net. Two settings change that, and both are off until you use them.

**Restricted** (**F8** on Manage Users, shown as *Restricted: On* or *Off*) limits full users to the nets they have been given. With it on, a full user can log a net they have (start or join its sessions, log and edit check-ins, close them, import and export, and add and edit its saved stations). On any other net they can only watch, as a view-only user does, and look at and export its History. They can't change a net's details, delete a net, remove its saved stations, or delete its sessions or check-ins. Ad hoc nets are never restricted: every full user can do everything with them. Turning Restricted on takes effect at once, so give people their nets first (or straight after).

A **Net Admin** (Access: Net Admin) looks after the nets they have been given. On those nets they can do everything: change the net's details, delete it, remove saved stations, and delete its sessions and check-ins in History. With Restricted on, they can also add a net and are given it, and a full user can't. They give their nets to full users in **Net Access** (**F5** on their Settings page), but never a net they don't have, and they can't make users or give anyone else a Net Admin's role. An Admin has every net, and gives any net to full users and Net Admins in **Net Access** (**F7** on Manage Users): pick the net, then **Enter** on a user gives them the net or takes it back. A view-only user can't be given a net.

Restricted covers the other ways in, too: with it on, a push from another QuickLogger (`import-session`, `import-net`) only goes into nets its user has, and only a Net Admin's push can add a new net. Giving and taking nets, and turning Restricted on or off, are in the Key Log (**F6** on Manage Users). Deleting a net, or a user, removes the nets they were given.

## Command-line flags

- `--ssh-port=N` — listen on port `N` instead of the default **2222**. QuickLogger does not default to port 22, since that's almost always already the machine's real system `sshd`. If you specifically want QuickLogger on port 22 instead (replacing the system's own sshd, or on a machine with no other sshd), you'll need to run it with permission to bind that privileged port (root, or `setcap cap_net_bind_service` on Linux) — pass `--ssh-port=22` once you've arranged that.
- `--no-ssh` — disable the SSH listener entirely; local console use only.
- `--version` — print QuickLogger's version and exit. (The version is also shown in the top bar of every page.)
- `--headless` — skip the local console session entirely and just run the SSH listener, for an unattended server with no one sitting at it. (Note: the first Admin has to be made at the console, so add users and make at least one an Admin *before* switching to headless-only operation; after that Admins manage users over SSH.)

## How it runs

Launched normally, QuickLogger starts two small helper processes alongside the console session — the SSH listener and the station-data updater — so you'll see three `QuickLogger` entries in `ps` (two with `--headless`, where there's no console session and the main process is the listener). While station data is being refreshed there's briefly one more, the refresh itself. That's intentional, not strays: a process that has the database open can't safely fork off others (SQLite doesn't allow it), and the updater has to outlive any one session. The helpers exit on their own when the process that started them quits or dies.

## Firewall / networking

Whatever port QuickLogger's SSH listener uses needs to actually be reachable (and, for [Mosh](CONNECTING.md#mosh), UDP ports 60000 to 61000) — opening it in your OS's firewall, and/or forwarding it through a router/NAT if you're connecting from outside the local network, is on you to set up. CMake and the app itself have no way to do this for you.

## Host key

A dedicated ed25519 host key (`ssh_host_ed25519_key`, separate from the machine's own real SSH host keys) is generated automatically the first time the SSH listener starts, and reused after that. It's `chmod 600`'d and gitignored — treat it like any other private key. Delete it (while QuickLogger isn't running) to force a fresh one to be generated on next launch; clients that connected before will see a "host key changed" warning afterward, same as with any SSH server.
