# Running QuickLogger as a Linux server

These files set up a Debian (12 or 13) or Ubuntu (22.04 or 24.04) server, amd64 or arm64 with 1 GB of RAM or more, to run QuickLogger for SSH users around the clock, and to keep it on the latest release without anyone touching it.

## What you get

- QuickLogger on **port 22**, so users connect with just `ssh <username>@<host>`. It runs as the unprivileged `quicklogger` user, as a systemd service (`systemctl status quicklogger`) that restarts if it ever exits.
- The server's own SSH moved to **port 2200**, keys only, for administration. If `ufw` is on, both ports are opened in it, and so are Mosh's UDP ports, 60000 to 61000.
- **Mosh** is installed, so a user can connect with `mosh <username>@<host>` as well as `ssh`, and keep their session through sleep or a change of network.
- Everything QuickLogger keeps in `/var/lib/quicklogger`: the database, its SSH host key, each user's settings, and database backups.
- **Automatic updates.** Every 5 minutes, a systemd timer runs `quicklogger-update`, which checks GitHub for a new release. When one's out and its Linux build is attached, it downloads it, checks its checksum and that it runs, then waits until nobody is connected (or until 4 AM, when anyone still connected is disconnected; an open net session can be resumed), backs up the database and restarts on the new version. Pre-releases are never installed. See what it did with `journalctl -t quicklogger-update`.
- Nightly security updates (`unattended-upgrades`), which never reboot on their own (a kernel update waits for a reboot), 1 GB of swap for the weekly station data import if the server has none, and times shown in the server's time zone (America/New_York unless you say otherwise).
- Logs in the journal: `journalctl -u quicklogger`.

## Setting one up

1. Make a Debian or Ubuntu server and make sure you can `ssh root@<host>` with a key (or a user with `sudo`).
2. Copy this directory to it and run the setup as root: `scp -r deploy/linux root@<host>:` then `ssh root@<host> sh linux/setup.sh` (for another time zone, `TIME_ZONE=America/Chicago sh linux/setup.sh`). Keep that session open until `ssh -p 2200 root@<host>` works from another window: from then on, the server's own SSH is on port 2200.
3. To start with an existing database rather than an empty one, stop the service (`systemctl stop quicklogger`), copy your `quicklogger.db` into `/var/lib/quicklogger` (owned by `quicklogger`), and start it again. Use a copy made with `sqlite3 quicklogger.db ".backup copy.db"` if QuickLogger is running where it came from.
4. Point a DNS name at the server if you like. A cloud firewall in front of it (the provider's, not the server's) needs TCP ports 22 and 2200 open, and UDP ports 60000 to 61000 for Mosh.

## Adding users

Manage Users opens from the console, and a server's console is the admin SSH: `ssh -p 2200 root@<host>`, then run `quicklogger-admin`, go to Settings (F4) and Manage Users (F5). Users you make Admins can also open it from their own logins. It runs alongside the server, on the same database; nothing needs stopping.

## Rolling back

Each install keeps the previous binary as `/usr/local/bin/QuickLogger.previous` and a backup of the database from just before it in `/var/lib/quicklogger/backups`. To go back: `systemctl stop quicklogger`, copy those back in place (`gunzip` the backup to `/var/lib/quicklogger/quicklogger.db`, owned by `quicklogger`), and `systemctl start quicklogger`. The updater will install the newer release again at its next check unless it's withdrawn, so mark that release as a pre-release on GitHub first, or stop the updater: `systemctl disable --now quicklogger-update.timer`.
