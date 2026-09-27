#!/bin/sh
#
# One-time setup of a fresh FreeBSD server to run QuickLogger, run as root
# from this directory (copied to the server). Afterwards the server keeps
# itself on the latest release (quicklogger-update, from cron).
#
# - The server's own SSH moves to port 2200 (keys only), freeing port 22
#   for QuickLogger, so users just type: ssh <username>@<host>
# - QuickLogger runs as the unprivileged quicklogger user, from
#   /var/db/quicklogger (database, host key, per-user settings, backups).
# - FreeBSD's own security updates and package audits run nightly.
#
# To seed the database, copy quicklogger.db (and optionally
# ssh_host_ed25519_key) into /var/db/quicklogger before or after running
# this, owned by quicklogger.

set -eu

cd "$(dirname "$0")"

env ASSUME_ALWAYS_YES=yes pkg bootstrap -f
pkg install -y libssh curl sqlite3 lrzsz jq

# The admin SSH: port 2200, keys only.
sysrc sshd_enable=YES
sed -i '' -e '/^#*Port /d' -e '/^#*PasswordAuthentication /d' -e '/^#*KbdInteractiveAuthentication /d' /etc/ssh/sshd_config
printf 'Port 2200\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n' >> /etc/ssh/sshd_config

# QuickLogger shows every time in the server's time zone.
TIME_ZONE="${TIME_ZONE:-America/New_York}"
tzsetup "$TIME_ZONE"

# 1 GB of swap, as headroom for the weekly station data import on a 1 GB
# server.
if ! grep -q '^md99' /etc/fstab; then
    dd if=/dev/zero of=/usr/swap0 bs=1m count=1024
    chmod 600 /usr/swap0
    echo 'md99 none swap sw,file=/usr/swap0,late 0 0' >> /etc/fstab
    swapon -aL
fi

# Only the first virtual console gets a login prompt (it's what the
# provider's web console shows); the other seven are never used.
sed -i '' -E '/^ttyv[1-7][[:space:]]/s/onifexists/off/' /etc/ttys
kill -HUP 1

# Lets an unprivileged process listen on port 22 (ports below 22 stay
# root-only).
sed -i '' '/^net\.inet\.ip\.portrange\.reservedhigh=/d' /etc/sysctl.conf
echo 'net.inet.ip.portrange.reservedhigh=21' >> /etc/sysctl.conf
sysctl net.inet.ip.portrange.reservedhigh=21

if ! pw usershow quicklogger > /dev/null 2>&1; then
    pw useradd quicklogger -c "QuickLogger" -d /var/db/quicklogger -s /usr/sbin/nologin
fi
install -d -o quicklogger -g quicklogger -m 750 /var/db/quicklogger /var/db/quicklogger/backups
chown -R quicklogger:quicklogger /var/db/quicklogger
touch /var/log/quicklogger.log
chown quicklogger:quicklogger /var/log/quicklogger.log

install -d /usr/local/etc/rc.d /usr/local/sbin /etc/cron.d /etc/newsyslog.conf.d
install -m 555 quicklogger.rc /usr/local/etc/rc.d/quicklogger
install -m 555 quicklogger-update /usr/local/sbin/quicklogger-update
install -m 555 quicklogger-admin /usr/local/sbin/quicklogger-admin
sysrc quicklogger_enable=YES quicklogger_port=22

# Updates: QuickLogger every 5 minutes; FreeBSD and packages nightly.
# Neither nightly job restarts anything: QuickLogger picks up upgraded
# libraries at its next restart (its next release), and a kernel update
# waits for a reboot.
cat > /etc/cron.d/quicklogger <<'CRON'
SHELL=/bin/sh
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin
*/5 * * * * root lockf -t 0 /var/run/quicklogger-update.lock /usr/local/sbin/quicklogger-update
30 3 * * * root freebsd-update cron && freebsd-update install --not-running-from-cron > /dev/null 2>&1 || true
45 3 * * * root pkg upgrade -y > /dev/null 2>&1 || true
CRON
# Rotate the log weekly, keeping 4.
echo '/var/log/quicklogger.log quicklogger:quicklogger 640 4 * $W0D0 JC' > /etc/newsyslog.conf.d/quicklogger.conf

# The first install happens now rather than in five minutes.
/usr/local/sbin/quicklogger-update
service sshd restart

echo "Done. Admin SSH is now on port 2200; QuickLogger is on port 22."
