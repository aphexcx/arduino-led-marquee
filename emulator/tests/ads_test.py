#!/usr/bin/env python3
"""The ads on the real firmware image: 'O' (letters one by one) and 'C' (chonky with the colour
wave), short texts and texts wider than a panel, with hearts, and how long each stays.

A scripted peer plays the app: it answers each of the sign's requests with the next message of a
list. sign-emu runs the firmware headless and writes a frames dump; everything is checked from
that dump. Simulated time is the truth here: the image's own Timer0, Timer1 and interrupt masking
run in it.

What a picture must be is computed here from Fonts.h and Gamma.h, not copied from an earlier run.
The settings (pace, hold) are read from LED_Marquee.ino, so retuning them does not break the test.

For every 'O' and 'C' message:
  - every frame is exactly 120 columns of clean bits, and both sides are equal LED for LED
  - every frame is the text the font tables give, at a place the description allows, in the
    colours the sign has always sent for that style
  - a text wider than a panel moves one column at a time, never back, and ends with its last
    character whole at the right edge
  - no gap between two frames is longer than the pace allows, and no pause inside a frame is
    longer than the firmware's frames had before (a long pause would end the frame early)
  - from the first frame that shows the complete text (for a long text: its end) to the sign's
    next request is `dly`, within 2 percent or 10 ms, whichever is larger. (Firmware that waits
    for a quiet line before every request, REQUEST_QUIET_MS in the sketch, asks that much later
    in every style: the last picture of a message then stays for `dly` and that wait.)

usage: ads_test.py <sign-emu> <firmware.hex> <repo root> <work dir> [port]
Python 3 standard library only. Uses three TCP ports from `port` (default 14440).
"""
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

COLS, PANEL, ROWS = 120, 60, 7
HEART, VT = 0x7F, 0x0B
# The strips take a pause in the data as the end of a frame (50 us by the WS2812 data sheet). The
# longest pause inside a frame of the firmware before the ads were rebuilt was 25.6 us; nothing
# checked here may need more than that.
MAX_PAUSE_US = 26
failures = []
measured = []            # (style, text, dly asked, hold measured in ms, time on the sign in s)
print_lock = threading.Lock()


def check(log, ok, text):
    log.append("  %s  %s" % ("ok  " if ok else "FAIL", text))
    if not ok:
        failures.append(text)


# ---- what the firmware was built from -------------------------------------------------------
def numbers(body):
    body = re.sub(r"//[^\n]*", "", body)
    return [int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+|0[bB][01]+|\b\d+\b", body)]


def table(src, name):
    body = src[src.index(name):]
    return numbers(body[body.index("{") + 1:body.index("};")])


class Font:
    def __init__(self, data, cols, heart_cols):
        self.data, self.cols, self.heart_cols = data, cols, heart_cols

    def columns(self, text, shown=None):
        """One row byte per column (bit 7 = top row) of the first `shown` characters: each
        character's columns, then one blank column. `text` is the bytes the sign receives."""
        out = []
        for ch in text[:shown]:
            if ch == VT:
                pass                                  # nothing but the blank column
            elif ch < 0x20 or ch > 0x7F:
                out += [0] * self.cols                # not in the font: a space
            else:
                n = self.heart_cols if ch == HEART else self.cols
                at = (ch - 0x20) * self.cols
                out += self.data[at:at + n]
            out.append(0)
        return out


def side(cols, offset, pad=0):
    """What one side shows: `pad` blank columns, then the text from column `offset` on."""
    out = [0] * PANEL
    for c in range(pad, PANEL):
        at = offset + c - pad
        if at < len(cols):
            out[c] = cols[at]
    return out


def ink(cols):
    """Number of columns up to the last lit one."""
    lit = [c for c, v in enumerate(cols) if v]
    return lit[-1] + 1 if lit else 0


def setting(sketch, name, default=None):
    m = re.search(r"^#define\s+%s\s+(\d+)\s*$" % name, sketch, re.M)
    if not m:
        if default is not None:
            return default
        sys.exit("LED_Marquee.ino has no #define %s <number>" % name)
    return int(m.group(1))


def quiet_note(env):
    """For the text of a hold's check: what the firmware adds to every hold, if anything."""
    if not env["quiet"]:
        return ""
    return " and the %d ms this firmware waits for a quiet line before every request" % env["quiet"]


def carried(v):
    """A colour byte that sendBitx8 starts with the carry flag set goes out like this."""
    return v | (v >> 1)


# ---- running the emulator ---------------------------------------------------------------------
class Frame:
    """One latched frame, kept small: a run of the long texts is tens of thousands of them."""
    __slots__ = ("t", "bits", "clean", "pause", "mask", "colours", "equal")

    def __init__(self, d, cache):
        self.t = d["t"]
        self.bits = d["bits"]
        self.clean = not d["marginal"] and not d["partial_bits"]
        self.pause = d["gap_us"]                  # the longest pause between two bits of this frame
        rows = d["rows"]
        self.equal = all(r[:PANEL * 6] == r[PANEL * 6:] for r in rows)
        # One byte per column, bit 7 = top row, like the font data; and each column's colour
        self.mask = mask = [0] * COLS
        self.colours = colours = [None] * COLS
        for r, row in enumerate(rows):
            if row.count("0") == COLS * 6:
                continue
            bit = 0x80 >> r
            for c in range(COLS):
                px = row[c * 6:c * 6 + 6]
                if px != "000000":
                    mask[c] |= bit
                    px = cache.setdefault(px, px)
                    if colours[c] is None:
                        colours[c] = px
                    elif colours[c] is not px:
                        colours[c] = "mixed"

    def colour(self, c):
        """The colours of the lit LEDs of column c: a set of one 'rrggbb', or of 'mixed'."""
        return {self.colours[c]} if self.colours[c] else set()

    def sides_equal(self):
        return self.equal


def run_group(env, name, messages, port):
    """Serves `messages` once. Returns for each message (request time, next request time, frames)."""
    dump = os.path.join(env["work"], name + ".jsonl")
    log = os.path.join(env["work"], name + ".log")
    with open(log, "w") as err:
        emu = subprocess.Popen(
            [env["emu"], "--headless", "--port", str(port), "--wait-client", "30",
             "--until-requests", str(len(messages) + 1), "--duration", "900", "--frames", dump,
             env["hex"]], stderr=err)
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.monotonic() > deadline or emu.poll() is not None:
                    raise RuntimeError("%s: cannot connect to sign-emu on port %d" % (name, port))
                time.sleep(0.05)
        sock.settimeout(None)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sent = 0
        while sent < len(messages):
            data = sock.recv(4096)
            if not data:
                break
            for byte in data:
                if byte == ord("~") and sent < len(messages):
                    wire = json.dumps({k: v for k, v in messages[sent].items() if not k.startswith("_")},
                                      separators=(",", ":"), ensure_ascii=True)
                    sock.sendall(wire.encode("ascii"))
                    sent += 1
        status = emu.wait(timeout=900)
        sock.close()
    finally:
        if emu.poll() is None:
            emu.kill()
    if status != 0:
        raise RuntimeError("%s: sign-emu exited with status %s: %s"
                           % (name, status, open(log).read().strip()[-300:]))
    requests, frames, cache = [], [], {}
    for line in open(dump, encoding="utf-8"):
        d = json.loads(line)
        if d["type"] == "frame":
            frames.append(Frame(d, cache))
        elif d["type"] == "tx":
            if d.get("char") != "~":
                raise RuntimeError("%s: the sign sent %r: it could not read a message" % (name, d.get("char")))
            requests.append(d["t"])
    if len(requests) != len(messages) + 1:
        raise RuntimeError("%s: the sign asked %d times for %d messages: the run did not get to "
                           "its end" % (name, len(requests), len(messages)))
    out = []
    at = 0
    for i in range(len(messages)):
        while at < len(frames) and frames[at].t <= requests[i]:
            at += 1
        first = at
        while at < len(frames) and frames[at].t <= requests[i + 1]:
            at += 1
        out.append((requests[i], requests[i + 1], frames[first:at]))
    return out


def text_bytes(message):
    return message.get("str", "").encode("utf-8")


def label(message):
    text = message.get("str", "")
    shown = text if len(text) <= 24 else text[:10] + "..." + text[-8:] + " (%d characters)" % len(text)
    return "%s %s dly %s" % (message["type"], json.dumps(shown), message.get("dly", "missing"))


def common_checks(log, frames):
    check(log, bool(frames) and all(f.bits == COLS * 24 and f.clean for f in frames),
          "every frame is exactly 120 columns of clean bits (%d frames)" % len(frames))
    check(log, all(f.sides_equal() for f in frames), "both sides are equal in every frame, LED for LED")
    pause_check(log, frames)


def pause_check(log, frames):
    longest = max([f.pause for f in frames] or [0])
    check(log, longest <= MAX_PAUSE_US, "no pause inside a frame is longer than %d us (longest %.1f us)"
          % (MAX_PAUSE_US, longest))


def hold_check(log, env, message, style, complete_t, start, end):
    dly = message.get("dly", 0)
    hold_ms = (end - complete_t) * 1000
    allowed = max(dly * 0.02, 10)
    check(log, abs(hold_ms - dly - env["quiet"]) <= allowed,
          "from the first frame with the %s to the next request: %.2f ms for dly %d%s (within %.0f ms)"
          % ("end of the text" if message.get("_long") else "complete text", hold_ms, dly,
             quiet_note(env), allowed))
    measured.append((message["_order"], style, message.get("str", ""), dly, hold_ms, end - start))


# ---- 'O' ----------------------------------------------------------------------------------------
def one_by_one_steps(font, text):
    """The frames the description asks for: (letters shown, text column at the left edge, the
    pace that led to this frame)."""
    steps, offset, after_move = [], 0, False
    for p in range(len(text)):
        end = len(font.columns(text, p + 1)) - 1          # columns up to this letter's last one
        target = max(0, end - PANEL)
        moves = offset < target
        while offset + 1 < target:                        # make room, still without the letter
            offset += 1
            steps.append((p, offset, after_move))
            after_move = True
        if offset < target:
            offset += 1
        steps.append((p + 1, offset, after_move))         # the letter, with the last column
        after_move = moves
    return steps


def check_one_by_one(env, message, start, end, frames):
    log = [label(message)]
    font, text = env["std"], text_bytes(message)
    whole = font.columns(text)
    width = max(0, len(whole) - 1)
    pad = (PANEL - width) // 2 if width < PANEL else 0
    steps = one_by_one_steps(font, text)
    # the first frame after the request is clear(): all dark
    check(log, bool(frames) and not any(frames[0].mask), "the sign is cleared first")
    frames = frames[1:]
    common_checks(log, frames)
    check(log, len(frames) == len(steps), "one frame per letter and per scrolled column (%d, expected %d)"
          % (len(frames), len(steps)))
    if len(frames) != len(steps):
        return log
    pictures = all(f.mask[:PANEL] == side(font.columns(text, shown), offset, pad)
                   for f, (shown, offset, _) in zip(frames, steps))
    check(log, pictures, "every frame is the letters so far as Fonts.h has them%s"
          % (", centred (%d blank columns before)" % pad if width < PANEL else
             ", moved left one column at a time once the next letter does not fit"))
    g = env["gamma"]
    colour = "%02x%02x%02x" % (carried(g[message["r"]]), g[message["g"]], carried(g[message["b"]]))
    check(log, all(f.colour(c) <= {colour} for f in frames for c in range(COLS)),
          "every lit LED is %s: the message's colour as this style has always sent it" % colour)
    if width > PANEL:
        # A frame in which the text has moved is the frame before, one column further left: every
        # lit column of that one is found again at its left neighbour. (What may be new is to the
        # right of them: the letter that arrives with the last column of a move.)
        def shifted(a, b):
            return (a.mask[:PANEL] != b.mask[:PANEL] and
                    all(b.mask[c] == a.mask[c + 1] for c in range(PANEL - 1) if a.mask[c + 1]))
        moved = [b[1] - a[1] for a, b in zip(steps, steps[1:])]
        shifts = sum(1 for (a, b), m in zip(zip(frames, frames[1:]), moved) if m == 1 and shifted(a, b))
        still = sum(1 for (a, b), m in zip(zip(frames, frames[1:]), moved)
                    if m == 0 and b.mask[:PANEL] != a.mask[:PANEL] and
                    all(b.mask[c] == a.mask[c] for c in range(PANEL) if a.mask[c]))
        check(log, set(moved) <= {0, 1} and shifts == sum(moved) and steps[-1][1] == width - PANEL,
              "the text moves %d columns, one per frame, never back: each of those frames is the one "
              "before shifted left by exactly one column (%d are)" % (sum(moved), shifts))
        check(log, still + shifts >= len(text) - text.count(bytes([VT])) - text.count(b" ") - 1 - sum(
                  1 for ch in text if ch < 0x20 or ch > 0x7F),
              "in the other frames nothing moves and a letter is added (%d)" % still)
        last = frames[-1].mask[:PANEL]
        check(log, last == side(whole, width - PANEL) and last[PANEL - 1] == whole[width - 1],
              "the last character ends whole at the right edge")
        newest = all(len(font.columns(text, shown)) - 1 == offset + PANEL
                     for (shown, offset, _), (before, _, _) in zip(steps[1:], steps) if shown > before and offset)
        check(log, newest, "every letter that arrives while the text moves is whole and at the right edge")
    gaps = [(b.t - a.t) * 1000 for a, b in zip(frames, frames[1:])]
    paces = [env["step"] if after_move else env["letter"] for _, _, after_move in steps[1:]]
    worst = max([abs(gap - pace) for gap, pace in zip(gaps, paces)] or [0])
    check(log, worst <= 0.5, "a letter every %d ms, a scrolled column every %d ms (worst %.2f ms off; "
          "longest gap %.2f ms)" % (env["letter"], env["step"], worst, max(gaps or [0])))
    message["_long"] = width > PANEL
    hold_check(log, env, message, "O", frames[-1].t, start, end)
    return log


# ---- 'C' ----------------------------------------------------------------------------------------
def wave_colours_ok(frame, k, dly, green, blue):
    """Frame k of a position: red is the low byte of dly, less 10 per frame, plus 10 per column."""
    for c in range(COLS):
        if frame.mask[c]:
            red = (dly - 10 * k + 10 * (c % PANEL)) & 0xFF
            if frame.colour(c) != {"%02x%02x%02x" % (carried(red), carried(green), carried(blue))}:
                return False
    return True


def check_chonky(env, message, start, end, frames):
    log = [label(message)]
    font, text = env["chonk"], text_bytes(message)
    cols = font.columns(text)
    last = max(0, ink(cols) - PANEL)                      # where the text ends up
    dly = message.get("dly", 0)
    common_checks(log, frames)
    if not frames:
        return log
    offset, at_end, moves, pictures, colours = 0, 0, [], True, True
    for k, f in enumerate(frames):
        if offset < last and f.mask[:PANEL] == side(cols, offset + 1) and f.mask[:PANEL] != side(cols, offset):
            offset += 1
            moves.append(f.t)
            if offset == last:
                at_end = k
        pictures = pictures and f.mask[:PANEL] == side(cols, offset)
        colours = colours and wave_colours_ok(f, k, dly, message["g"], message["b"])
    check(log, pictures, "every frame is the text as Fonts.h has it, from column 0%s"
          % (", then moved left one column at a time, never back" if last else ""))
    check(log, colours, "the wave: red starts at the low byte of dly, less 10 per frame, plus 10 per "
          "column; green and blue are the message's; all as this style has always sent them")
    gaps = [(b.t - a.t) * 1000 for a, b in zip(frames, frames[1:])]
    check(log, max(gaps or [0]) <= env["wave"] + 0.5,
          "the wave never stops: no gap between two frames longer than %d ms (longest %.2f ms)"
          % (env["wave"], max(gaps or [0])))
    if last:
        check(log, offset == last and frames[-1].mask[PANEL - 1] != 0,
              "the text moved %d columns and its last lit column is the panel's last" % last)
        held = (moves[0] - frames[0].t) * 1000
        check(log, abs(held - env["hold"]) <= 1, "its start stood still for %.2f ms (AD_SCROLL_HOLD_MS %d)"
              % (held, env["hold"]))
        steps = [(b - a) * 1000 for a, b in zip(moves, moves[1:])]
        worst = max([abs(s - env["step"]) for s in steps] or [0])
        check(log, worst <= 0.5, "then one column every %d ms (worst %.2f ms off)" % (env["step"], worst))
    elif dly == 1000:
        # A frame every AD_WAVE_FRAME_MS for as long as the text stays. The hold is counted from when
        # the first frame is out (4.4 to 4.9 ms after it was begun), and a frame is not begun when
        # fewer than 6 ms of the hold are left: frames are begun at 0, W, 2 W, ... up to 1000 - 6 + 4.4.
        # (10 ms: 100 frames, as it always was; 20 ms: 50; 15 ms: 67.)
        wanted = (1000 - 2) // env["wave"] + 1
        check(log, len(frames) == wanted, "dly 1000 is %d frames, one every %d ms (%d)"
              % (wanted, env["wave"], len(frames)))
    elif dly == 0:
        check(log, len(frames) == 1, "dly 0 or missing is one frame (%d)" % len(frames))
    message["_long"] = bool(last)
    hold_check(log, env, message, "C", frames[at_end].t, start, end)
    return log


# ---- the other styles that draw a heart, and the nudge ---------------------------------------------
def find(haystack, needle):
    n = len(needle)
    return any(haystack[i:i + n] == needle for i in range(len(haystack) - n + 1))


def check_marquee(env, message, start, end, frames, font_name, what, probe):
    """`probe`: how many bytes from the start of the text to look for in one frame."""
    log = [label(message)]
    font = env[font_name]
    cols = font.columns(text_bytes(message)[:probe])[:-1]
    check(log, any(find(f.mask, [0] + cols + [0]) for f in frames),
          "%s: a frame shows %s as Fonts.h has it, the heart %d columns wide"
          % (what, json.dumps(message["str"][:probe]), font.heart_cols))
    # Pairs of whole frames with text in them, one straight after the other. (A marquee frame can
    # be a few columns longer or shorter than the strip, and a shorter one leaves the columns it
    # does not reach as they were; the starfield after the marquee lights one LED per frame.)
    def whole(f):
        return f.bits >= COLS * 24 and sum(bin(m).count("1") for m in f.mask) >= 2
    pairs = [(a, b) for a, b in zip(frames, frames[1:]) if whole(a) and whole(b)]
    left = sum(1 for a, b in pairs if b.mask[:COLS - 1] == a.mask[1:])
    check(log, len(pairs) > 30 and left == len(pairs),
          "it moves one column per frame, through the hearts too (%d of %d pairs of whole frames)"
          % (left, len(pairs)))
    return log


def check_flashy(env, message, start, end, frames, what):
    log = [label(message)]
    font, text = env["std"], text_bytes(message)
    cols = font.columns(text)[:-1]
    pad = (PANEL - len(cols)) // 2 if len(cols) < PANEL else 0
    lit = [f for f in frames if f.bits == COLS * 24 and sum(bin(m).count("1") for m in f.mask) >= 2]
    expected = side(cols, 0, pad)
    hits = [f for f in lit if f.mask[:PANEL] == expected and f.mask[PANEL:] == expected]
    check(log, len(hits) > 20, "%s (%d frames)" % (what, len(hits)))
    pause_check(log, lit)
    return log


def check_nudge(env, message, start, end, frames):
    log = [label(message)]
    cols = env["chonk"].columns(text_bytes(message))
    common_checks(log, frames)
    seen = []
    for f in frames:
        for skip in range(10):
            if f.mask[:PANEL] == side(cols, skip):
                if not seen or seen[-1] != skip:
                    seen.append(skip)
                break
        else:
            seen.append(None)
    # the first character's lit columns and its blank one; a vertical tab is only its blank column
    text = text_bytes(message)
    positions = 1 if not text or text[0] == VT else env["chonk"].cols + 1
    check(log, seen == list(range(positions)), "the text moves left by its first character, a column at a "
          "time, no further, and the message ends (positions seen: %s)" % seen)
    # when each position first appears; the last one stays until the next request
    appear = [f.t for f, before in zip(frames, [None] + frames[:-1])
              if before is None or f.mask[:PANEL] != before.mask[:PANEL]] + [end]
    shown = [(b - a) * 1000 for a, b in zip(appear, appear[1:])]
    # one position to the next is the firmware's own pacing; the last one ends with the sign's next
    # request, which is held to the same tolerance as every hold (2 percent or 10 ms)
    dly = message["dly"]
    worst = max([abs(s - dly) for s in shown[:-1]] or [0])
    last_ok = bool(shown) and abs(shown[-1] - dly - env["quiet"]) <= max(dly * 0.02, 10)
    check(log, len(shown) == positions and worst <= 1 and last_ok, "each column is shown for dly (%s ms for dly %d%s)"
          % (" ".join("%.2f" % s for s in shown), message["dly"],
             "" if not env["quiet"] else "; the last one also for the %d ms" % env["quiet"] + quiet_note(env)[len(" and the %d ms" % env["quiet"]):]))
    return log


# ---- the cases ---------------------------------------------------------------------------------------
def long_text(n):
    """n characters with spaces and the odd heart, different all along so that a wrong column shows."""
    words = "I LIVE FOR THAT ENERGY \u007f TRANCE FAMILY UNITED \u007f DREAMSTATE 2026".split(" ")
    out, i = "", 0
    while len(out) < n:
        out += words[i % len(words)] + " "
        i += 1
        if i % len(words) == 0:
            out += "%d " % (i // len(words))
    out = out[:n]
    return out[:-1] + "!" if out.endswith(" ") else out


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    root = sys.argv[3]
    fonts = open(os.path.join(root, "Fonts.h"), encoding="utf-8", errors="replace").read()
    sketch = open(os.path.join(root, "LED_Marquee.ino"), encoding="utf-8", errors="replace").read()
    gamma = table(open(os.path.join(root, "Gamma.h"), encoding="utf-8").read(), "gamma[]")
    std, chonk = table(fonts, "FontStd5x7[]"), table(fonts, "FontChonk[]")
    if len(std) != 95 * 5 + 7 or len(chonk) != 96 * 8 or len(gamma) != 256:
        sys.exit("Fonts.h or Gamma.h is not what this test expects (%d, %d, %d values)"
                 % (len(std), len(chonk), len(gamma)))
    env = {
        "emu": sys.argv[1], "hex": sys.argv[2], "work": sys.argv[4],
        "std": Font(std, 5, 7), "chonk": Font(chonk, 8, 8), "gamma": gamma,
        "letter": setting(sketch, "CHARS_ONEBYONE_DELAY"), "step": setting(sketch, "AD_SCROLL_STEP_MS"),
        "hold": setting(sketch, "AD_SCROLL_HOLD_MS"), "wave": setting(sketch, "AD_WAVE_FRAME_MS"),
        # (0 for firmware that asks at once: master's, before chooser contract 3)
        "quiet": setting(sketch, "REQUEST_QUIET_MS", 0),
    }
    port = int(sys.argv[5]) if len(sys.argv) > 5 else 14440
    os.makedirs(env["work"], exist_ok=True)

    forty = "THE QUICK BROWN FOX JUMPS OVER LAZY DOGS"
    assert len(forty) == 40 and len(long_text(300)) == 300
    o = lambda s, dly=None, **kw: dict({"type": "O", "str": s, "r": 255, "g": 212, "b": 59},
                                       **({} if dly is None else {"dly": dly}), **kw)
    c = lambda s, dly=None, **kw: dict({"type": "C", "str": s, "r": 29, "g": 161, "b": 242},
                                       **({} if dly is None else {"dly": dly}), **kw)
    one_by_one = [
        (o("TEN CHARS!", 1000), check_one_by_one), (o("ABCDEFGH", 2000), check_one_by_one),
        (o("HELLO", 3000), check_one_by_one), (o("HELLO YOU?", 4000), check_one_by_one),
        (o("WAIT FOR..", 999), check_one_by_one), (o("NO DLY"), check_one_by_one),
        (o("LOVE YOU\u007f", 2000), check_one_by_one),            # a heart, fits
        (o("HELLO WORLD", 1000), check_one_by_one),               # 11 characters: just too wide
        (o("I \u007f TRANCE \u007f\u007f AND YOU", 2000), check_one_by_one),  # hearts, too wide
        (o("TAB\u000bS é AND \u0001 ODD BYTES", 333), check_one_by_one),  # tabs; bytes outside the font
        (o(forty, 1000), check_one_by_one),
        (o("HOLD 30 S", 30000), check_one_by_one),                # the clock over a long hold
        (o(long_text(300), 1000), check_one_by_one),
    ]
    chonky = [
        (c("1234!!", 1000), check_chonky), (c(" ABC", 2000), check_chonky), (c("CHONK", 3000), check_chonky),
        (c(" A..", 4000), check_chonky), (c("ENERGY", 999), check_chonky), (c("NO DLY"), check_chonky),
        (c("SAYS...", 250), check_chonky),                        # 7 characters, 56 lit columns: fits
        (c("I \u007f U", 2000), check_chonky),                    # a heart, fits
        (c("ENERGY!", 1000), check_chonky),                       # 7 characters: one column too wide
        (c("I \u007f DREAMSTATE", 2000), check_chonky),           # a heart, too wide
        (c("TAB\u000bS é ODD", 333), check_chonky),          # tabs; bytes outside the font
        (c(forty, 1000), check_chonky),
        (c("HOLD30", 30000), check_chonky),                       # the clock over a long hold
    ]
    rest = [
        (c(long_text(300), 1000), check_chonky),
        (c("ABCDEF", 333, scroll=True), check_nudge),             # an odd dly never ended before
        (c("\u000bAB", 333, scroll=True), check_nudge),           # a tab first: one position, not nine
        ({"type": "N", "str": "I \u007f DREAMSTATE \u007f"},
         lambda e, m, s, t, f: check_marquee(e, m, s, t, f, "chonk", "chonky marquee", 5)),
        ({"type": "D", "str": "I \u007f U \u0001! AND \u007f\u007f TRANCE"},
         lambda e, m, s, t, f: check_marquee(e, m, s, t, f, "std", "marquee (a byte outside the font is a space)", 8)),
        ({"type": "T", "str": "I \u007f THIS TRACK \u007f WHAT IS IT CALLED \u007f"},
         lambda e, m, s, t, f: check_marquee(e, m, s, t, f, "std", "track id marquee", 12)),
        ({"type": "F", "str": "I \u007f U"},
         lambda e, m, s, t, f: check_flashy(e, m, s, t, f, "flashy text with a heart is centred, the same on both sides")),
        ({"type": "F", "str": "NEW\u000bMSG\u000bX"},
         lambda e, m, s, t, f: check_flashy(e, m, s, t, f, "flashy text with tabs is centred, the same on both sides")),
        ({"type": "F", "str": "TEN CHARS!"},
         lambda e, m, s, t, f: check_flashy(e, m, s, t, f, "flashy text as wide as a panel, the same on both sides")),
    ]
    groups = [("one_by_one", one_by_one), ("chonky", chonky), ("rest", rest)]
    results = [None] * len(groups)
    order = 0
    for _, cases in groups:
        for message, _ in cases:
            message["_order"] = order   # keys starting with "_" are this script's and are not sent
            order += 1

    def work(i):
        name, cases = groups[i]
        try:
            runs = run_group(env, name, [m for m, _ in cases], port + i)
            logs = []
            for (message, checker), (start, end, frames) in zip(cases, runs):
                logs += checker(env, message, start, end, frames)
            results[i] = logs
        except Exception as e:  # a run that could not be made or read is a failed test, not a crash
            failures.append("%s: %s" % (name, e))
            results[i] = ["  FAIL  %s: %r" % (name, e)]

    threads = [threading.Thread(target=work, args=(i,)) for i in range(len(groups))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for (name, _), logs in zip(groups, results):
        print("== %s" % name)
        print("\n".join(logs))

    print("== the holds, measured (first frame with the complete text, or its end, to the next request)")
    print("  %-5s %-34s %9s %12s %12s" % ("style", "text", "dly asked", "hold (ms)", "on the sign"))
    with open(os.path.join(env["work"], "holds.tsv"), "w", encoding="utf-8") as out:
        out.write("style\ttext\tcharacters\tdly_ms\thold_ms\ton_sign_s\n")
        for _, style, text, dly, hold, total in sorted(measured):
            shown = text if len(text) <= 30 else "%s... (%d characters)" % (text[:12], len(text))
            print("  %-5s %-34s %9d %12.2f %10.2f s" % (style, json.dumps(shown), dly, hold, total))
            out.write("%s\t%s\t%d\t%d\t%.3f\t%.3f\n" % (style, json.dumps(text), len(text), dly, hold, total))

    if failures:
        print("FAILED: %d check(s)" % len(failures))
        for f in failures:
            print("  " + f)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
