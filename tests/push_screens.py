#!/usr/bin/env python3
"""The Federated Logging push screens, through the real program.

Run by tests/ssh_end_to_end.sh after the other end-to-end tests, against the
upstream it started: Close & Push, the push in History's export window, and the look-alike confirm
prompt, and pulling a net or a net's sessions from the Import page, driven
in a pseudo-terminal and read back through pyte.

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
    "F2": "\x1bOQ", "F3": "\x1bOR", "F4": "\x1bOS", "F6": "\x1b[17~", "F7": "\x1b[18~", "F8": "\x1b[19~", "F9": "\x1b[20~",
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

    def highlight(self, needle, presses=8):
        """Goes to the top of the list, then presses Down until the highlighted row has `needle` in it."""
        self.send(*["Up"] * presses)
        for _ in range(presses):
            marked = [line for line in self.text().split("\n") if line.startswith("\u2502>")]
            if marked and needle in marked[0]:
                return True
            self.send("Down")
        return False

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


def upstream_sessions(directory, net):
    """How many closed sessions the upstream's `net` has."""
    db = sqlite3.connect("file:" + directory + "/up/quicklogger.db?mode=ro", uri=True)
    try:
        return db.execute("SELECT COUNT(*) FROM net_instances i JOIN nets n ON n.id = i.net_id "
                          "WHERE n.name = ? AND i.status = 1", (net,)).fetchone()[0]
    finally:
        db.close()


def upstream_net_exists(directory, net):
    """True if the upstream has a net called `net`."""
    db = sqlite3.connect("file:" + directory + "/up/quicklogger.db?mode=ro", uri=True)
    try:
        return db.execute("SELECT COUNT(*) FROM nets WHERE name = ?", (net,)).fetchone()[0] > 0
    finally:
        db.close()


def uploads_left(directory):
    """The .qlsession and .qlnet uploads in any user's /imports on the upstream."""
    found = []
    for root, _, files in os.walk(directory + "/up"):
        if "imports" in root.split(os.sep):
            found += [f for f in files if f.endswith((".qlsession", ".qlnet"))]
    return found


def uploads_gone(directory, seconds=10):
    """True once no upload is left on the upstream (a declined push's is deleted by a background ssh)."""
    end = time.time() + seconds
    while time.time() < end:
        if not uploads_left(directory):
            return True
        time.sleep(0.2)
    return False


def local_scalar(run_dir, sql, args=()):
    """One value from the local database the program ran on."""
    db = sqlite3.connect("file:" + run_dir + "/quicklogger.db?mode=ro", uri=True)
    try:
        return db.execute(sql, args).fetchone()[0]
    finally:
        db.close()


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
    program.send("Down", "F7", "F3")  # past the open session, to the closed one
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
    program.send("Down", "F7", "F3")
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

    # The older closed session: pushed from the export window.
    program.send("Down", "F7", "F3")
    program.expect("Pushed to TAG Skywarn on 127.0.0.1.", "History export: pushed")
    check(upstream_check_ins(directory, "TAG Skywarn", "2026-10-06") == 3,
          "History export: the upstream has the 3 check-ins")
    program.send("Esc")

    # A look-alike net: declined, then confirmed.
    program.send("Up")
    program.send("F6")
    program.expect("History: Dixie Traders Net", "look-alike: History opens")
    program.send("F7", "F3")
    program.expect("Push to Dixie Traders?", "look-alike: asks first")
    program.expect("Its net Dixie", "look-alike: names the upstream net")
    program.send("Esc")
    program.expect("Not pushed. Push it from History (F7 Export) later.", "look-alike: declined")
    check(upstream_check_ins(directory, "Dixie Traders", "2026-10-07") is None,
          "look-alike: declined, so nothing reached the upstream")
    check(uploads_gone(directory), "look-alike: declined, so the upstream's copy is deleted")
    program.send("F7", "F3")
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
    program.send("F7", "F3")
    program.expect("No net named like 220 EOR net on 127.0.0.1.", "no match: push says so")
    check(upstream_check_ins(directory, "220 EOR net", "2026-10-08") is None,
          "no match: nothing reached the upstream")
    check(uploads_gone(directory, 3), "no match: nothing left in the upstream's /imports")
    program.quit()

    # Pulling: a whole net from the upstream, from the net list's Import page,
    # then the sessions of a net it has more of, from History's.
    program = Program(binary, run_dir)
    program.expect("Recurring Nets", "pull: net list shows")
    program.send("F9")
    program.expect("Import Net", "pull: the Import page opens")
    program.expect("F4  Pull", "pull: the Import page offers Pull")
    program.send("F4")
    program.expect("Select a net to import from 127.0.0.1:", "pull: the upstream's nets are listed")
    concurrent = upstream_sessions(directory, "Concurrent Net")
    program.expect("Concurrent Net  (%d sessions)" % concurrent, "pull: a net's closed sessions are counted")
    program.send("Down", "Enter")  # Concurrent Net, second after Brand New Net, which nothing here is like
    program.expect("Imported \"Concurrent Net\"", "pull: the net is imported")
    check(local_scalar(run_dir, "SELECT COUNT(*) FROM net_instances i JOIN nets n ON n.id = i.net_id "
                                "WHERE n.name = 'Concurrent Net'") == concurrent,
          "pull: the net's %d sessions are here" % concurrent)

    # Cancelled: nothing changes.
    program.send("F9")
    program.send("F4")
    program.expect("Select a net to import from 127.0.0.1:", "pull: the list opens again")
    program.send("Esc")
    program.expect("Import Net", "pull: Esc closes the window")
    program.send("Esc")
    program.expect("Recurring Nets", "pull: back on the net list")

    # The sessions of TAG Skywarn: the upstream has two this history lacks.
    before = local_scalar(run_dir, "SELECT COUNT(*) FROM net_instances i JOIN nets n ON n.id = i.net_id "
                                   "WHERE n.name = 'TAG Skywarn'")
    check(program.highlight("TAG Skywarn"), "pull sessions: TAG Skywarn highlighted")
    program.send("F6")
    program.expect("History: TAG Skywarn", "pull sessions: History opens")
    program.send("F6")
    program.expect("Import Session: TAG Skywarn", "pull sessions: the Import page opens")
    program.send("F4")
    program.expect("Select a net to pull sessions from:", "pull sessions: asks which net")
    program.send("Enter")  # TAG Skywarn is highlighted: it has this net's name
    program.expect("Added 2 sessions of TAG Skywarn from 127.0.0.1; 2 here already.", "pull sessions: two added")
    program.expect("History: TAG Skywarn", "pull sessions: back in History")
    check(local_scalar(run_dir, "SELECT COUNT(*) FROM net_instances i JOIN nets n ON n.id = i.net_id "
                                "WHERE n.name = 'TAG Skywarn'") == before + 2,
          "pull sessions: the History has the two")
    check("2026-09-15" in program.text(), "pull sessions: History lists a pulled session")
    check(not os.path.exists(run_dir + "/imports/.pull"), "pull sessions: the fetch folder is gone")

    # Again: nothing new.
    program.send("F6")
    program.send("F4")
    program.expect("Select a net to pull sessions from:", "pull again: asks which net")
    program.send("Enter")
    program.expect("Nothing new:", "pull again: nothing new")
    program.quit()

    # Pushing a whole net: F8 Export offers it. A net the upstream lacks is
    # added; one it has asks before merging.
    program = Program(binary, run_dir)
    program.expect("Recurring Nets", "push net: net list shows")
    check(program.highlight("220 EOR net"), "push net: 220 EOR net highlighted")
    program.send("F8")
    program.expect("Saved to:", "push net: the export window opens")
    program.expect("Push to upstream", "push net: it offers a push")
    program.send("F3")
    program.expect("Pushed to 220 EOR net on 127.0.0.1.", "push net: pushed")
    check(upstream_net_exists(directory, "220 EOR net"), "push net: the upstream has the net")

    check(program.highlight("TAG Skywarn"), "push net: TAG Skywarn highlighted")
    program.send("F8")
    program.expect("Push to upstream", "push net: offers a push again")
    program.send("F3")
    program.expect("Merge Into TAG Skywarn?", "push net: asks before merging")
    program.expect("127.0.0.1 has TAG Skywarn already.", "push net: says it has the net")
    program.send("Enter")
    program.expect("Merged into TAG Skywarn on 127.0.0.1", "push net: merged")

    # Declined: nothing changes, and the upload is deleted.
    program.send("F8")
    program.send("F3")
    program.expect("Merge Into TAG Skywarn?", "push net: asks again")
    program.send("Esc")
    program.expect("Not pushed. Push it again with F8 Export.", "push net: declined")
    check(uploads_gone(directory), "push net: declined, so the upstream's copy is deleted")
    program.quit()

    print("")
    if failures:
        print("%d push screen check(s) failed" % len(failures))
        sys.exit(1)
    print("push screens: all checks passed")


main()
