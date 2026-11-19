#!/usr/bin/env python3
"""A tool talks only to the emulator it started, ends only that one, and says so when it failed.

ONE table of sittings, each run in a terminal of its own (a pseudo-terminal that is the program's
controlling terminal, as a window is). Most are emulator/game_app.py:

  - the emulator never comes to listen for it (it cannot be started; ANOTHER EMULATOR has the
    port): it must end with a status that is not 0, say why, and leave the other emulator alone:
    not connected to, still running afterwards. The same for tests/game_test.py's run.
  - it begins well and ends in the ways there are: q is typed, Ctrl-C is typed (both: status 0,
    the screen put back), the window is closed, it is told to end (SIGTERM), it is killed outright.
    And once started as the README and a launcher start it, with no port named: it finds one
  - its emulator fails as it ends (a recording it cannot finish: status 4), or does not end when
    asked: q is typed, and the status is not 0 all the same

After every one of them no emulator of the sitting is left running. Which process that is, the
sitting's emulator says itself (it is started through a script that writes its process number
down): nothing here looks for a process by its name or its command line, or ends one it did not
start.

usage: game_app_test.py <sign-emu> <firmware.hex> <repo root> [port]
Python 3 standard library only. Uses one TCP port (default 14475).
"""
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))
import ads_test as A  # noqa: E402
import sign_emu  # noqa: E402

check = A.check
REDRAWN = b"\x1b[J"                  # sign-emu ends every redraw of its view with this
SCREEN_BACK = "\x1b[?25h\x1b[?1049l"  # ... and leaves its screen with this: the cursor, the window's own text

# The emulator of a sitting: it writes down which process it is, then is the program the row names.
ITS_EMULATOR = '#!/bin/sh\necho $$ > "$GAME_APP_TEST_PID"\nexec "$GAME_APP_TEST_EMU" "$@"\n'
# An emulator that listens, takes its app and draws, and when asked to end (SIGINT) either ends with
# status 4, as sign-emu does when it cannot finish a recording, or does not end.
BAD_EMULATOR = """#!%s
import os, signal, socket, sys, time
port = int(sys.argv[sys.argv.index("--port") + 1])
stays = os.environ["GAME_APP_TEST_EMU"].endswith("stays")
signal.signal(signal.SIGINT, signal.SIG_IGN if stays else lambda *a: os._exit(4))
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", port))
listener.listen(1)
sys.stderr.write("sign-emu: waiting for an app on tcp 127.0.0.1:%%d ...\\n" %% port)
sys.stderr.flush()
app = listener.accept()[0]
while True:
    sys.stdout.write("\\x1b[H\\x1b[0m(a sign)\\x1b[J")
    sys.stdout.flush()
    time.sleep(0.1)
""" % sys.executable
# tests/game_test.py's run, as a program of its own: tests dir, emulator, image, work dir, port
ITS_RUN = ("import sys; sys.path.insert(0, sys.argv[1]); import game_test as G; "
           "G.Run({'emu': sys.argv[2], 'hex': sys.argv[3], 'work': sys.argv[4]}, 'taken', [b'{}'], int(sys.argv[5]), 5)")


def its_terminal():
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


def sitting(command, how, env, seconds=60):
    """Runs the command in a terminal of 130 x 36 and, when the sign has been drawn, ends it:
    `how` is q or ctrl-c (the key), close (the window is closed), a signal's number, or None (it
    ends by itself). Returns its exit status (None when it had to be stopped here) and what it
    wrote, as it wrote it."""
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 36, 130, 0, 0))
    child = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
                             preexec_fn=its_terminal, env=env)
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
            if not done and REDRAWN in seen and how is not None:
                done = True
                if how in ("q", "ctrl-c"):
                    os.write(master, b"q" if how == "q" else b"\x03")
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
    return status, seen.decode("utf-8", "replace")


def runs(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    emu, image, root = sys.argv[1:4]
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 14475
    work = tempfile.mkdtemp(prefix="game_app_test.")
    its, fails, stays, its_pid = (os.path.join(work, n) for n in ("emu", "fails", "stays", "pid"))
    for path, text in ((its, ITS_EMULATOR), (fails, BAD_EMULATOR), (stays, BAD_EMULATOR)):
        with open(path, "w") as f:
            f.write(text)
        os.chmod(path, 0o755)
    app = [sys.executable, os.path.join(root, "emulator", "game_app.py"), "--hex", image, "--port", str(port), "--emu", its]
    no_port = [a for a in app if a not in ("--port", str(port))]
    run = [sys.executable, "-c", ITS_RUN, os.path.dirname(os.path.abspath(__file__)), its, image, work, str(port)]
    listen, score = "did not come to listen", "best score of this sitting"
    # what it is; the program; the emulator it gets; is another emulator on the port; how it is
    # ended; is its status 0; what it must say
    table = [
        ("the emulator cannot be started", app, "/usr/bin/false", False, "q", False, [listen]),
        ("another emulator has the port", app, emu, True, "q", False, [listen]),
        ("game_test.py's run, and another emulator has the port", run, emu, True, None, False, [listen]),
        ("the emulator is there, and q is typed", app, emu, False, "q", True, [score, SCREEN_BACK]),
        ("the emulator is there, and Ctrl-C is typed", app, emu, False, "ctrl-c", True, [score, SCREEN_BACK]),
        ("no port is named (it finds a free one), and q is typed", no_port, emu, False, "q", True, [score, SCREEN_BACK]),
        ("the emulator is there, and the window is closed", app, emu, False, "close", False, []),
        ("the emulator is there, and it is told to end (SIGTERM)", app, emu, False, signal.SIGTERM, False, []),
        ("the emulator is there, and it is killed outright", app, emu, False, signal.SIGKILL, False, []),
        ("q is typed, and the emulator fails as it ends", app, fails, False, "q", False, ["sign-emu ended with status 4"]),
        ("q is typed, and the emulator does not end", app, stays, False, "q", False, ["sign-emu was ended by signal 9"]),
    ]
    log = []
    for what, command, its_emu, taken, how, well, says in table:
        other = None
        if taken:        # somebody else's emulator, waiting for its own app
            other = sign_emu.Emu([emu, "--headless", "--port", str(port), "--wait-client", "120", "--until-disconnect",
                                  "--duration", "1", image], port, log=os.path.join(work, "other.log"))
        if os.path.exists(its_pid):
            os.remove(its_pid)
        status, text = sitting(command, how, dict(os.environ, GAME_APP_TEST_PID=its_pid, GAME_APP_TEST_EMU=its_emu))
        text += re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)
        pid = int(open(its_pid).read()) if os.path.exists(its_pid) else None
        end = time.monotonic() + 5                   # (an emulator whose app has gone ends by itself: a moment)
        while pid and runs(pid) and time.monotonic() < end:
            time.sleep(0.1)
        left = bool(pid) and runs(pid)
        if left:
            os.kill(pid, signal.SIGKILL)             # (the sitting's own, by its number: this test leaves none behind)
        alone = True
        if other:
            time.sleep(1)                            # (an emulator that had an app and lost it ends: a moment)
            alone = other.poll() is None             # still waiting for its app: nobody connected, nobody ended it
            other.end()
        missing = [s for s in says if s not in text]
        check(log, (status == 0) == well and status is not None and not missing and alone and not left,
              "%s: it ends with status %s (%s)%s%s, and %s"
              % (what, status, "0" if well else "not 0",
                 "".join(", %s %r" % ("DOES NOT SAY" if s in missing else "says", s) for s in says),
                 "" if not taken else ", the other emulator %s" % (
                     "is still waiting for its own app" if alone else "WAS CONNECTED TO OR ENDED"),
                 "no emulator of its is left running" if not left else "ITS EMULATOR WAS LEFT RUNNING"))
    shutil.rmtree(work, ignore_errors=True)
    print("\n".join(log))
    if A.failures:
        print("FAILED: %d check(s)" % len(A.failures))
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
