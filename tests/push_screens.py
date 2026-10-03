#!/usr/bin/env python3
"""The Federated Logging push screens, through the real program.

Run by tests/ssh_end_to_end.sh after the other end-to-end tests, against the
upstream it started: Close & Push, History's F3, and the look-alike confirm
prompt, driven in a pseudo-terminal and read back through pyte.

  push_screens.py <QuickLogger> <run-dir> <port>

Needs pyte (pip install pyte); tests/ssh_end_to_end.sh skips this when it
isn't installed. Python does no networking here, only the pseudo-terminal:
ssh and scp are run by QuickLogger itself.
"""
import fcntl
import os
import pty
import select
import signal
import sqlite3
import struct
import sys
import termios
import time

import pyte

KEYS = {
    "F2": "\x1bOQ", "F3": "\x1bOR", "F4": "\x1bOS", "F6": "\x1b[17~",
    "Esc": "\x1b", "Enter": "\r", "Down": "\x1b[B", "Up": "\x1b[A",
}
COLS = int(os.environ.get("QL_SCREEN_COLS", "100"))
ROWS = int(os.environ.get("QL_SCREEN_ROWS", "30"))
failures = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


class Program:
    """QuickLogger on a pseudo-terminal, with a pyte screen."""

    def __init__(self, binary, run_dir):
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(run_dir)
            os.execv(binary, [binary, "--no-ssh"])
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

    def send(self, *keys):
        for key in keys:
            os.write(self.fd, KEYS.get(key, key).encode())
            time.sleep(0.2)  # two Escapes closer than this merge
            self.pump(0.3)

    def expect(self, needle, what, seconds=20):
        found = self.wait_for(needle, seconds)
        check(found, what)
        if not found:
            print("--- screen:\n" + self.text() + "\n---")
        return found

    def quit(self):
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)


def upstream_check_ins(directory, net, date):
    """How many check-ins the upstream's `net` has on `date`; None if no session."""
    db = sqlite3.connect("file:" + directory + "/up/quicklogger.db?mode=ro", uri=True)
    try:
        row = db.execute(
            "SELECT COUNT(c.id) FROM net_instances i JOIN nets n ON n.id = i.net_id "
            "LEFT JOIN check_ins c ON c.net_instance_id = i.id "
            "WHERE n.name = ? AND i.instance_date = ? GROUP BY i.id", (net, date)).fetchone()
        return None if row is None else row[0]
    finally:
        db.close()


def uploads_left(directory):
    """The .qlsession uploads in any user's /imports on the upstream."""
    found = []
    for root, _, files in os.walk(directory + "/up"):
        if "imports" in root.split(os.sep):
            found += [f for f in files if f.endswith(".qlsession")]
    return found


def uploads_gone(directory, seconds=10):
    """True once no upload is left on the upstream (a declined push's is deleted by a background ssh)."""
    end = time.time() + seconds
    while time.time() < end:
        if not uploads_left(directory):
            return True
        time.sleep(0.2)
    return False


def settings(run_dir, port, user):
    with open(run_dir + "/settings.txt", "w") as f:
        f.write("callsign=W4KWK\nlocation=37415\nupstream_host=127.0.0.1\n"
                "upstream_user=%s\nupstream_port=%s\n" % (user, port))


def main():
    binary, directory, port = sys.argv[1], sys.argv[2], sys.argv[3]
    run_dir = directory + "/local-screens"

    # A view-only upstream user: the push is refused, and nothing is marked pushed.
    settings(run_dir, port, "K4VIEW")
    program = Program(binary, run_dir)
    program.expect("Recurring Nets", "view-only: net list shows")
    program.send("Up", "Up", "Down", "Down")  # TAG Skywarn
    program.send("F6")
    program.expect("History: TAG Skywarn", "view-only: History opens")
    program.send("Down", "F3")  # past the open session, to the closed one
    program.expect("Your user on 127.0.0.1 is view-only.", "view-only: push says so")
    check(upstream_check_ins(directory, "TAG Skywarn", "2026-10-06") is None,
          "view-only: nothing reached the upstream")
    selected = [line for line in program.text().split("\n") if line.startswith("\u2502>")]
    check(selected and "pushed" not in selected[0],
          "view-only: the session isn't marked pushed")
    program.quit()

    # Nobody listening: a plain connection failure.
    settings(run_dir, "1", "W4KWK")
    program = Program(binary, run_dir)
    program.expect("Recurring Nets", "unreachable: net list shows")
    program.send("Down", "Down", "F6")
    program.expect("History: TAG Skywarn", "unreachable: History opens")
    program.send("Down", "F3")
    program.expect("Couldn't reach 127.0.0.1.", "unreachable: push says so")
    program.quit()

    settings(run_dir, port, "W4KWK")
    program = Program(binary, run_dir)
    program.expect("Recurring Nets", "net list shows")

    # Close & Push on the open TAG Skywarn session.
    program.send("Down", "Down", "Enter")
    program.expect("Session Still Open", "open session: asks what to do")
    program.send("Enter")
    program.expect("Resumed the 2026-10-13", "open session: resumed")
    program.send("F4")
    program.expect("F3  Close & Push", "close prompt offers Close & Push")
    program.send("F3")
    program.expect("Pushed to TAG Skywarn on 127.0.0.1.", "Close & Push: pushed")
    program.expect("Recurring Nets", "Close & Push: back on the net list")
    check(upstream_check_ins(directory, "TAG Skywarn", "2026-10-13") == 2,
          "Close & Push: the upstream has the 2 check-ins")

    # History: that session reads "pushed", and can't be pushed again.
    program.send("F6")
    program.expect("History: TAG Skywarn", "History opens")
    program.expect("2026-10-13", "History lists the closed session")
    check("pushed" in program.text(), "History shows it as pushed")
    program.send("F3")
    program.expect("That session has been pushed already.", "F3 on a pushed session refuses")

    # The older closed session: F3 pushes it.
    program.send("Down", "F3")
    program.expect("Pushed to TAG Skywarn on 127.0.0.1.", "History F3: pushed")
    check(upstream_check_ins(directory, "TAG Skywarn", "2026-10-06") == 3,
          "History F3: the upstream has the 3 check-ins")
    program.send("Esc")

    # A look-alike net: declined, then confirmed.
    program.send("Up")
    program.send("F6")
    program.expect("History: Dixie Traders Net", "look-alike: History opens")
    program.send("F3")
    program.expect("Push to Dixie Traders?", "look-alike: asks first")
    program.expect("Its net Dixie", "look-alike: names the upstream net")
    program.send("Esc")
    program.expect("Not pushed. Push it from History (F3) later.", "look-alike: declined")
    check(upstream_check_ins(directory, "Dixie Traders", "2026-10-07") is None,
          "look-alike: declined, so nothing reached the upstream")
    check(uploads_gone(directory), "look-alike: declined, so the upstream's copy is deleted")
    program.send("F3")
    program.expect("Push to Dixie Traders?", "look-alike: asks again")
    program.send("Enter")
    program.expect("Pushed to Dixie Traders on 127.0.0.1.", "look-alike: confirmed and pushed")
    check(upstream_check_ins(directory, "Dixie Traders", "2026-10-07") == 2,
          "look-alike: the upstream has the 2 check-ins")
    program.send("Esc")

    # A net the upstream has nothing like.
    program.send("Up")
    program.send("F6")
    program.expect("History: 220 EOR net", "no match: History opens")
    program.send("F3")
    program.expect("No net named like 220 EOR net on 127.0.0.1.", "no match: push says so")
    check(upstream_check_ins(directory, "220 EOR net", "2026-10-08") is None,
          "no match: nothing reached the upstream")
    check(uploads_gone(directory, 3), "no match: nothing left in the upstream's /imports")
    program.quit()

    print("")
    if failures:
        print("%d push screen check(s) failed" % len(failures))
        sys.exit(1)
    print("push screens: all checks passed")


main()
