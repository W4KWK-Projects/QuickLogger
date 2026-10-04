# Connecting to a shared QuickLogger

For operators who connect to a QuickLogger someone else runs. Whoever runs it will add your key and tell you the host and port (see [Shared server](SHARED_SERVER.md)); the default port is 2222.

## Creating your SSH key

Anyone who wants to connect needs an SSH key pair, created on the machine they'll be connecting **from**. The private half never leaves that machine; only the public half (the `.pub` file) gets handed to whoever is adding you in Manage Users.

**1. See whether you already have a key.**

```
ls ~/.ssh/*.pub
```

If that lists `id_ed25519.pub`, skip to step 3. A "No such file or directory" error just means you've never made one — normal on a new account, and step 2 fixes it.

**2. Create one.** (Same command on macOS, Linux, and Windows PowerShell — Windows 10+ ships with the OpenSSH client.)

```
ssh-keygen -t ed25519 -C "your-name-or-callsign"
```

Press Enter to accept the default file location. When it asks for a passphrase, choose one (recommended) or press Enter twice for none. This creates `~/.ssh/` if it doesn't exist, plus two files in it:

- `id_ed25519` — your **private** key. Never share it, email it, or paste it anywhere.
- `id_ed25519.pub` — your **public** key. This is the one you give out.

**3. Print your public key and send it to the person running QuickLogger.**

```
cat ~/.ssh/id_ed25519.pub
```

(On Windows PowerShell: `type $env:USERPROFILE\.ssh\id_ed25519.pub`. On macOS you can copy it straight to the clipboard with `pbcopy < ~/.ssh/id_ed25519.pub`.)

The output is a single line that looks like this:

```
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI... your-name-or-callsign
```

That whole line — starting with `ssh-ed25519`, on one line with no line breaks — is what gets pasted into Manage Users. It's a public key, so sending it by email or chat is fine. Send your call signs with it, and the username you'd like: what you'll type before the `@` when you connect (in either case). If you'll connect from more than one computer, send each computer's public key for the same username.

**If you have more than one key**, tell `ssh` which one to offer so it doesn't pick the wrong one:

```
ssh -i ~/.ssh/id_ed25519 -p 2222 <username>@<host>
```

## Slow or metered connections: turn on compression

QuickLogger redraws the whole screen whenever anything changes — every keypress, and once a minute for the clock in the top bar. Uncompressed, one redraw is about 3 KB on an 80×24 terminal (most of it blank space and border characters), which is fine on a LAN but adds up over a slow, metered or radio-linked connection.

Ask your SSH client to compress the session and that drops to roughly 0.2 KB per redraw — over 90% less. No server setup is needed. It's off by default in OpenSSH, so opt in with `-C`:

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

after which `ssh quicklogger` is all you type. Most other SSH clients have an equivalent "enable compression" setting.

## If the screen freezes after you've been away

Over the internet, a session can look frozen when you come back to it (nothing you type or click does anything) even though your terminal still shows it connected. Running QuickLogger on your own machine never does this, because no network sits in between.

Being idle isn't the cause: the server redraws the clock every minute, so a connection is never quiet. The connection itself has died, usually because your computer slept, your Wi-Fi or VPN reconnected, or your network changed. Your SSH client doesn't find out until it tries to send something, and it can then keep waiting for many minutes before giving up.

The fix is on your end: have your SSH client check the connection every 30 seconds, so a dead one is closed within about a minute and a half instead of leaving a frozen screen. With `ssh`, add `-o ServerAliveInterval=30`:

```
ssh -o ServerAliveInterval=30 -p 2222 <username>@<host>
```

or add it to your `~/.ssh/config` entry (see above):

```
Host quicklogger
    HostName <host>
    Port 2222
    User <username>
    Compression yes
    ServerAliveInterval 30
```

PuTTY's equivalent is Connection → "Seconds between keepalives" (set it to 30). When a session does drop, just reconnect: a net session you were running is still open, and QuickLogger offers to resume it.

Or use Mosh, below, which doesn't drop at all.

## Mosh

[Mosh](https://mosh.org) keeps your session through sleep, a change of Wi-Fi or cell network, and a dropped connection: the screen picks up where it was as soon as your device is back online. It's well suited to a laptop or phone on an unreliable connection.

It needs `mosh-server` installed on the server (see [Optional: `mosh`](INSTALL.md#optional-mosh-mosh-connections)) and UDP ports 60000 to 61000 open to it, in the server's firewall and any in front of it. You need the Mosh client: `brew install mosh`, `sudo apt install mosh` and so on, or Blink Shell on iOS; on Windows, use it from WSL. Connect the way you would with `ssh`, giving `mosh` the port:

```
mosh --ssh="ssh -p 2222" <username>@<host>
```

Mosh logs in with your SSH key as usual, then switches to UDP. You always get QuickLogger: a command given after `--` is ignored. A Mosh session you never come back to ends after a day.

**Termius:** turn on Use Mosh for the host, set Port to QuickLogger's SSH port, and leave the Mosh command field empty. Termius's own default works.

Mosh keeps the screen in step rather than passing data through, so ZMODEM doesn't work over it: QuickLogger doesn't offer it, and you copy files with `scp` or `sftp` instead (see the [User Guide](USER_GUIDE.md)).
