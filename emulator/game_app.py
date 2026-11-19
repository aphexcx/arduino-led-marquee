#!/usr/bin/env python3
"""Play the jump game on the emulated sign, in one terminal window.

This is a STAND-IN for the Android app's side of the game: the app's rules and its three shows
(energy-sign, JumpGame.kt: the 900 ms jump, 30 rising to 54 columns a second, the three obstacle
kinds and their spacing, the collisions LED for LED), written again here so that the game can be
played without the app. The sign's side is the real firmware image, running in sign-emu.

It starts sign-emu with its live view, answers every request of the sign itself and reads the
keys in the same window:

    g or Enter         start a game (its intro first)
    SPACE or up arrow  jump
    q or Ctrl-C        quit

The game goes by this computer's clock, not by the sign's requests, as the app's does, so the
emulator runs at real-time speed. The window must be 124 columns by 32 rows or larger.

usage: game_app.py [--emu sign-emu] [--hex firmware.hex] [--port n] [--auto n] [-- sign-emu options]
  --auto n   press the button itself for the first n obstacles of every game (a demo, a recording:
             game_app.py --auto 4 -- --record game.gif)
Python 3 standard library only. Not on Windows (it needs a pseudo-terminal).
"""
import argparse
import json
import math
import os
import pty
import random
import select
import signal
import socket
import subprocess
import sys
import termios
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sign_emu  # noqa: E402  (the one way an emulator is started, connected to and ended here)
VT = "\u000b"


# ---- the app's game (JumpGame.kt), in ticks of elapsed time -----------------------------------------
class Rules:
    face_columns = 60
    rows = 7
    runner_at = 2
    # what a shape lights, a number a column from the left; bit 0 is the bottom row
    runner = [0b0000001, 0b0000011, 0b0000111, 0b0001101, 0b0001011, 0b0001110, 0b0001101,
              0b0011001, 0b0110011, 0b1100110, 0b1001100, 0b1001000, 0b0001000]
    obstacles = [[0b1, 0b11, 0b1], [0b111, 0b111], [0b1, 0b11, 0b111, 0b11, 0b1]]
    jump_rows = 9.0             # the top of a jump, higher than the display
    jump_ms = 900
    start_speed = 30.0          # columns a second
    top_speed = 54.0
    faster_every_second = 0.4
    reaction_ms = 450           # between two obstacles: a whole jump, this, and up to `spread` columns
    spread = 20
    first_after_columns = 18
    tick_ms = 10
    longest_ms = 180000
    stride_ms = 160


class Frame:
    def __init__(self, rise, picture, obstacles, runner_at=None, colours=None, effect=0):
        self.rise, self.picture, self.obstacles = rise, picture, obstacles
        self.runner_at, self.colours, self.effect = runner_at, colours, effect

    def text(self):
        """The frame as the sign reads it (the firmware's GameFrame.h)."""
        out = "%d%d" % (min(max(self.rise, 0), 7), min(max(self.picture, 0), 1))
        for kind, column in [o for o in self.obstacles if 0 <= o[0] <= 2 and -99 <= o[1] <= 99][:4]:
            out += "abc"[kind] + str(column)
        if self.runner_at is not None:
            out += "r%d" % min(max(self.runner_at, -99), 99)
        if self.colours is not None:
            out += "p%d%d" % tuple(min(max(c, 0), 7) for c in self.colours)
        if self.effect in (1, 2):
            out += "e%d" % self.effect
        return out


class JumpGame:
    def __init__(self, rules=Rules, rng=random):
        self.rules, self.rng = rules, rng
        self.score, self.ended, self.running = 0, None, False
        self.started_at = self.ticks = 0
        self.jumped_at = -1
        self.coming = []            # [kind, x, passed]
        self.next_after = 0.0

    def start(self, now):
        self.running, self.ended, self.score = True, None, 0
        self.started_at, self.ticks, self.jumped_at = now, 0, -1
        self.coming = []
        self.next_after = float(self.rules.first_after_columns)

    def jump(self, now):
        self.work_out_to(now)
        if not self.running or self.jumped_at >= 0:
            return False
        self.jumped_at = self.ticks
        return True

    def frame(self, now):
        self.work_out_to(now)
        if not self.running:
            return None
        r = self.rules
        stride = 0 if self.jumped_at >= 0 else self.ticks * r.tick_ms // r.stride_ms % 2
        return Frame(min(self.rise(), r.rows), stride,
                     [(kind, math.floor(x)) for kind, x, _ in self.coming if x < r.face_columns])

    def speed(self):
        r = self.rules
        return min(r.top_speed, r.start_speed + r.faster_every_second * self.ticks * r.tick_ms / 1000.0)

    def work_out_to(self, now):
        while self.running and (self.ticks + 1) * self.rules.tick_ms <= now - self.started_at:
            self.tick()

    def tick(self):
        r = self.rules
        self.ticks += 1
        elapsed = self.ticks * r.tick_ms
        if self.jumped_at >= 0 and (self.ticks - self.jumped_at) * r.tick_ms >= r.jump_ms:
            self.jumped_at = -1                                   # landed
        speed = self.speed()
        moved = speed * r.tick_ms / 1000.0
        for obstacle in self.coming:
            obstacle[1] -= moved
            if not obstacle[2] and obstacle[1] + len(r.obstacles[obstacle[0]]) <= r.runner_at:
                obstacle[2] = True
                self.score += 1
        self.coming = [o for o in self.coming if o[1] + len(r.obstacles[o[0]]) > 0]
        self.next_after -= moved
        if self.next_after <= 0:
            self.coming.append([self.rng.randrange(len(r.obstacles)), float(r.face_columns), False])
            self.next_after = (speed * (r.jump_ms + r.reaction_ms) / 1000.0 + len(r.runner)
                               + self.rng.randrange(r.spread + 1))
        up = self.rise()
        for kind, x, _ in self.coming:
            left = math.floor(x)
            for i, lit in enumerate(r.obstacles[kind]):
                column = left + i - r.runner_at
                if 0 <= column < len(r.runner) and (r.runner[column] << up) & lit:
                    self.running, self.ended = False, "HIT"
                    return
        if elapsed >= r.longest_ms:
            self.running, self.ended = False, "TOO_LONG"

    def rise(self):
        if self.jumped_at < 0:
            return 0
        part = (self.ticks - self.jumped_at) * self.rules.tick_ms / float(self.rules.jump_ms)
        return math.floor(4 * self.rules.jump_rows * part * (1 - part) + 1e-9)


# ---- the app's three shows (GameShows) and what the sign gets for each request (GameOnTheSign) ------
def colour_message(kind, text, colour, **more):
    out = {"str": text, "type": kind, "r": colour >> 16 & 255, "g": colour >> 8 & 255, "b": colour & 255}
    out.update(more)
    return out


class Shows:
    """A show is a list of steps: a message of a kind the sign has, shown once; or (milliseconds,
    function of the time since the step began giving a Frame)."""
    runner_colour, crash_colour, score_colour = 0x4F62D2, 0xEF0C1D, 0xFF0080      # the app's resources

    def flash(self, text):
        return {"str": text, "type": "F", "time": 200}

    def intro(self):
        return [
            colour_message("I", "D", self.runner_colour),
            colour_message("C", "READY?", self.score_colour, dly=600, scroll=False),
            self.flash("JUMP!"),
            (500, lambda t: Frame(0, t // 100 % 2, [], runner_at=-12 + 14 * t // 500, effect=1)),
        ]

    def hit(self, last):
        return (700, lambda t: Frame(last.rise if last else 0, 0, last.obstacles if last else [],
                                     colours=(2 if t // 100 % 2 == 0 else 0, 2), effect=2 if t < 100 else 0))

    def over(self, score, best, last):
        return [
            self.hit(last),
            colour_message("I", "X", self.crash_colour),
            self.flash("GAME" + VT + "OVER"),
            colour_message("O", "SCORE %d" % score, self.score_colour, dly=1000),
            colour_message("O", "BEST %d" % best, self.runner_colour, dly=1000),
            {"str": "", "type": "S", "stars": 300},
        ]

    def new_best(self, score, last):
        digits = str(score)
        return [
            self.hit(last),
            colour_message("I", "X", self.crash_colour),
            self.flash("NEW" + VT + "BEST!"),
            colour_message("C", digits.rjust((6 + len(digits)) // 2).ljust(6), self.score_colour, dly=1500, scroll=False),
            (2000, lambda t: Frame(int(abs(math.sin(t / 140.0)) * 3), 0, [], runner_at=2 + 45 * t // 2000,
                                   colours=(1 + t // 120 % 7, 2), effect=1)),
            {"str": "", "type": "S", "stars": 600},
        ]


class GameOnTheSign:
    def __init__(self, shows=None, game=None):
        self.shows, self.game = shows or Shows(), game or JumpGame()
        self.stage = "NONE"                       # NONE, INTRO, PLAY, CLOSING
        self.show, self.beat_began, self.last, self.best = [], -1, None, 0
        self.pressed_at, self.press_ms = -1, None
        self.said = "no game yet"

    def start(self):
        if self.stage in ("INTRO", "PLAY"):
            return False
        self.show, self.beat_began, self.stage = self.shows.intro(), -1, "INTRO"
        self.said = "the intro"
        return True

    def jump(self, now):
        if self.stage == "PLAY" and self.game.jump(now):
            self.pressed_at = now

    def answer(self, now):
        """What the sign gets for a request at `now` (a dictionary to send as JSON); None when
        the game has nothing for it."""
        if self.stage == "PLAY":
            frame = self.game.frame(now)
            if frame is not None:
                if self.pressed_at >= 0:
                    self.press_ms = now - self.pressed_at
                self.pressed_at, self.last = -1, frame
                return {"str": frame.text(), "type": "G"}
            score = self.game.score
            is_best = score > self.best
            self.said = "game over: score %d, %s" % (score, "A NEW BEST (it was %d)" % self.best if is_best
                                                     else "best %d" % self.best)
            self.show = self.shows.new_best(score, self.last) if is_best else self.shows.over(score, self.best, self.last)
            self.best = max(self.best, score)
            self.beat_began, self.stage, self.pressed_at = -1, "CLOSING", -1
        while self.stage != "NONE":
            if not self.show:
                if self.stage == "INTRO":
                    self.stage, self.last = "PLAY", None
                    self.game.start(now)
                    return self.answer(now)
                self.stage = "NONE"
            elif isinstance(self.show[0], dict):
                return self.show.pop(0)
            else:
                ms, at = self.show[0]
                if self.beat_began < 0:
                    self.beat_began = now
                if now - self.beat_began < ms:
                    return {"str": at(now - self.beat_began).text(), "type": "G"}
                self.show.pop(0)
                self.beat_began = -1
        return None

    def status(self):
        if self.stage == "PLAY":
            press = "" if self.press_ms is None else "   last press: in the frame sent %d ms later" % self.press_ms
            return "PLAYING   score %d   best %d   %.0f columns a second%s" % (
                self.game.score, self.best, self.game.speed(), press)
        return self.said + ("" if self.stage != "NONE" else "   (g or Enter starts a game)")


# ---- the window: sign-emu's live view with two lines of ours on top, and the keys --------------------
IDLE = {"str": "", "type": "S", "stars": 20}       # what a request gets when no game is on
HOME = b"\x1b[H\x1b[0m"                             # sign-emu begins every redraw with this ...
END = b"\x1b[J"                                     # ... and ends it with this


def now_ms():
    return int(time.monotonic() * 1000)


class Ended(Exception):
    """The window was closed (SIGHUP) or this program was told to end (SIGTERM)."""


def on_signal(number, frame):
    raise Ended("ended by signal %d" % number)


def leave(emu, master, pending):
    """THE way out, whatever ended the sitting (q, Ctrl-C, the window closed, a signal, an error).
    The emulator this program started is asked to end, so that it leaves its screen and says what
    it ran; it is ended outright when it has not done so within 3 s; and it is waited for. Gives
    what it still wrote, and its status as it is once the process is gone: not 0 when it failed
    (a recording it could not write) or had to be ended here."""
    try:
        emu.wait(0.3)                               # (its app or its screen has just gone: is it ending by itself?)
    except subprocess.TimeoutExpired:
        emu.proc.send_signal(signal.SIGINT)
    end = time.monotonic() + 3
    while time.monotonic() < end:
        if not select.select([master], [], [], 0.2)[0]:
            if emu.poll() is not None:
                break
            continue
        try:
            data = os.read(master, 65536)
        except OSError:
            break
        if not data:
            break
        pending += data
    return pending, emu.end()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--emu", default=os.path.join(HERE, "build", "sign-emu"))
    p.add_argument("--hex", default=os.path.join(HERE, "build", "firmware", "LED_Marquee.hex"))
    p.add_argument("--port", type=int, default=0, help="TCP port for the serial link (default: a free one)")
    p.add_argument("--auto", type=int, default=0, help="press the button itself for the first n obstacles of a game")
    p.add_argument("--start", action="store_true", help="start a game at once (with --auto: that one game, then it ends: a recording)")
    p.add_argument("emu_options", nargs="*", help="after --: more options for sign-emu (--record out.gif)")
    args = p.parse_args()
    for path in (args.emu, args.hex):
        if not os.path.isfile(path):
            sys.exit("game_app: %s is not there (make -C emulator builds both)" % path)
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        sys.exit("game_app: this needs a terminal window (it draws the sign and reads the keys in it)")
    size = os.get_terminal_size()
    if size.columns < 124 or size.lines < 32:
        print("game_app: this window is %d x %d; the sign needs 124 x 32. Make it larger and start again, or go on\n"
              "and see a part of it (starting in 4 s)." % (size.columns, size.lines))
        time.sleep(4)
    port = args.port
    if not port:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]

    master, slave = pty.openpty()
    termios.tcsetwinsize(slave, (size.lines, size.columns)) if hasattr(termios, "tcsetwinsize") else None
    keys = sys.stdin.fileno()
    saved = termios.tcgetattr(keys)
    out = sys.stdout.buffer
    sign = GameOnTheSign()
    emu, sock, pending, report, status = None, None, bytearray(), b"", 0

    def said():
        """All the emulator has written to its screen so far."""
        try:
            while select.select([master], [], [], 0)[0]:
                data = os.read(master, 65536)
                if not data:
                    break
                pending.extend(data)
        except OSError:
            pass
        return bytes(pending)

    # The emulator is ended on every way out of here: q, Ctrl-C, the window closed, a signal, an
    # error (leave(), in the `finally` below). And should this program be killed outright, the
    # emulator ends by itself when its app hangs up (--until-disconnect).
    for number in (signal.SIGHUP, signal.SIGTERM):
        signal.signal(number, on_signal)
    try:
        tty.setcbreak(keys)
        # sign_emu.Emu: first the emulator's own word that it listens on the port, then the
        # connection. Whoever else listens there (another sign-emu) is not connected to.
        emu = sign_emu.Emu([args.emu, "--speed", "1", "--port", str(port), "--wait-client", "20",
                            "--until-disconnect"] + args.emu_options + [args.hex], port, said=said, wait_s=20,
                           stdin=subprocess.DEVNULL, stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        sock = emu.connect()
        if args.start:
            sign.start()
        jumped_for = 0                # --auto: obstacles of this game it has pressed for
        quit_at = None
        while True:
            ready = select.select([sock, master, keys], [], [], 0.01)[0]
            now = now_ms()
            if keys in ready:
                data = os.read(keys, 64)
                if not data or b"q" in data or b"Q" in data or b"\x03" in data:     # (no data: the window is gone)
                    break
                if b" " in data or b"\x1b[A" in data or b"\x1bOA" in data:
                    sign.jump(now)
                if b"g" in data or b"G" in data or b"\r" in data or b"\n" in data:
                    if sign.start():
                        jumped_for = 0
            if args.auto and sign.stage == "PLAY" and sign.game.jumped_at < 0 and jumped_for < args.auto:
                # the nearest obstacle still to come is under the middle of the jump
                ahead = [x for _, x, passed in sign.game.coming if not passed]
                if ahead and min(ahead) <= 8 + sign.game.speed() * Rules.jump_ms / 2000.0:
                    sign.jump(now)
                    jumped_for += 1
            if args.auto and args.start and sign.stage == "NONE":
                if quit_at is None:
                    quit_at = now + 1500         # (a recording ends a moment after the closing show)
                elif now >= quit_at:
                    break
            if sock in ready:
                data = sock.recv(4096)
                if not data:
                    break
                for byte in data:
                    if byte == ord("~"):
                        message = sign.answer(now) or IDLE
                        sock.sendall(json.dumps(message, separators=(",", ":"), ensure_ascii=True).encode("ascii"))
            if master in ready:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    data = b""
                if not data:
                    break
                pending += data
                # whole redraws only: our two lines go in at the top of each
                while END in pending:
                    screen, rest = bytes(pending).split(END, 1)
                    pending[:] = rest
                    lines = ("  \x1b[1;33mSTAND-IN for the app's game\x1b[0m: the app's rules and shows, played by "
                             "emulator/game_app.py; the sign is the real firmware\x1b[K\r\n"
                             "  \x1b[1mg\x1b[0m or \x1b[1mEnter\x1b[0m start   \x1b[1mSPACE\x1b[0m or \x1b[1mup arrow\x1b[0m "
                             "jump   \x1b[1mq\x1b[0m quit      %s\x1b[K\r\n\x1b[K\r\n" % sign.status()).encode("utf-8")
                    screen = screen.replace(HOME, HOME + lines, 1).replace(b"  Ctrl-C to quit", b"  q to quit")
                    out.write(screen + END)
                    out.flush()
    except KeyboardInterrupt:                         # Ctrl-C is q
        pass
    except (RuntimeError, Ended, OSError) as e:      # (OSError: the window is gone, or the emulator's file)
        report, status = ("game_app: %s\n" % e).encode(), 1
    finally:
        for number in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT):
            signal.signal(number, signal.SIG_IGN)       # nothing ends the ending
        try:
            termios.tcsetattr(keys, termios.TCSADRAIN, saved)
        except (OSError, termios.error):
            pass                                        # the window is gone
        if emu is not None:
            pending, ended = leave(emu, master, pending)
            if ended != 0:
                report += ("game_app: sign-emu %s\n" % ("ended with status %d" % ended if ended > 0 else
                                                        "was ended by signal %d" % -ended)).encode()
                status = 1
        if sock is not None:
            sock.close()
    try:
        out.write(bytes(pending).replace(b"\r\n", b"\n").replace(b"\n", b"\r\n") + report)
        out.flush()
        print("game_app: best score of this sitting: %d" % sign.best)
    except OSError:
        pass                                            # the window is gone
    sys.exit(status)         # (not 0 when the emulator could not be reached or ended with an error, or on a signal)


if __name__ == "__main__":
    main()
