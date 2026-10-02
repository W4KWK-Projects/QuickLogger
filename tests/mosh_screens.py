#!/usr/bin/env python3
"""Mosh through the real client, against the SSH server ssh_end_to_end.sh started.

  mosh_screens.py <run-dir> <port>

Runs the system's mosh (mosh-client and the mosh script) in a pseudo-terminal,
read back through pyte: the server must accept the client's mosh-server
request, start QuickLogger under mosh-server logged in as the SSH user, show
the net list, answer keys, and end when the client quits. Needs mosh and pyte;
tests/ssh_end_to_end.sh skips this without them. Mosh's UDP traffic is on this
machine only. The `ssh` found on PATH is the script's wrapper (key and
known_hosts of the run).
"""
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

import pyte

COLS, ROWS = 100, 30
failures = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


def utf8_locale():
    """A UTF-8 locale this machine has: mosh refuses to run without one."""
    names = subprocess.run(["locale", "-a"], capture_output=True, text=True).stdout.split()
    for wanted in ("C.UTF-8", "en_US.UTF-8", "C.utf8", "en_US.utf8"):
        if wanted in names:
            return wanted
    return None


def mosh_servers():
    out = subprocess.run(["pgrep", "-f", "mosh-server new.*--mosh-session"], capture_output=True, text=True)
    return set(out.stdout.split())


class Client:
    def __init__(self, user, port, locale):
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["LANG"] = locale
            os.environ["LC_ALL"] = locale
            os.environ["TERM"] = "xterm-256color"
            os.execvp("mosh", ["mosh", "--ssh=ssh -p %s" % port, "%s@127.0.0.1" % user])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                try:
                    self.stream.feed(os.read(self.fd, 65536))
                except OSError:
                    return

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def wait_for(self, needle, seconds=20):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.1)
            if needle in self.text():
                return True
        return False

    def send(self, data):
        os.write(self.fd, data.encode())
        time.sleep(0.2)
        self.pump(0.5)

    def expect(self, needle, what, seconds=20):
        found = self.wait_for(needle, seconds)
        check(found, what)
        if not found:
            print("--- screen:\n" + self.text() + "\n---")
        return found

    def finish(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


def first_login(client, user):
    """A new SSH user starts on Settings, asking for a ZIP; save one for the net list."""
    client.expect("— Settings", "mosh: %s is logged in, first on Settings" % user)
    client.send("37415")
    client.send("\x1bOQ")  # F2
    client.expect("Recurring Nets", "mosh: %s reaches the net list" % user)


def main():
    directory, port = sys.argv[1], sys.argv[2]
    locale = utf8_locale()
    if locale is None:
        print("Skipping Mosh: this machine has no UTF-8 locale.")
        return
    before = mosh_servers()

    client = Client("W4KWK", port, locale)
    first_login(client, "W4KWK")
    net_list = client.text()
    client.send("\x1bOP")  # F1: a key reaches the program and the screen changes
    client.pump(1)
    check(client.text() != net_list, "mosh: the screen answers keys")
    client.send("\x1b")
    started = mosh_servers() - before
    check(len(started) >= 1, "mosh: a mosh-server is running QuickLogger")

    # Ctrl-^ . is mosh-client's quit; the server side ends with it.
    client.send("\x1e.")
    client.pump(2)
    end = time.time() + 15
    while time.time() < end and (mosh_servers() & started):
        time.sleep(0.5)
    check(not (mosh_servers() & started), "mosh: the server side ended with the client")
    client.finish()

    # A view-only user logs in the same way.
    client = Client("K4VIEW", port, locale)
    first_login(client, "K4VIEW")
    client.send("\x1e.")
    client.finish()

    print("")
    if failures:
        print("%d Mosh check(s) failed" % len(failures))
        sys.exit(1)
    print("mosh: all checks passed")


main()
