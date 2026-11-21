#!/usr/bin/env python3
"""A tool talks only to the emulator it started, ends only that one, and says so when it failed;
and the stand-in's closing shows are the app's.

First a table of closing shows (emulator/game_app.py's GameOnTheSign, no emulator): a game whose
last tick gave a point its last frame did not carry, ended as a new best and as not one. What the
sign gets, from the hit to the end, must be the app's show (energy-sign, JumpGame.kt, GameShows):
the hit's every frame with the FINAL score; for a new best HIGH SCORE in the big font (a chonky
slide, 1 s), never a NEW BEST flash.

Then ONE table of sittings, each run in a terminal of its own (a pseudo-terminal that is the program's
controlling terminal, as a window is). Most are emulator/game_app.py:

  - the emulator never comes to listen for it (it cannot be started; ANOTHER EMULATOR has the
    port): it must end with a status that is not 0, say why, and leave the other emulator alone:
    not connected to, still running afterwards. The same for tests/game_test.py's run.
  - it begins well and ends in the ways there are: q is typed, Ctrl-C is typed (both: status 0,
    the screen put back), the window is closed, it is told to end (SIGTERM), it is killed outright.
    And once started as the README and a launcher start it, with no port named: it finds one
  - its emulator fails as it ends (a recording it cannot finish: status 4), or does not end when
    asked: q is typed, and the status is not 0 all the same
  - it plays a game by itself to a new best (--auto 2 --start) and ends: the sign got HIGH SCORE

After every one of them no emulator of the sitting is left running. Which process that is, the
sitting's emulator says itself (it is started through a script that writes its process number
down): nothing here looks for a process by its name or its command line, or ends one it did not
start.

usage: game_app_test.py <sign-emu> <firmware.hex> <repo root> [port]
Python 3 standard library only. Uses one TCP port (default 14475).
"""
import fcntl
import math
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
import game_app  # noqa: E402
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


class EndsAfter:
    """A game for GameOnTheSign: one frame that says `shown`, then the game is over at `final`, its
    last tick having given a point that frame did not carry (as at the time limit: the stand-in once
    showed 97 on the hit of a game that ended on 98)."""

    def __init__(self, shown, final):
        self.shown, self.final, self.score, self.sent = shown, final, shown, False

    def start(self, now):
        pass

    def frame(self, now):
        if self.sent:
            self.score = self.final
            return None
        self.sent = True
        return game_app.Frame(2, 0, [(1, 1)], stars=17, score=self.shown)


def closing_show(shown, final, best):
    """What the sign gets for a request every 20 ms, from the end of such a game to the end of its
    show. A run of game frames is one entry, [what they show, how many]: those with a score as the
    frame without its colours and effect (all alike: the hit), those without as 'frames, no score'
    (the runner hopping). At 20 ms a request, a beat of frames of t ms is t / 20 of them."""
    sign = game_app.GameOnTheSign(game=EndsAfter(shown, final))
    sign.best = best
    sign.start()
    got, now = [], 0
    while now < 60000:
        message = sign.answer(now)
        if message is None and sign.stage == "NONE":
            break
        if sign.stage == "CLOSING":
            if message["type"] != "G":
                got.append(message)
            else:
                run = re.sub(r"p\d\d|e\d", "", message["str"]) if re.search(r"n\d+$", message["str"]) else "frames, no score"
                if got and isinstance(got[-1], list) and got[-1][0] == run:
                    got[-1][1] += 1
                else:
                    got.append([run, 1])
        now += 20
    return got


def eats_at(column, press=True):
    """A game at its start (30 columns a second) with a treat at `column`, no obstacle near; the
    button is pressed at once (or never). Is the treat eaten, what does the game then give, and in
    how many ms from the press?"""
    game = game_app.JumpGame(rng=game_app.random.Random(7))
    game.start(0)
    game.next_after = 1e9                      # (no obstacle comes)
    game.treat = [float(column), False]
    if press:
        game.jump(0)
    score, now = game.score, 0
    while game.treat is not None and now < 3000:
        now += game.rules.tick_ms
        game.work_out_to(now)
    eaten = game.score == score + game.rules.treat_score
    frames = [game.frame(now + ms) for ms in range(0, 500, 10)]
    sparkle = sum(f.effect == 1 for f in frames) * game.rules.tick_ms
    return eaten, sparkle, all(f.treat is None for f in frames), now


def played(seed, seconds=180):
    """A game played by a perfect jumper (every obstacle cleared, never a treat taken on purpose):
    for each treat, the obstacle it came with, its kind (free or dilemma), the columns it is first
    and last sent at, and where it was in front of the next obstacle when that obstacle started."""
    game = game_app.JumpGame(rng=game_app.random.Random(seed))
    game.start(0)
    treats, sent, now = [], {}, 0
    while game.running and now < seconds * 1000:
        now += 10
        had, dilemmas = game.treat, game.dilemmas
        nearest = [x for kind, x, passed in game.coming if not passed]
        if game.jumped_at < 0 and nearest and min(nearest) <= 8 + game.speed() * game.rules.jump_ms / 2000.0:
            game.jump(now)
        game.work_out_to(now)
        if game.treat is not None and had is None:                    # placed in this tick
            treats.append({"with": game.placed, "dilemma": game.dilemmas > dilemmas, "first": None, "last": None})
        if game.treat is not None and game.placed == treats[-1]["with"] + 1 and "ahead" not in treats[-1]:
            treats[-1]["ahead"] = game.coming[-1][1] - game.treat[0]  # (the next obstacle has just started)
        frame = game.frame(now)
        if frame and frame.treat is not None and treats:
            treats[-1]["first"] = frame.treat if treats[-1]["first"] is None else treats[-1]["first"]
            treats[-1]["last"] = frame.treat
            sent[now] = frame.text()
    return game, treats, sent


def treat_table(log):
    """THE TREAT, as the app has it (es-osx/treat-rules.md): one table. A row is what the rules say
    and whether the stand-in does it."""
    rules = game_app.Rules
    eat = {x: eats_at(x) for x in (8 + n / 10.0 for n in range(181))}     # (every tenth of a column, 8 to 26)
    eaten_at = [x for x, (eaten, _, _, _) in eat.items() if eaten]
    eating = sorted({int(math.floor(x)) for x in eaten_at})
    games = [played(seed) for seed in range(12)]
    treats = [t for _, ts, _ in games for t in ts]
    rows = [
        ("a dilemma's D is round(0.77 x speed - 6.5): 17 at 30, 21 at 36, 26 at 42, 30 at 48, 35 at 54",
         [rules.dilemma_before(s) for s in (30, 36, 42, 48, 54)] == [17, 21, 26, 30, 35],
         [rules.dilemma_before(s) for s in (30, 36, 42, 48, 54)]),
        ("at 30 a second the presses that eat it are those with its left column at 14 to 18, a window of "
         "120 ms (3.6 columns)", eating == list(range(14, 19)) and round(max(eaten_at) - min(eaten_at), 1) == 3.6,
         (eating, min(eaten_at), max(eaten_at))),
        ("a runner that stands never eats it (at any column from 8 to 26)",
         not any(eats_at(x, press=False)[0] for x in eat), ""),
        ("eaten: +3, the sparkle (e1) in the frames of 400 ms from that tick, and no t from then on",
         all(eat[x][1] == rules.sparkle_ms and eat[x][2] for x in eaten_at), sorted({eat[x][1:3] for x in eaten_at})),
        ("one treat in the gap after every 10 to 15 obstacles (the first after obstacle 10 to 15)",
         all(ts and 10 <= ts[0]["with"] <= 15 and all(10 <= b["with"] - a["with"] <= 15 for a, b in zip(ts, ts[1:]))
             for _, ts, _ in games), [[t["with"] for t in ts] for _, ts, _ in games[:3]]),
        ("about a third of them a dilemma (0.2 to 0.47 of %d)" % len(treats),
         0.2 <= sum(t["dilemma"] for t in treats) / float(len(treats)) <= 0.47,
         "%d of %d" % (sum(t["dilemma"] for t in treats), len(treats))),
        ("a free one is 4 (or 5) columns in front of the next obstacle as that obstacle starts, a dilemma one D or D + 1",
         all(t.get("ahead") is not None and (17 <= t["ahead"] <= 36 if t["dilemma"] else 4 <= t["ahead"] <= 5)
             for t in treats), sorted({round(t["ahead"], 1) for t in treats if t.get("ahead") is not None})[:8]),
        ("t is sent from column 44 (its right column at 46) and until it is off the face (t-3 at the last)",
         all(t["first"] == rules.treat_up_to and t["last"] in (-3, -2, -1) for t in treats if t["last"] is not None
             and t["last"] < 0), sorted({(t["first"], t["last"]) for t in treats})[:6]),
        ("t is written after the obstacles, as the app writes it",
         all(re.fullmatch(r"[0-7][01](?:[abc]-?\d+)*t-?\d+(?:e1)?s\d+n\d+", s) for _, _, sent in games
             for s in sent.values()), next(iter(games[0][2].values()), "")),
    ]
    for what, ok, detail in rows:
        check(log, ok, "the treat: %s%s" % (what, "" if ok else ": NOT SO (%s)" % (detail,)))


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    emu, image, root = sys.argv[1:4]
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 14475
    log = []
    # The closing shows, as the app has them (GameShows.over and .newBest): the colours are the
    # app's (score 0xFF0080, crash 0xEF0C1D, runner 0x4F62D2). The hit is 700 ms of the last frame
    # (rise 2, an obstacle b at column 1, the stars at 17) with the final score, 98, where the frame
    # said 97; the runner hops for 2000 ms.
    pink, crash = {"r": 255, "g": 0, "b": 128}, {"str": "X", "type": "I", "r": 239, "g": 12, "b": 29}
    hit = ["20b1s17n98", 700 // 20]
    # what it is; the score the last frame said; the final score; the best before; the show
    shows = [
        ("a new best (it was 0)", 97, 98, 0,
         [hit, crash, dict(pink, str="HIGH SCORE", type="C", dly=1000, scroll=False),
          dict(pink, str="  98  ", type="C", dly=1500, scroll=False), ["frames, no score", 2000 // 20],
          {"str": "", "type": "S", "stars": 600}]),
        ("not a new best (it is 120)", 97, 98, 120,
         [hit, crash, {"str": "GAME\x0bOVER", "type": "F", "time": 200}, dict(pink, str="SCORE 98", type="O", dly=1000),
          {"str": "BEST 120", "type": "O", "r": 79, "g": 98, "b": 210, "dly": 1000}, {"str": "", "type": "S", "stars": 300}]),
    ]
    treat_table(log)
    for what, shown, final, best, expected in shows:
        got = closing_show(shown, final, best)
        check(log, got == expected, "the last tick gave a point, %s: the sign gets the app's show, every frame of "
              "the hit (700 ms) with the final score%s" % (what, "" if got == expected else ": NOT SO, it gets %r" % got))
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
        # (the view's line of what the sign got keeps its last 78 bytes: here all of it but the brace)
        ("it plays a game by itself to a new best and ends (--auto 2 --start)", app + ["--auto", "2", "--start"], emu,
         False, None, True, [score, "A NEW BEST (it was 0)", '"str":"HIGH SCORE","type":"C","r":255,"g":0,"b":128,'
                             '"dly":1000,"scroll":false}']),
    ]
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
