#!/usr/bin/env python3
"""emulator/game_app.py says so when its emulator is not there for it, and never leaves it running.

ONE table of sittings, each run in a terminal of its own (a pseudo-terminal that is the program's
controlling terminal, as a window is):

  - two in which the emulator never comes to listen for game_app.py (it cannot be started; another
    program has the port): game_app.py must end with a status that is not 0, say why, and have
    connected to nobody: whoever else listens on the port is not its emulator
  - four that begin well and end in the four ways there are: q is typed (status 0, so the status
    means something), the window is closed, the program is told to end (SIGTERM), the program is
    killed outright

After every one of them no emulator that game_app.py started is left running.

usage: game_app_test.py <sign-emu> <firmware.hex> <repo root> [port]
Python 3 standard library only. Uses one TCP port (default 14475).
"""
import fcntl
import os
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ads_test as A  # noqa: E402

check = A.check
REDRAWN = b"\x1b[J"          # sign-emu ends every redraw of its view with this


def its_terminal():
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


def sitting(command, how, seconds=60):
    """Runs the command in a terminal of 130 x 36 and, when the sign has been drawn, ends it:
    `how` is q (the key), close (the window is closed), or a signal's number. Returns its exit
    status (None when it had to be stopped here) and what it wrote, as plain text."""
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 36, 130, 0, 0))
    child = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
                             preexec_fn=its_terminal)
    os.close(slave)
    seen, done, end = bytearray(), False, time.monotonic() + seconds
    while time.monotonic() < end and master is not None:
        if select.select([master], [], [], 0.05)[0]:
            try:
                data = os.read(master, 65536)
            except OSError:
                data = b""
            if not data:
                break
            seen += data
            if not done and REDRAWN in seen:
                done = True
                if how == "q":
                    os.write(master, b"q")
                elif how == "close":
                    os.close(master)
                    master = None
                else:
                    os.kill(child.pid, how)
        elif child.poll() is not None:
            break
    try:
        status = child.wait(10)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()
        status = None
    if master is not None:
        os.close(master)
    return status, re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", seen.decode("utf-8", "replace"))


def emulators(port):
    """The process numbers of the emulators that were started for this port."""
    listing = subprocess.run(["ps", "-axo", "pid=,command="], capture_output=True, text=True).stdout
    return [int(line.split()[0]) for line in listing.splitlines()
            if " --port %d --wait-client " % port in line and "game_app.py" not in line]


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    emu, image, root = sys.argv[1:4]
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 14475
    app = [sys.executable, os.path.join(root, "emulator", "game_app.py"), "--hex", image, "--port", str(port)]
    listen, score = "did not come to listen", "best score of this sitting"
    # what it is; the emulator it is given; is another program on the port; how it is ended; is its
    # status 0; what it must say
    table = [
        ("the emulator cannot be started", "/usr/bin/false", False, "q", False, listen),
        ("another program has the port", emu, True, "q", False, listen),
        ("the emulator is there, and q is typed", emu, False, "q", True, score),
        ("the emulator is there, and the window is closed", emu, False, "close", False, None),
        ("the emulator is there, and it is told to end (SIGTERM)", emu, False, signal.SIGTERM, False, None),
        ("the emulator is there, and it is killed outright", emu, False, signal.SIGKILL, False, None),
    ]
    log = []
    for what, its_emu, taken, how, well, says in table:
        other = None
        if taken:
            other = socket.socket()
            # (a port that was in use a moment ago can be had again; two listeners cannot have it)
            other.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            other.bind(("127.0.0.1", port))
            other.listen(4)
            other.settimeout(0.5)
        status, text = sitting(app + ["--emu", its_emu], how)
        connected = False
        if other:
            try:
                other.accept()[0].close()
                connected = True
            except OSError:
                pass
            other.close()
        end = time.monotonic() + 5                   # (an emulator whose app has gone ends by itself: a moment)
        while emulators(port) and time.monotonic() < end:
            time.sleep(0.1)
        left = emulators(port)
        for pid in left:                             # (this test leaves none behind either)
            os.kill(pid, signal.SIGKILL)
        said = says is None or says in text
        check(log, (status == 0) == well and status is not None and said and not connected and not left,
              "%s: it ends with status %s (%s)%s%s, and %s"
              % (what, status, "0" if well else "not 0",
                 "" if says is None else ", says \"%s\"" % (says if said else "... NOT: " + text.strip()[-160:]),
                 "" if not taken else ", has %s to the other program" % ("CONNECTED" if connected else "not connected"),
                 "no emulator of its is left running" if not left else "ITS EMULATOR WAS LEFT RUNNING"))
    print("\n".join(log))
    if A.failures:
        print("FAILED: %d check(s)" % len(A.failures))
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
