# Running QuickLogger as a FreeBSD server

These files set up a FreeBSD 15 server (amd64 or arm64, 1 GB of RAM or more) to run QuickLogger for SSH users around the clock, and to keep it on the latest release without anyone touching it.

## What you get

- QuickLogger on **port 22**, so users connect with just `ssh <username>@<host>`. It runs as the unprivileged `quicklogger` user, as a service (`service quicklogger status`) that restarts if it ever exits.
- The server's own SSH moved to **port 2200**, keys only, for administration.
- **Mosh** is installed, so a user can connect with `mosh <username>@<host>` as well as `ssh`, and keep their session through sleep or a change of network. It needs UDP ports 60000 to 61000 reachable. FreeBSD's own firewall is off unless you turn it on.
- Everything QuickLogger keeps in `/var/db/quicklogger`: the database, its SSH host key, each user's settings, and database backups.
- **Automatic updates.** Every 5 minutes, `quicklogger-update` checks GitHub for a new release. When one's out and its FreeBSD build is attached, it downloads it, checks its checksum and that it runs, then waits until nobody is connected (or until 4 AM, when anyone still connected is disconnected; an open net session can be resumed), backs up the database and restarts on the new version. Pre-releases are never installed. It logs to `/var/log/messages` (`grep quicklogger-update /var/log/messages`).
- Nightly FreeBSD security updates and package upgrades, neither of which restarts anything (a kernel update waits for a reboot), 1 GB of swap for the weekly station data import, and times shown in the server's time zone (America/New_York unless you say otherwise).
- **No mail to root.** Cron jobs' output is dropped, and FreeBSD's daily, weekly and monthly reports (security checks included) go to `/var/log/daily.log`, `weekly.log` and `monthly.log` instead of root's mailbox.

## Setting one up

1. Make a FreeBSD 15 server and make sure you can `ssh root@<host>` with a key.
2. Copy this directory to it and run the setup as root: `scp -r deploy/freebsd root@<host>:` then `ssh root@<host> sh freebsd/setup.sh` (for another time zone, `TIME_ZONE=America/Chicago sh freebsd/setup.sh`). From then on the server's own SSH is on port 2200: `ssh -p 2200 root@<host>`.
3. To start with an existing database rather than an empty one, stop the service, copy your `quicklogger.db` into `/var/db/quicklogger` (owned by `quicklogger`), and start it again. Use a copy made with `sqlite3 quicklogger.db ".backup copy.db"` if QuickLogger is running where it came from.
4. Point a DNS name at the server if you like. A cloud firewall in front of it (the provider's, not the server's) needs TCP ports 22 and 2200 open, and UDP ports 60000 to 61000 for Mosh.

## Adding users

Manage Users opens from the console, and a server's console is the admin SSH: `ssh -p 2200 root@<host>`, then run `quicklogger-admin`, go to Settings (F4) and Manage Users (F5). Users you make Admins can also open it from their own logins. It runs alongside the server, on the same database; nothing needs stopping.

## Rolling back

Each install keeps the previous binary as `/usr/local/bin/QuickLogger.previous` and a backup of the database from just before it in `/var/db/quicklogger/backups`. To go back: `service quicklogger stop`, copy those back in place (`gunzip` the backup to `/var/db/quicklogger/quicklogger.db`, owned by `quicklogger`), and `service quicklogger start`. The updater will install the newer release again at its next check unless it's withdrawn, so mark that release as a pre-release on GitHub first.
