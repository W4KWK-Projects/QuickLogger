#!/usr/bin/env python3
"""Restricted-nets mode, through real SSH logins to the real program.

Run by tests/ssh_end_to_end.sh against the restricted server it started
(users N4FUL, a full user given Tuesday Net; N4NET, a Net Admin given Tuesday
Net; N4ADM, an Admin; Friday Net is nobody's but the Admin's). Each user is
a real `ssh -tt` login in a pseudo-terminal, 80 columns by 24 rows, read back
through pyte. The point: what each level sees and may do, and that a change
the Admin makes in one session shows in another session's very next action.

  restricted_screens.py <run-dir> <port>

Needs pyte (pip install pyte); tests/ssh_end_to_end.sh skips this when it
isn't installed. Python does no networking here, only the pseudo-terminal:
ssh is the system's, with the run's key and known_hosts (the ssh wrapper in
PATH).
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
    "F2": "\x1bOQ", "F3": "\x1bOR", "F4": "\x1bOS", "F5": "\x1b[15~", "F6": "\x1b[17~", "F7": "\x1b[18~",
    "F8": "\x1b[19~", "F9": "\x1b[20~", "Esc": "\x1b", "Enter": "\r", "Down": "\x1b[B", "Up": "\x1b[A",
}
COLS = int(os.environ.get("QL_SCREEN_COLS", "80"))
ROWS = int(os.environ.get("QL_SCREEN_ROWS", "24"))
failures = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


class Login:
    """One user's ssh login, with a pyte screen."""

    def __init__(self, user, port):
        self.user = user
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execvp("ssh", ["ssh", "-tt", "-p", port, user + "@127.0.0.1"])
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
        check(found, self.user + ": " + what)
        if not found:
            print("--- screen:\n" + self.text() + "\n---")
        return found

    def lacks(self, needle, what):
        absent = needle not in self.text()
        check(absent, self.user + ": " + what)
        if not absent:
            print("--- screen:\n" + self.text() + "\n---")
        return absent

    def highlight(self, needle, presses=8):
        """Goes to the top of the list, then presses Down until the highlighted row has `needle` in it."""
        self.send(*["Up"] * presses)
        for _ in range(presses):
            marked = [line for line in self.text().split("\n") if line.startswith("│>")]
            if marked and needle in marked[0]:
                return True
            self.send("Down")
        return False

    def quit(self):
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)


def row_number(login, net):
    """The number the pick prompt would show beside `net` (its place in the list, from 1)."""
    names = []
    for line in login.text().split("\n"):
        if line.startswith("│") and ("Amateur" in line or "GMRS" in line) and "Type" not in line:
            names.append(line[1:].strip(" >").split("  ")[0].strip())
    return names.index(net) + 1 if net in names else 0


def scalar(run_dir, sql, args=()):
    db = sqlite3.connect("file:" + run_dir + "/restricted/quicklogger.db?mode=ro", uri=True)
    try:
        return db.execute(sql, args).fetchone()[0]
    finally:
        db.close()


def main():
    directory, port = sys.argv[1], sys.argv[2]

    # A full user: Tuesday is theirs, Friday isn't.
    full = Login("N4FUL", port)
    full.expect("Operating as N4FUL", "logs in to the net list")
    full.highlight("Friday Net")
    full.expect("Not one of your nets", "Friday says it isn't theirs")
    full.expect("F3/Enter  View", "Friday's key is View")
    full.lacks("F2  New", "no New key")
    full.lacks("F7", "no Edit key on Friday")
    full.send("F3")
    full.expect("isn't one of your nets", "F3 on Friday only watches")
    full.send("F2")
    full.expect("Only Net Admins can add a new net.", "F2 is refused")
    full.highlight("Tuesday Net")
    full.expect("F3/Enter  Log Net", "Tuesday's key is Log Net")
    full.expect("F7  Stations", "Tuesday's F7 is Stations")
    number = row_number(full, "Tuesday Net")
    check(number > 0, "N4FUL: found Tuesday Net's number")
    full.send("F7", str(number), "Enter")
    full.expect("Only a Net Admin changes these", "Edit Net shows Tuesday read-only")
    full.expect("F6  Add Station", "Edit Net offers Add Station")
    full.lacks("F8  Del Net", "no Del Net key")
    full.send("Esc")
    full.expect("Operating as N4FUL", "back on the net list")

    # A Net Admin: Tuesday in full, Friday not theirs; Net Access has only Tuesday.
    net_admin = Login("N4NET", port)
    net_admin.expect("(Net Admin)", "the net list says Net Admin")
    net_admin.highlight("Tuesday Net")
    net_admin.expect("F7  Edit", "Tuesday's F7 is Edit")
    net_admin.expect("F2  New", "New is there")
    net_admin.highlight("Friday Net")
    net_admin.expect("Not one of your nets", "Friday isn't theirs")
    net_admin.send("F4", "F5")
    net_admin.expect("Choose a net:", "Net Access opens from Settings F5")
    net_admin.expect("Tuesday Net", "it lists Tuesday")
    net_admin.lacks("Friday Net", "it doesn't list Friday")
    net_admin.send("Esc", "Esc")
    net_admin.expect("Recurring Nets", "back on the net list")

    # The Admin gives Friday to N4FUL while their session is open.
    admin = Login("N4ADM", port)
    admin.expect("(Admin)", "the net list says Admin")
    admin.send("F4", "F5")
    admin.expect("Manage Users", "Manage Users opens from Settings F5")
    admin.expect("F8  Restricted: On", "Restricted is on")
    admin.send("F7")
    admin.expect("Choose a net:", "Net Access opens")
    admin.expect("Friday Net", "it lists Friday")
    admin.send("Enter")
    admin.expect("Who has Friday Net", "Friday's people")
    admin.expect("[ ] N4FUL", "N4FUL doesn't have it yet")
    admin.send("Enter")
    admin.expect("N4FUL has Friday Net", "N4FUL is given Friday")
    check(scalar(directory, "SELECT COUNT(*) FROM net_grants WHERE username = 'N4FUL' AND net_id = "
                            "(SELECT id FROM nets WHERE name = 'Friday Net')") == 1, "the grant is in the database")

    # Their very next action sees it: no new login.
    full.send("Up", "Up", "Up")
    full.highlight("Friday Net")
    full.send("F3")
    full.expect("Select Your Role", "N4FUL logs Friday now")
    full.send("Esc")
    full.expect("Operating as N4FUL", "back on the net list")

    # The Admin takes it back; the next action refuses.
    admin.send("Enter")
    admin.expect("N4FUL no longer has Friday Net", "N4FUL has Friday taken away")
    full.highlight("Friday Net")
    full.send("F3")
    full.expect("isn't one of your nets", "Friday is refused again")

    # Restricted off: a full user may add a net.
    admin.send("Esc", "Esc")
    admin.expect("F8  Restricted: On", "the Admin is back on Manage Users")
    admin.send("F8")
    admin.expect("Restricted off", "Restricted goes off")
    full.send("F2")
    full.expect("New Recurring Net", "N4FUL's New works with Restricted off")
    full.send("Esc")
    admin.send("F8")
    admin.expect("Restricted on", "Restricted goes back on")
    full.expect("Recurring Nets", "N4FUL is back on the net list")
    full.send("F2")
    full.expect("Only Net Admins can add a new net.", "New is refused again")

    # The Key Log has it all, under the Admin's name.
    admin.send("F6")
    admin.expect("Key Log", "the Key Log opens")
    admin.expect("net given", "it shows the net given")
    admin.expect("restricted off", "it shows Restricted off")
    check(scalar(directory, "SELECT COUNT(*) FROM key_log WHERE actor = 'N4ADM'") >= 4,
          "the Key Log has the Admin's actions under their name")

    for login in (full, net_admin, admin):
        login.quit()

    if failures:
        print("\n%d check(s) failed." % len(failures))
        sys.exit(1)


main()
