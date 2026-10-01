#!/bin/sh
#
# One-time setup of a fresh Debian (12, 13) or Ubuntu (22.04, 24.04) server
# to run QuickLogger, run as root from this directory (copied to the
# server). Afterwards the server keeps itself on the latest release
# (quicklogger-update, from a systemd timer).
#
# - The server's own SSH moves to port 2200 (keys only), freeing port 22
#   for QuickLogger, so users just type: ssh <username>@<host>
# - QuickLogger runs as the unprivileged quicklogger user, from
#   /var/lib/quicklogger (database, host key, per-user settings, backups).
# - Security updates install nightly (unattended-upgrades), without
#   rebooting.
# - Everything logs to the journal: journalctl -u quicklogger, and
#   journalctl -t quicklogger-update for the updater.
#
# To seed the database, copy quicklogger.db (and optionally
# ssh_host_ed25519_key) into /var/lib/quicklogger, owned by quicklogger,
# with the service stopped.
#
# Keep the SSH session you ran this from open until `ssh -p 2200
# root@<host>` works from another window.

set -eu

cd "$(dirname "$0")"

if [ "$(id -u)" != 0 ]; then
    echo "Run this as root." >&2
    exit 1
fi
if ! command -v apt-get > /dev/null; then
    echo "This is for Debian and Ubuntu (apt); see README.md." >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
# libcurl's package is libcurl4t64 on Debian 13 and Ubuntu 24.04 and
# newer, libcurl4 before them.
if apt-cache show libcurl4t64 > /dev/null 2>&1; then
    libcurl=libcurl4t64
else
    libcurl=libcurl4
fi
apt-get install -y -q libssh-4 "$libcurl" libsqlite3-0 sqlite3 lrzsz jq curl ca-certificates \
    unattended-upgrades

# The admin SSH: port 2200, keys only. A drop-in file, which sshd reads
# before the main config (its first value for each setting wins).
install -d /etc/ssh/sshd_config.d
printf 'Port 2200\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n' \
    > /etc/ssh/sshd_config.d/10-quicklogger.conf
if ! grep -q '^Include /etc/ssh/sshd_config.d/' /etc/ssh/sshd_config; then
    sed -i '1i Include /etc/ssh/sshd_config.d/*.conf' /etc/ssh/sshd_config
fi
sed -i -e 's/^Port 22$/#Port 22/' /etc/ssh/sshd_config
# The config check needs the directory the SSH service makes when it
# starts, which a socket-started sshd (Ubuntu 24.04) may not have yet.
install -d -m 755 /run/sshd
sshd -t
# Ubuntu 22.10 and newer start sshd from ssh.socket. From 24.04 a
# generator builds its port from sshd_config, so the change above is
# enough; before that the socket listens on 22 whatever sshd_config says,
# so it's moved to 2200 here.
if systemctl is-enabled ssh.socket > /dev/null 2>&1 &&
    [ ! -x /usr/lib/systemd/system-generators/sshd-socket-generator ] &&
    [ ! -x /lib/systemd/system-generators/sshd-socket-generator ]; then
    install -d /etc/systemd/system/ssh.socket.d
    printf '[Socket]\nListenStream=\nListenStream=2200\n' \
        > /etc/systemd/system/ssh.socket.d/10-quicklogger.conf
fi

# A firewall that's on needs both ports open.
if command -v ufw > /dev/null && ufw status | grep -q '^Status: active'; then
    ufw allow 2200/tcp
    ufw allow 22/tcp
fi

# QuickLogger shows every time in the server's time zone.
TIME_ZONE="${TIME_ZONE:-America/New_York}"
timedatectl set-timezone "$TIME_ZONE"

# 1 GB of swap, as headroom for the weekly station data import on a 1 GB
# server, if there's none.
if [ -z "$(swapon --show --noheadings)" ] && [ ! -e /swapfile ]; then
    fallocate -l 1G /swapfile
    chmod 600 /swapfile
    mkswap /swapfile
    echo '/swapfile none swap sw 0 0' >> /etc/fstab
    swapon /swapfile
fi

if ! id quicklogger > /dev/null 2>&1; then
    useradd --system --home-dir /var/lib/quicklogger --shell /usr/sbin/nologin quicklogger
fi
install -d -o quicklogger -g quicklogger -m 750 /var/lib/quicklogger /var/lib/quicklogger/backups
chown -R quicklogger:quicklogger /var/lib/quicklogger

install -m 644 quicklogger.service quicklogger-update.service quicklogger-update.timer \
    /etc/systemd/system/
install -m 755 quicklogger-update /usr/local/sbin/quicklogger-update
install -m 755 quicklogger-admin /usr/local/sbin/quicklogger-admin
systemctl daemon-reload
systemctl enable quicklogger.service
systemctl enable --now quicklogger-update.timer

# Security updates nightly, never rebooting on their own (a kernel update
# waits for a reboot).
cat > /etc/apt/apt.conf.d/20auto-upgrades <<'APT'
APT::Periodic::Update-Package-Lists "1";
APT::Periodic::Unattended-Upgrade "1";
APT
cat > /etc/apt/apt.conf.d/52quicklogger-unattended <<'APT'
Unattended-Upgrade::Automatic-Reboot "false";
APT

# The admin SSH moves to 2200 before QuickLogger takes 22.
if systemctl is-enabled ssh.socket > /dev/null 2>&1; then
    systemctl daemon-reload
    systemctl restart ssh.socket
fi
systemctl restart ssh 2> /dev/null || systemctl restart sshd

# The first install happens now rather than in five minutes.
/usr/local/sbin/quicklogger-update

echo "Done. Admin SSH is now on port 2200; QuickLogger is on port 22."
