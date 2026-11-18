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
    next request is `dly`, within 2 percent or 10 ms, whichever is larger

The supply. For every message the test adds up what each frame asks of the supply, by the model in
LedBudget.h (20 mA per colour channel at 255, in proportion to the value sent), and prints a table:
the peak of the two sides together, the peak of one side, and the biggest rise from one frame to
the next. With the sketch's settings (no limit, no ramp) that table is all; nothing is asserted.
For an image built with limits or a ramp, say so with the options below, and the test also holds:
no frame above a limit, a text under the limits untouched LED for LED, a dimmed text at one
brightness from its first frame to its last (worked out here the slow way, not copied from the
firmware), and with a ramp no rise between two frames of a chonky text larger than a frame's share
of its peak plus the wave's own flicker.

The limits hold for every style but the chooser (H) and the starfield (S, and the stars the sketch
draws after F, W, D and N): those are measured and never asserted. For every other style, on an
image with limits, every frame from the message's request to the next must write all 120 columns of
the strip and be within the limits. What is measured is the strip as latched: a column that a frame
does not write keeps what the frame before put there, and is counted. On such an image every style
is also run straight after every style (121 pairs, the beat input on), and the result printed as a
table.

usage: ads_test.py <sign-emu> <firmware.hex> <repo root> <work dir> [port]
                   [--limit-ma N] [--limit-side-ma N] [--ramp-ms N] [--also playlist.jsonl]
                   [--random N [--seed S]]
--also: more messages (one JSON object per line) to run and put in the table, for example the
sign's own ad list; O and C messages in it get all the checks.
--random: N more messages of the limited styles with random texts and colours (seed S, default 1),
held to the limits like the rest.
--same-as: a second image (the one built without limits) to hold this one to, for an image whose
limit is so high that it is never reached: every frame the same, LED for LED, wherever the other
image's frame writes (see same_pictures).
Python 3 standard library only. Uses a TCP port per group of messages from `port` (default 14440):
8 groups (9 on an image with limits), and one more for every 20 random messages and for --also;
with --same-as the same again 100 higher.
"""
import itertools
import json
import os
import random
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
supply = []              # (order, label, peak both sides, peak one side, biggest rise both, biggest rise one side) in A
transitions = {}         # (style before, style after): the most one side asked for during the style after, in A
# The styles the supply's limits do not hold for (the chooser, the starfield): measured, never asserted
EXEMPT = ("H", "S")
# The starfield that the sketch draws straight after some styles, in frames (one star each): not limited either
STARS_AFTER = {"F": 300, "W": 300, "D": 100, "N": 100, "-": 300}
KNOWN = ("O", "C", "U", "K", "F", "W", "S", "I", "T", "N", "D", "H", "-")
AMPS = 20.0 / 255 / 1000 # what one unit of load (a value of 1 on one channel of one LED) is in A
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


def setting(sketch, name):
    m = re.search(r"^#define\s+%s\s+(\d+)\s*$" % name, sketch, re.M)
    if not m:
        sys.exit("LED_Marquee.ino has no #define %s <number>" % name)
    return int(m.group(1))


def carried(v):
    """A colour byte that sendBitx8 starts with the carry flag set goes out like this."""
    return v | (v >> 1)


# ---- running the emulator ---------------------------------------------------------------------
class Frame:
    """One latched frame, kept small: a run of the long texts is tens of thousands of them."""
    __slots__ = ("t", "bits", "clean", "pause", "mask", "colours", "equal", "load", "lit")

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
        load = [0, 0]                             # the values sent, added up, side A and side B
        self.lit = 0                              # how many LEDs are lit
        for r, row in enumerate(rows):
            if row.count("0") == COLS * 6:
                continue
            bit = 0x80 >> r
            for c in range(COLS):
                px = row[c * 6:c * 6 + 6]
                if px != "000000":
                    mask[c] |= bit
                    self.lit += 1
                    px, value = cache.setdefault(px, (px, int(px[0:2], 16) + int(px[2:4], 16) + int(px[4:6], 16)))
                    load[c >= PANEL] += value
                    if colours[c] is None:
                        colours[c] = px
                    elif colours[c] is not px:
                        colours[c] = "mixed"
        self.load = load

    def colour(self, c):
        """The colours of the lit LEDs of column c: a set of one 'rrggbb', or of 'mixed'."""
        return {self.colours[c]} if self.colours[c] else set()

    def sides_equal(self):
        return self.equal


class Replies:
    """What the app answers, like fake_app.py with chooser contract 1: '~' gets the next message.
    So does '?', the chooser's poll, except that the first `_polls` polls after a chooser message
    (type H) are answered with {} ("nothing changed"). One of these per run, and one more to read
    the dump the same way."""

    def __init__(self, messages):
        self.messages, self.sent, self.polls_left = messages, 0, 0

    def answer(self, char):
        """Returns (bytes to send or None, True if this request was answered with a message)."""
        if char == "?" and self.polls_left > 0:
            self.polls_left -= 1
            return b"{}", False
        if char not in "~?" or self.sent >= len(self.messages):
            return None, False
        message = self.messages[self.sent]
        self.sent += 1
        self.polls_left = message.get("_polls", 0) if message.get("type") == "H" else 0
        wire = json.dumps({k: v for k, v in message.items() if not k.startswith("_")},
                          separators=(",", ":"), ensure_ascii=True)
        return wire.encode("ascii"), True


def run_group(env, name, messages, port, flags=()):
    """Serves `messages` once. Returns for each message (request time, next request time, frames)."""
    polls = sum(m.get("_polls", 0) for m in messages if m.get("type") == "H")
    dump = os.path.join(env["work"], name + ".jsonl")
    log = os.path.join(env["work"], name + ".log")
    with open(log, "w") as err:
        emu = subprocess.Popen(
            [env["emu"], "--headless", "--port", str(port), "--wait-client", "30",
             "--until-requests", str(len(messages) + polls + 1), "--duration", "900", "--frames", dump,
             *flags, env["hex"]], stderr=err)
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
        app = Replies(messages)
        while app.sent < len(messages):
            data = sock.recv(4096)
            if not data:
                break
            for byte in data:
                reply, _ = app.answer(chr(byte))
                if reply is not None:
                    sock.sendall(reply)
        status = emu.wait(timeout=900)
        sock.close()
    finally:
        if emu.poll() is None:
            emu.kill()
    if status != 0:
        raise RuntimeError("%s: sign-emu exited with status %s: %s"
                           % (name, status, open(log).read().strip()[-300:]))
    requests, frames, cache = [], [], {}
    app = Replies(messages)                               # the same answers again, to tell the requests apart
    unreadable = any(m.get("_error") for m in messages)   # the sign then writes the error's name on the wire
    for line in open(dump, encoding="utf-8"):
        d = json.loads(line)
        if d["type"] == "frame":
            frames.append(Frame(d, cache))
        elif d["type"] == "tx":
            if d.get("char") not in ("~", "?"):
                if unreadable:
                    continue
                raise RuntimeError("%s: the sign sent %r: it could not read a message" % (name, d.get("char")))
            # a request that was answered with a message starts that message; the one after the
            # last message ends the run; a poll answered with {} is neither
            if app.sent >= len(messages) or app.answer(d["char"])[1]:
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
        # what the sign was showing when the message came: the last frame latched before it
        before = frames[first - 1].load if first else [0, 0]
        out.append((requests[i], requests[i + 1], frames[first:at], before))
    # ... and what the sign drew by itself before it asked for anything: the startup words
    return out, [f for f in frames if f.t <= requests[0]]


# ---- an image whose limit is never reached draws what the image without limits draws ---------------
# The groups that are compared (in the others the pictures depend on when the beat input fires, or on
# when the chooser's requests are answered, and the two images take different times over a frame)
SAME_GROUPS = ("one_by_one", "chonky", "rest", "notices", "shift", "frames_not_texts")


def by_message(dump, windows):
    """Yields (number of the message, its frames as the dump has them) for every message with
    frames, in order. windows: (after, up to) in seconds for each message."""
    def frames():
        i = 0
        for line in open(dump, encoding="utf-8"):
            d = json.loads(line)
            if d["type"] != "frame":
                continue
            while i < len(windows) and d["t"] > windows[i][1]:
                i += 1
            if i == len(windows):
                return
            if d["t"] > windows[i][0]:
                yield i, d
    for i, group in itertools.groupby(frames(), key=lambda x: x[0]):
        yield i, [d for _, d in group]


def same_pictures(cases, runs, dump, ref_runs, ref_dump, ref_name):
    """Holds this image's frames to the reference image's, message by message and frame by frame:
    every LED of every column the reference frame wrote has the same three values here. Where a
    reference frame is shorter than the strip (it leaves what was there before), this image's
    frame, when it writes the whole strip, is black."""
    log = []
    labels = ["the startup words and the stars after them"] + [label(m) for m, _ in cases]
    windows = lambda rs: [(0.0, rs[0][0])] + [(start, end) for start, end, _, _ in rs]
    ref, got = by_message(ref_dump, windows(ref_runs)), by_message(dump, windows(runs))
    a, b = next(ref, None), next(got, None)
    while a or b:
        i = min(x[0] for x in (a, b) if x)
        fa = a[1] if a and a[0] == i else []
        fb = b[1] if b and b[0] == i else []
        equal, short = len(fa) == len(fb), 0
        for x, y in zip(fa, fb):
            n = min(x["bits"] // 24, COLS) * 6            # what the reference frame wrote
            short += n < COLS * 6
            padded = y["bits"] >= COLS * 24
            for rx, ry in zip(x["rows"], y["rows"]):
                if rx[:n] != ry[:n] or (padded and ry[n:].strip("0")):
                    equal = False
        check(log, equal, "%s: LED for LED what %s draws (%d frames, %d there%s)"
              % (labels[i], ref_name, len(fb), len(fa),
                 "; %d of them shorter than the strip there, and black beyond that here" % short if short else ""))
        if a and a[0] == i:
            a = next(ref, None)
        if b and b[0] == i:
            b = next(got, None)
    return log


def text_bytes(message):
    return message.get("str", "").encode("utf-8")


def label(message):
    text = message.get("str", "")
    shown = text if len(text) <= 24 else text[:10] + "..." + text[-8:] + " (%d characters)" % len(text)
    return "%s %s dly %s%s" % (message["type"], json.dumps(shown), message.get("dly", "missing"),
                               " " + message["_note"] if "_note" in message else "")


def check_shifted(env, message, start, end, frames):
    """An O or C text that fits a panel, shown while the sign's brightness shift is `_shift`.
    The shift halves what is sent, so a text that only the shift brings under the limits must be
    left alone by them: not dimmed a second time. The colours themselves cannot be pinned (with a
    shift the carry of each byte is whatever the shifting left), so each frame's load is held
    between what the values come to without any carry and with every carry."""
    log = [label(message)]
    shift, wave = message["_shift"], message["type"] == "C"
    font = env["chonk"] if wave else env["std"]
    cols = font.columns(text_bytes(message))
    if wave:
        r, g, b = 0, message["g"], message["b"]
    else:
        frames = frames[1:]                               # the clear
        message["_before"] = [0, 0]
        r, g, b = (env["gamma"][message[k]] for k in "rgb")
    common_checks(log, frames)
    limit_check(log, env, message, frames)
    dim = model_scale(env, cols, r, g, b, wave, shift)
    plain = model_scale(env, cols, r, g, b, wave, 0)
    if plain == 255 or plain == dim:
        # this image's limits are above this text even without the shift, or so far below it that
        # the shift does not help either: nothing to tell apart
        log.append("  note  the shift makes no difference to what this image's limits do to this text")
    else:
        check(log, plain < dim, "the test has teeth: without the shift this text would be dimmed to %d/256, "
              "with shift %d %s" % (plain + 1, shift, "it fits as it is" if dim == 255 else
                                    "only to %d/256" % (dim + 1)))
    dly = message.get("dly", 0)
    width = max(0, len(cols) - 1)
    pad = 0 if wave else (PANEL - width) // 2
    ok, low_seen, high_seen = True, 0, 0
    dims = ramp_dims(env, dim, frames, len(frames)) if wave else None  # a text that fits: every frame is paced
    for k, f in enumerate(frames if wave else frames[-1:]):
        low = high = 0
        for c, v in enumerate(side(cols, 0, pad)):
            for d, top in ((dims[k][0], False), (dims[k][-1], True)) if wave else ((dim, False), (dim, True)):
                red = dimmed((dly - 10 * k + 10 * c) & 0xFF, d) if wave else dimmed(r, d)
                values = [red >> shift, dimmed(g, d) >> shift, dimmed(b, d) >> shift]
                if top:
                    high += bin(v).count("1") * sum(carried(x) for x in values)
                else:
                    low += bin(v).count("1") * sum(values)
        ok = ok and low <= f.load[0] <= high
        low_seen, high_seen = max(low_seen, low), max(high_seen, high)
    check(log, ok, "at brightness shift %d it is sent as the shift alone makes it, not dimmed again: every "
          "frame's load on a side is between %.2f and %.2f A (most: %.2f A)"
          % (shift, low_seen * AMPS, high_seen * AMPS, max([f.load[0] for f in frames] or [0]) * AMPS))
    return log


def check_dark(env, message, start, end, frames):
    """A message shown while the brightness shift is 8: nothing is left of any value. (A shift of
    200 used to be stored as it came, and the image then sent everything at full brightness: on
    the sign, and past the limits, whose estimate took such a shift for dark.)"""
    log = [label(message)]
    check(log, bool(frames) and not any(any(f.mask) for f in frames),
          "the most the brightness shift can be is 8, and at 8 the sign is dark: no LED lit in %d frames"
          % len(frames))
    limit_check(log, env, message, frames)
    return log


def check_style(env, message, start, end, frames):
    """A message that is run for the table of what it asks of the supply, and for the limits."""
    log = [label(message), "  ok    ran to its end (%d frames, %.2f s)" % (len(frames), end - start)]
    limit_check(log, env, message, frames)
    if "_after" in message:
        style = style_frames(message, frames)[0]
        transitions[(message["_after"], message["type"])] = max([max(f.load) for f in style] or [0]) * AMPS
    return log


# ---- the supply -------------------------------------------------------------------------------------
def supply_row(message, frames):
    """Puts a message's row into the table. Returns (peak both sides, peak one side) as loads.
    A rise is from one latched frame to the next, and for a message's first frame from what the
    sign was showing when the message came (a chonky text stays lit until the next message draws)."""
    loads = [message.get("_before", [0, 0])] + [f.load for f in frames]
    both = [f.load[0] + f.load[1] for f in frames] or [0]
    one = [max(f.load) for f in frames] or [0]
    rise_both = max([sum(b) - sum(a) for a, b in zip(loads, loads[1:])] + [0])
    rise_one = max([max(b[0] - a[0], b[1] - a[1]) for a, b in zip(loads, loads[1:])] + [0])
    supply.append((message["_order"], label(message), max(both) * AMPS, max(one) * AMPS, rise_both * AMPS,
                   rise_one * AMPS))
    return max(both), max(one)


def style_frames(message, frames):
    """(the frames of the message's own style, the frames of the starfield the sketch draws after
    it). A star is a frame of 121 columns with at most one LED lit; how many follow which style is
    the sketch's (STARS_AFTER). None for the second when the last frames are not that starfield."""
    kind = message["type"]
    stars = STARS_AFTER.get(kind, 0 if kind in KNOWN else STARS_AFTER["D"])  # an unknown type is shown as D
    stars = message.get("_stars", stars)
    if not stars:
        return frames, []
    tail = frames[-stars:]
    if len(tail) != stars or not all(f.bits == (COLS + 1) * 24 and f.lit <= 1 for f in tail):
        return frames, None
    return frames[:-stars], tail


def limit_check(log, env, message, frames):
    """Every style: its row in the table (all the frames from the message's request to the next).
    On an image with limits, for every style the limits hold for: each of its frames writes the
    whole strip (so nothing of an earlier picture stays lit beside it), and none asks more than a
    limit. The frame a test sees is the strip as it is latched, whatever part of it the frame
    wrote. The chooser, the starfield, and the starfield after a style are measured only."""
    supply_row(message, frames)
    if message["type"] in EXEMPT:
        log.append("  note  this style is not limited: measured only")
        return
    style, stars = style_frames(message, frames)
    check(log, stars is not None, "the starfield follows the style (its last frames are stars: 121 columns, one LED)")
    if not (env["limit_both"] or env["limit_side"]):
        return
    check(log, all(f.bits >= COLS * 24 for f in style),
          "every frame writes all 120 columns of the strip (shortest: %d columns; %d frames)"
          % (min([f.bits for f in style] or [COLS * 24]) // 24, len(style)))
    both = max([f.load[0] + f.load[1] for f in style] or [0])
    one = max([max(f.load) for f in style] or [0])
    if env["limit_both"]:
        check(log, both <= env["limit_both"], "no frame asks more than %d mA of the supply, both sides together "
              "(most: %.0f mA)" % (env["limit_ma"], both * AMPS * 1000))
    if env["limit_side"]:
        check(log, one <= env["limit_side"], "no frame asks more than %d mA on one side (most: %.0f mA)"
              % (env["limit_side_ma"], one * AMPS * 1000))


def allowance(env, lit):
    """What one lit LED may send, its three values added up, when a side has `lit` LEDs lit."""
    allowed = 765
    if lit and env["limit_both"]:
        allowed = min(allowed, env["limit_both"] // (2 * lit))
    if lit and env["limit_side"]:
        allowed = min(allowed, env["limit_side"] // lit)
    return allowed


def dimmed(v, scale):
    return v * (scale + 1) // 256


def model_scale(env, cols, r, g, b, wave, shift=0):
    """How far a text in this colour must be dimmed (255 = not at all), worked out the slow way:
    the most LEDs any 60 columns of it light on a side, what each may then send under the limits,
    and the brightest dimming up to which no dimming sends more (a ramp passes through all those
    below its own). wave: a chonky text, whose red can be any value and whose green goes out carried.
    shift: the sign's brightness shift; with one, every value is shifted right before it is sent and
    the carry of every byte is unknown, so all three channels are taken as carried."""
    lit = [bin(v).count("1") for v in cols]
    most = max([sum(lit[i:i + PANEL]) for i in range(len(lit))] or [0])
    allowed = 765
    if most and env["limit_both"]:
        allowed = min(allowed, env["limit_both"] // (2 * most))
    if most and env["limit_side"]:
        allowed = min(allowed, env["limit_side"] // most)
    best = 0
    for scale in range(256):
        red = (max(carried(dimmed(v, scale) >> shift) for v in range(256)) if wave
               else carried(dimmed(r, scale) >> shift))
        green = carried(dimmed(g, scale) >> shift) if wave or shift else dimmed(g, scale)
        if red + green + carried(dimmed(b, scale) >> shift) > allowed:
            # A chonky text with a ramp passes through all the dimmings below its own: it has to stay
            # below the first that is too bright. Any other text is only shown at its own dimming.
            if wave and env["ramp_frames"] > 1:
                break
            continue
        best = scale
    return best


def ramp_dims(env, scale, frames, paced):
    """For each frame of a chonky message, the dimmings it may have while the text comes on: the
    ramp is a stretch of TIME cut into parts as long as a frame of the wave, and the part a frame
    is in comes from when it is drawn, counted from the message's first frame.
    The first `paced` frames are drawn one per frame of the wave, so frame k is in part k. Later
    frames (a text that scrolls: they come faster) are placed by the time they were latched; that
    is the time they were drawn give or take 0.3 ms (the firmware's sums before a frame take a
    little longer for some values than for others), so a frame within 0.3 ms of the edge of two
    parts may be in either and gets two candidates."""
    out = []
    for k, f in enumerate(frames):
        ms = (f.t - frames[0].t) * 1000
        parts = {k} if k < paced else {int(max(0.0, ms - 0.3) // env["wave"]), int((ms + 0.3) // env["wave"])}
        out.append(sorted({ramp_scale(env, scale, part) for part in parts}))
    return out


def ramp_scale(env, scale, frame):
    """The dimming of a frame in part number `frame` of the ramp over which a chonky text comes on."""
    n = env["ramp_frames"]
    return scale if n < 2 or frame + 1 >= n else scale * (frame + 1) // n


def dim_note(scale):
    return "not dimmed" if scale == 255 else "dimmed to %d/256 for the supply's limit" % (scale + 1)


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
    check(log, abs(hold_ms - dly) <= allowed,
          "from the first frame with the %s to the next request: %.2f ms for dly %d (within %.0f ms)"
          % ("end of the text" if message.get("_long") else "complete text", hold_ms, dly, allowed))
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
    message["_before"] = [0, 0]                           # so its letters come onto a dark sign
    common_checks(log, frames)
    check(log, len(frames) == len(steps), "one frame per letter and per scrolled column (%d, expected %d)"
          % (len(frames), len(steps)))
    if len(frames) != len(steps):
        return log
    # (a limit can be so low that the text is dimmed to nothing: then every frame is dark)
    lit_colour = env["gamma"]
    scale_now = model_scale(env, whole, lit_colour[message["r"]], lit_colour[message["g"]], lit_colour[message["b"]],
                            False)
    dark = not any(dimmed(lit_colour[message[k]], scale_now) for k in "rgb")
    if dark:
        check(log, not any(any(f.mask) for f in frames), "the limit leaves nothing of this text: every frame is dark")
        limit_check(log, env, message, frames)
        message["_long"] = width > PANEL
        hold_check(log, env, message, "O", frames[-1].t, start, end)
        return log
    pictures = all(f.mask[:PANEL] == side(font.columns(text, shown), offset, pad)
                   for f, (shown, offset, _) in zip(frames, steps))
    check(log, pictures, "every frame is the letters so far as Fonts.h has them%s"
          % (", centred (%d blank columns before)" % pad if width < PANEL else
             ", moved left one column at a time once the next letter does not fit"))
    g = env["gamma"]
    dim = model_scale(env, whole, g[message["r"]], g[message["g"]], g[message["b"]], False)
    colour = "%02x%02x%02x" % (carried(dimmed(g[message["r"]], dim)), dimmed(g[message["g"]], dim),
                               carried(dimmed(g[message["b"]], dim)))
    check(log, all(f.colour(c) <= {colour} for f in frames for c in range(COLS)),
          "every lit LED is %s in every frame: the message's colour as this style has always sent it, %s"
          % (colour, dim_note(dim)))
    limit_check(log, env, message, frames)
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
def wave_colours_ok(frame, k, dly, green, blue, dim=255):
    """Frame k of a position: red is the low byte of dly, less 10 per frame, plus 10 per column.
    dim: how far this frame is dimmed (255 = not at all)."""
    green, blue = carried(dimmed(green, dim)), carried(dimmed(blue, dim))
    for c in range(COLS):
        if frame.mask[c]:
            red = carried(dimmed((dly - 10 * k + 10 * (c % PANEL)) & 0xFF, dim))
            if frame.colour(c) != {"%02x%02x%02x" % (red, green, blue)}:
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
    dim = model_scale(env, cols, 0, message["g"], message["b"], True)
    # a text that fits is drawn a frame of the wave apart all the time; one that scrolls, until it moves
    dims = ramp_dims(env, dim, frames, -(-env["hold"] // env["wave"]) if last else len(frames))

    def seen(k, at):
        """What can be seen of the text at `at` in frame k: a column whose colour comes to nothing
        (a dimmed text where the wave's red is at 0, the first frame of a ramp) is dark. One
        picture per dimming the frame may have."""
        out = []
        for d in dims[k]:
            rest = carried(dimmed(message["g"], d)) + carried(dimmed(message["b"], d))
            out.append([v if rest or dimmed((dly - 10 * k + 10 * c) & 0xFF, d) else 0
                        for c, v in enumerate(side(cols, at))])
        return out

    def colours_ok(k, f):
        return any(wave_colours_ok(f, k, dly, message["g"], message["b"], d) for d in dims[k])

    # A text dimmed nearly to nothing shows too little to follow its scroll by eye. Its frames are
    # then compared with where the clock says the text is (one column either way), and the pace,
    # which the other cases check, is not judged from the pictures.
    faint = dim < 32
    if faint and last:
        t0, ok = frames[0].t, True
        for k, f in enumerate(frames):
            since = (f.t - t0) * 1000 - env["hold"]
            at = 0 if since < -1 else min(last, 1 + int((since + 1) // env["step"]))
            ok = ok and any(f.mask[:PANEL] in seen(k, o) for o in (at, max(0, at - 1), min(last, at + 1)))
            if at >= last and not at_end:
                at_end = k
        check(log, ok, "dimmed to %d/256 the text is too faint to follow; every frame is the text as Fonts.h has "
              "it where the clock says it is" % (dim + 1))
        common_done = True
    else:
        common_done = False
    for k, f in enumerate(frames if not common_done else []):
        if offset < last and f.mask[:PANEL] in seen(k, offset + 1) and f.mask[:PANEL] not in seen(k, offset):
            offset += 1
            moves.append(f.t)
            if offset == last:
                at_end = k
        pictures = pictures and f.mask[:PANEL] in seen(k, offset)
        colours = colours and colours_ok(k, f)
    if not common_done:
        check(log, pictures, "every frame is the text as Fonts.h has it, from column 0%s"
              % (", then moved left one column at a time, never back" if last else ""))
    else:
        colours = all(colours_ok(k, f) for k, f in enumerate(frames))
    check(log, colours, "the wave: red starts at the low byte of dly, less 10 per frame, plus 10 per "
          "column; green and blue are the message's; all as this style has always sent them, %s%s"
          % (dim_note(dim), ", coming on over %d ms" % (env["ramp_frames"] * env["wave"])
             if env["ramp_frames"] > 1 else ""))
    limit_check(log, env, message, frames)
    n = env["ramp_frames"]
    if n > 1 and len(frames) > 2 * n:
        # Coming on. The steps of the dimming are even, but what is SENT is not: a value that
        # crosses a power of two goes out half as large again (127 as 127, 128 as 192), so one
        # step can be a frame's share of the level plus up to a third of the level. On top comes
        # what the wave itself moves from one frame to the next once the text is fully on.
        both = [f.load[0] + f.load[1] for f in frames]
        rises = [b - a for a, b in zip([sum(message.get("_before", [0, 0]))] + both, both)]
        flicker = max(rises[n:] + [0])
        # (and no step can be finer than one unit of a channel of every lit LED: at a very low limit
        # that is all there is)
        grain = 3 * max(sum(bin(m).count("1") for m in f.mask) for f in frames)
        bound = max(both) / n + max(both) / 3 + flicker + grain
        check(log, max(rises[:n]) <= bound,
              "it comes on over %d frames: largest rise between two frames %.2f A of a peak of %.2f A (an even "
              "ramp would be %.2f A a frame; the carry quirk allows up to %.2f A)"
              % (n, max(rises[:n]) * AMPS, max(both) * AMPS, max(both) / n * AMPS, bound * AMPS))
    gaps = [(b.t - a.t) * 1000 for a, b in zip(frames, frames[1:])]
    check(log, max(gaps or [0]) <= env["wave"] + 0.5,
          "the wave never stops: no gap between two frames longer than %d ms (longest %.2f ms)"
          % (env["wave"], max(gaps or [0])))
    if last and common_done:
        pass                                              # too faint to judge the pace from the pictures
    elif last:
        check(log, offset == last and side(cols, last)[PANEL - 1] != 0,
              "the text moved %d columns and its last lit column is the panel's last" % last)
        held = (moves[0] - frames[0].t) * 1000
        check(log, abs(held - env["hold"]) <= 1, "its start stood still for %.2f ms (AD_SCROLL_HOLD_MS %d)"
              % (held, env["hold"]))
        steps = [(b - a) * 1000 for a, b in zip(moves, moves[1:])]
        worst = max([abs(s - env["step"]) for s in steps] or [0])
        check(log, worst <= 0.5, "then one column every %d ms (worst %.2f ms off)" % (env["step"], worst))
    elif dly == 1000:
        # The first frame is out 4.4 ms after its start and the hold ends 1000 ms after that; a
        # frame is drawn every AD_WAVE_FRAME_MS as long as 6 ms are left for it. At 10 ms that is
        # frames at 0 to 990 ms: 100, as it always was. At 12 ms: 0 to 996 ms, 84.
        count = int((1000 + 4.4 - 6) // env["wave"]) + 1
        check(log, len(frames) == count, "dly 1000 is %d frames, one every %d ms (%d)"
              % (count, env["wave"], len(frames)))
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
    limit_check(log, env, message, frames)
    font = env[font_name]
    # A limit can be so low that little or nothing is left of the text's colours: then there are no
    # pictures to look at (a marquee's colours run through all values; with less than 24 for the
    # three channels of an LED together, whole stretches of it are dark)
    lit = [bin(v).count("1") for v in font.columns(text_bytes(message))]
    allowed = allowance(env, max([sum(lit[i:i + PANEL]) for i in range(len(lit))] or [0]))
    if allowed < 24:
        log.append("  note  this image's limit leaves an LED of this text %d of 765: too faint to follow, the "
                   "pictures are not checked" % allowed)
        return log
    cols = font.columns(text_bytes(message)[:probe])[:-1]
    check(log, any(find(f.mask, [0] + cols + [0]) for f in frames),
          "%s: a frame shows %s as Fonts.h has it, the heart %d columns wide"
          % (what, json.dumps(message["str"][:probe]), font.heart_cols))
    # Pairs of whole frames with text in them, one straight after the other. (A marquee frame can
    # be a few columns longer or shorter than the strip. A shorter one leaves the columns it does
    # not reach as they were, or, on an image with limits, black: the chonky marquee sends 14
    # characters, which at the end of each character's move is 118 and 119 columns. So the last
    # two columns are left out of the comparison. The starfield after the marquee lights one LED
    # per frame.)
    def whole(f):
        return f.bits >= COLS * 24 and f.lit >= 2
    pairs = [(a, b) for a, b in zip(frames, frames[1:]) if whole(a) and whole(b)]
    left = sum(1 for a, b in pairs if b.mask[:COLS - 3] == a.mask[1:COLS - 2])
    check(log, len(pairs) > 30 and left == len(pairs),
          "it moves one column per frame, through the hearts too (%d of %d pairs of whole frames)"
          % (left, len(pairs)))
    return log


def check_flashy(env, message, start, end, frames, what):
    log = [label(message)]
    limit_check(log, env, message, frames)
    font, text = env["std"], text_bytes(message)
    cols = font.columns(text)[:-1]
    pad = (PANEL - len(cols)) // 2 if len(cols) < PANEL else 0
    lit = [f for f in frames if f.bits == COLS * 24 and f.lit >= 2]
    expected = side(cols, 0, pad)
    hits = [f for f in lit if f.mask[:PANEL] == expected and f.mask[PANEL:] == expected]
    # The text fades from white. Under a limit its top is the brightest white below which every
    # white fits (a fade passes through them all); a limit that allows an LED less than 3 leaves
    # nothing of it, and the sign is then rightly dark.
    allowed = allowance(env, sum(bin(v).count("1") for v in expected))
    top = 0
    while top < 255 and 3 * carried(top + 1) <= allowed:
        top += 1
    if top == 0:
        check(log, not any(f.lit for f in style_frames(message, frames)[0]),
              "the limit leaves nothing of this text (an LED may send %d of 765): every frame is dark" % allowed)
        return log
    # the frames of the fade in which something of a white dimmed to `top` is left
    seen = 3 * sum(1 for count in range(100) if dimmed(env["gamma"][count * 256 // 100], top))
    check(log, len(hits) > min(20, seen // 2), "%s (%d frames)" % (what, len(hits)))
    pause_check(log, lit)
    # The frame that asks most is the fade's step that goes out largest, with its top dimmed to
    # `top`, on every LED of the text: not above it, and not dimmed further than the limit asks
    # either. (Red and blue go out carried; green may or may not.)
    fade = fade_values(env, top)
    leds = sum(bin(m).count("1") for m in expected)
    peak = max([f.load[0] for f in style_frames(message, frames)[0]] or [0])
    check(log, leds * max(2 * carried(v) + v for v in fade) <= peak <= leds * max(3 * carried(v) for v in fade),
          "at its brightest every LED of the text is white %d of 255 as sent (%s): %.2f A a side"
          % (max(carried(v) for v in fade), dim_note(top), peak * AMPS))
    return log


def fade_values(env, top):
    """The whites of the 100 steps of the flashy fade when its top is dimmed to `top`, before they
    are sent. (What is sent does not fall evenly with them: 124 goes out as 126, 119 as 127. So
    the frame that asks most is not always the first.)"""
    return [dimmed(env["gamma"][step * 256 // 100], top) for step in range(100)]


def countdown_string(font, text):
    """The columns of a string as sendString() draws it: 21 characters at most, a blank column
    after each but the string's last (after the first always)."""
    string = []
    for i, ch in enumerate(text[:21]):
        string += font.columns(bytes([ch]))[:-1]
        if i == 0 or i + 1 < len(text):
            string.append(0)
    return string


def countdown_frame(font, text, digits):
    """The 120 columns of a countdown frame as the sketch sends them: the string, the count as
    "d.dd", and both again; what is beyond the strip falls off. digits: three bytes."""
    string = countdown_string(font, text)
    count = []
    for i, ch in enumerate([digits[0], ord("."), digits[1], digits[2]]):
        count += font.columns(bytes([ch]))[:None if i < 3 else -1]
    return ((string + count) * 2 + [0] * COLS)[:COLS]


def check_countdown(env, message, start, end, frames):
    """W: within the limits like every style, and dimmed by exactly what the rule says: a side
    counts as the LEDs of its string and point, and half of what the digits add (every digit each
    place can show, laid over each other); white's top is the brightest below which every white
    fits. So a count of the digits that is too small or too large shows here."""
    log = check_style(env, message, start, end, frames)
    text = text_bytes(message)
    if not text or not (env["limit_both"] or env["limit_side"]):
        return log                                        # (an empty string is drawn past its end)
    font = env["std"]
    lit = lambda cols, a: sum(bin(v & 0xFE).count("1") for v in cols[a:a + PANEL])
    blank = countdown_frame(font, text, b"   ")
    union = list(blank)
    for d in b"0123456789":
        union = [u | v for u, v in zip(union, countdown_frame(font, text, bytes([d, d, d])))]
    counted = max(lit(blank, a) + (lit(union, a) - lit(blank, a) + 1) // 2 for a in (0, PANEL))
    allowed = allowance(env, counted)
    top = 0
    while top < 255 and 3 * carried(top + 1) <= allowed:
        top += 1
    if not any(countdown_string(font, text)):
        return log                                        # a string that lights nothing: only the count is there
    style = style_frames(message, frames)[0]
    # blue is in the string's LEDs only (the count is red) and always goes out carried
    blue = max([int(px[4:6], 16) for f in style for px in f.colours if px and px != "mixed"] or [0])
    expected = max(carried(v) for v in fade_values(env, top))
    check(log, blue == expected, "the string's white is at most %d of 255 as sent in the fade, as the rule gives "
          "for %d LEDs counted on the fuller side (%s): it is %d" % (expected, counted, dim_note(top), blue))
    return log


def check_nudge(env, message, start, end, frames):
    log = [label(message)]
    cols = env["chonk"].columns(text_bytes(message))
    common_checks(log, frames)
    limit_check(log, env, message, frames)
    dim = model_scale(env, cols, 0, message["g"], message["b"], True)
    if dim < 32:
        # a limit so low that the text is dimmed to nothing, or too nearly to tell its positions apart
        if dim == 0:
            check(log, not any(any(f.mask) for f in frames), "the limit leaves nothing of this text: every frame is dark")
        else:
            log.append("  note  dimmed to %d/256 the text is too faint to tell its positions apart" % (dim + 1))
        check(log, end - start < 9 * (message["dly"] / 1000 + 0.02) + 0.1, "the message ends (%.2f s)" % (end - start))
        return log
    # With a ramp the message's first frames can be dark (a small part of a dimmed colour comes to
    # nothing). They are the first position's: a text dimmed to 32/256 or more is dark for at most
    # a 32nd of the ramp. They are left out when the positions are told apart by their pictures.
    lead = 0
    if env["ramp_frames"] > 1:
        while lead < len(frames) - 1 and not any(frames[lead].mask):
            lead += 1
    lit = frames[lead:]
    seen = []
    for f in lit:
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
    appear = ([frames[0].t] + [f.t for before, f in zip(lit, lit[1:]) if f.mask[:PANEL] != before.mask[:PANEL]]
              + [end])
    shown = [(b - a) * 1000 for a, b in zip(appear, appear[1:])]
    # one position to the next is the firmware's own pacing; the last one ends with the sign's next
    # request, which is held to the same tolerance as every hold (2 percent or 10 ms)
    dly = message["dly"]
    worst = max([abs(s - dly) for s in shown[:-1]] or [0])
    last_ok = bool(shown) and abs(shown[-1] - dly) <= max(dly * 0.02, 10)
    check(log, len(shown) == positions and worst <= 1 and last_ok, "each column is shown for dly (%s ms for dly %d)"
          % (" ".join("%.2f" % s for s in shown), message["dly"]))
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


# One message per style, each as full as the style gets and bright where the next style might
# leave it: for the table of what a style shows when it follows another
WHITE = {"r": 255, "g": 255, "b": 255}
EACH_STYLE = [
    dict({"type": "O", "str": "BBBBBBBBBB", "dly": 60}, **WHITE),
    {"type": "C", "str": "BBBBBBB", "dly": 60, "g": 255, "b": 255},
    {"type": "K", "str": "BBBBBBBBB_", "mode": "N"},
    {"type": "F", "str": "BBBBB     BBBBB"},
    {"type": "W", "str": "NEW"},
    {"type": "D", "str": "B"},
    {"type": "N", "str": "B"},
    {"type": "T", "str": "B"},
    dict({"type": "I", "str": "D"}, **WHITE),
    {"type": "H", "str": "BBBBBB", "flashy": "8/8"},  # fits beside the count: it blinks, and asks with '~' only
    {"type": "S", "str": "", "stars": 5},
]


def every_pair(n):
    """A walk through 0..n-1 in which every one follows every one (itself too) exactly once: n * n
    + 1 places. (The cycle that takes every arrow of the complete graph once, by Hierholzer.)"""
    arrows = {a: list(range(n)) for a in range(n)}
    stack, walk = [0], []
    while stack:
        a = stack[-1]
        if arrows[a]:
            stack.append(arrows[a].pop())
        else:
            walk.append(stack.pop())
    return walk[::-1]


def transition_cases():
    walk = every_pair(len(EACH_STYLE))
    assert len(walk) == len(EACH_STYLE) ** 2 + 1 and \
        len(set(zip(walk, walk[1:]))) == len(EACH_STYLE) ** 2, "every style after every style, once"
    cases = [(dict(EACH_STYLE[walk[0]], _note="(first)"), check_style)]
    for a, b in zip(walk, walk[1:]):
        before = EACH_STYLE[a]["type"]
        cases.append((dict(EACH_STYLE[b], _after=before, _note="(after %s)" % before), check_style))
    return cases


def fuzz_cases(n, seed):
    """n messages of the styles the limits hold for, with texts and colours drawn at random: full
    characters, hearts, tabs, bytes outside the font, any length a style takes in a few seconds."""
    rnd = random.Random(seed)
    glyphs = "B#8MW@ .!|ABXYZ0189" + "\u007f\u000b\u0001\u00e9"

    def text(most, least=0):
        full = rnd.random() < 0.4                         # often nothing but the fullest characters
        return "".join(rnd.choice("B#8M\u007f" if full and rnd.random() < 0.8 else glyphs)
                       for _ in range(rnd.randint(least, most)))

    def colour():
        return dict(zip("rgb", rnd.choice([(255, 255, 255), (255, 255, 255), (255, 0, 0), (4, 255, 25),
                                           tuple(rnd.randrange(256) for _ in range(3))])))
    cases = []
    for k in range(n):
        kind = rnd.choice("FFFWWDNTTIKKOC")
        if kind in "FW":
            m = {"type": kind, "str": text(30)}
        elif kind in "DNT":
            m = {"type": kind, "str": text(14)}
        elif kind == "I":
            m = dict({"type": "I", "str": rnd.choice("12XABD")}, **colour())
        elif kind == "K":
            m = {"type": "K", "str": text(rnd.choice((15, 15, 40))) + "_", "mode": rnd.choice("NW")}
        elif kind == "O":
            m = dict({"type": "O", "str": text(14), "dly": rnd.randint(20, 120)}, **colour())
        else:
            m = dict({"type": "C", "str": text(9), "dly": rnd.randint(20, 120), "scroll": False}, **colour())
        cases.append((dict(m, _note="(random %d of seed %d)" % (k, seed)),
                      check_countdown if kind == "W" else check_style))
    return cases


def options(argv):
    """Takes the --name value pairs out of argv. Returns (the other arguments, {name: value})."""
    known = {"--limit-ma": 0, "--limit-side-ma": 0, "--ramp-ms": 0, "--also": None, "--random": 0, "--seed": 1,
             "--same-as": None}
    rest, i = [], 0
    while i < len(argv):
        if argv[i] in known and i + 1 < len(argv):
            known[argv[i]] = argv[i + 1] if argv[i] in ("--also", "--same-as") else int(argv[i + 1])
            i += 2
        elif argv[i].startswith("--"):
            sys.exit("ads_test.py: unknown option %s\n%s" % (argv[i], __doc__))
        else:
            rest.append(argv[i])
            i += 1
    return rest, known


def main():
    sys.argv, opts = options(sys.argv)
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
        # what the image under test was built with (the sketch itself has no limit and no ramp)
        "limit_ma": opts["--limit-ma"], "limit_side_ma": opts["--limit-side-ma"],
        "limit_both": opts["--limit-ma"] * 255 // 20, "limit_side": opts["--limit-side-ma"] * 255 // 20,
    }
    env["ramp_frames"] = opts["--ramp-ms"] // env["wave"]
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
    ]
    # What a side shows is not always one stretch of 60 columns of the text. Each of these was over
    # a limit when the estimate counted the text and not the frame; `also` in the same run.
    tab = "\u000b"
    frames_not_texts = [(m, check_style) for m in (
        # two copies back to back, and side B spans the join: twice the LEDs of any 60 columns
        {"type": "F", "str": "BBBBB     BBBBB", "_note": "(side B spans the join of the two copies)"},
        {"type": "F", "str": "#####     #####", "_note": "(side B spans the join)"},
        {"type": "F", "str": "MMMMM      MMMMM", "_note": "(side B spans the join)"},
        # sendString() stops after 21 characters: the copies are shorter than the text and side A shows two
        {"type": "F", "str": "###" + tab * 42, "_note": "(3 characters and 42 tabs: side A shows the text twice)"},
        # nothing but the count, twice on side A (an empty string is drawn past its end: whatever
        # follows it in memory; a tab is one blank column)
        {"type": "W", "str": "", "_note": "(no text: the count and what follows the string)"},
        {"type": "W", "str": tab, "_note": "(a tab: side A shows the count twice and nothing else)"},
        # a short text with the count: side A shows the text twice and the digits
        {"type": "W", "str": "NEW", "_note": "(side A shows text, count, text)"},
        {"type": "W", "str": "\u007f\u007f\u007f", "_note": "(side A shows text, count, text)"},
        {"type": "W", "str": "NEW" + tab + "MSG" + tab, "_note": "(the notice)"},
        # the keyboard echo, which had no limit at all; a line longer than a panel is drawn past its end
        {"type": "K", "str": "BBBBBBBBB_", "mode": "N", "_note": "(keyboard echo)"},
        {"type": "K", "str": "BBBBBBBBB_", "mode": "W", "_note": "(keyboard echo, warning)"},
        {"type": "K", "str": "BBBBBBBBBBBBB_", "mode": "N", "_note": "(keyboard echo, 14 characters)"},
        # frames shorter than the strip: what the picture before left in the columns they do not reach
        dict({"type": "O", "str": "         B", "dly": 100, "_note": "(leaves a B at the right edge)"}, **WHITE),
        {"type": "T", "str": "B", "_note": "(66 columns a frame; after the O)"},
        dict({"type": "O", "str": "     #####", "dly": 100, "_note": "(leaves ##### on the right)"}, **WHITE),
        {"type": "T", "str": "#####", "_note": "(90 columns a frame; after the O)"},
        {"type": "C", "str": "     ||", "dly": 100, "g": 255, "b": 255, "_note": "(leaves || at the right edge)"},
        dict({"type": "I", "str": "D", "_note": "(103 to 109 columns a frame; after the C)"}, **WHITE),
        dict({"type": "O", "str": "         B", "dly": 100, "_note": "(leaves a B at the right edge)"}, **WHITE),
        dict({"type": "I", "str": "B", "_note": "(106 to 112 columns a frame; after the O)"}, **WHITE),
        dict({"type": "I", "str": "A", "_note": "(an icon with the unused bottom bit set)"}, **WHITE),
        dict({"type": "O", "str": "         B", "dly": 100, "_note": "(leaves a B at the right edge)"}, **WHITE),
        {"type": "N", "str": tab + tab + "B", "_note": "(119 columns a frame; after the O)"},
        dict({"type": "O", "str": "         B", "dly": 100, "_note": "(leaves a B at the right edge)"}, **WHITE),
        {"type": "D", "str": "B", "_note": "(its last frames shrink to nothing; after the O)"})]
    frames_not_texts = [(m, check_countdown if m["type"] == "W" else checker) for m, checker in frames_not_texts]
    rest += [
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
    # The app's own notices (texts and colours from the app's source), for the table of what they
    # ask of the supply, with all the checks of a chonky text
    green, red, pink = {"r": 4, "g": 255, "b": 25}, {"r": 239, "g": 12, "b": 29}, {"r": 255, "g": 0, "b": 128}
    n = lambda s, dly, colour: dict({"type": "C", "str": s, "dly": dly, "scroll": False, "_note": "(app notice)"},
                                    **colour)
    notices = [(m, check_chonky) for m in (
        n("RAVEGPT", 750, green), n("SAYS.", 250, green), n("SAYS..", 250, green), n("SAYS...", 250, green),
        n(".|    ", 250, pink), n("...|  ", 250, pink), n("......", 150, pink),
        n("NO", 650, red), n("NO BARS", 650, red), n("TRYING", 400, red), n("AGAIN", 400, red), n("LATER!", 400, red))]
    # The beat input flashing a marquee white: the largest loads the firmware makes.
    beat = [(dict(m, _note="(beat at 128 a minute)"), check_style) for m in (
        {"type": "D", "str": "I LIVE FOR THAT ENERGY"}, {"type": "N", "str": "CHONKY MARQUEE"},
        {"type": "T", "str": "TRACK ID GOES HERE"})]
    quiet = [(dict(m, _note="(no beat)"), check_style) for m in (
        {"type": "D", "str": "I LIVE FOR THAT ENERGY"}, {"type": "N", "str": "CHONKY MARQUEE"},
        {"type": "W", "str": "NEW MSG"}, {"type": "F", "str": "MIC ON"},
        {"type": "I", "str": "1", "r": 79, "g": 98, "b": 210}, {"type": "I", "str": "2", "r": 79, "g": 98, "b": 210})]
    quiet = [(m, check_countdown if m["type"] == "W" else checker) for m, checker in quiet]
    # The sign's brightness shift ('B' utility message), which halves every value: two short
    # texts that are over this test's limits as they are and under them at shift 1
    shifted = [
        ({"type": "U", "subtype": "B", "str": "1", "_note": "(sets brightness shift 1)"}, check_style),
        (dict(c("HI", 400), g=255, b=25, _shift=1, _note="(at brightness shift 1)"), check_shifted),
        (dict(o("HI THERE!", 400), r=255, g=0, b=0, _shift=1, _note="(at brightness shift 1)"), check_shifted),
    ] + [(dict(m, _note="(at brightness shift 1)"), check_style) for m in (
        {"type": "F", "str": "BBBBB     BBBBB"}, {"type": "W", "str": "NEW"},
        {"type": "K", "str": "BBBBBBBBB_", "mode": "N"}, {"type": "D", "str": "B#B"},
        dict({"type": "I", "str": "1"}, **WHITE))
    ] + [
        ({"type": "U", "subtype": "B", "str": "5", "_note": "(sets brightness shift 5)"}, check_style),
    ] + [(dict(m, _note="(at brightness shift 5)"), check_style) for m in (
        {"type": "F", "str": "BBBBB     BBBBB"}, {"type": "K", "str": "BBBBBBBBB_", "mode": "W"},
        {"type": "N", "str": "B#"}, dict({"type": "I", "str": "D"}, **WHITE))
    ] + [
        ({"type": "U", "subtype": "B", "str": "200", "_note": "(asks for shift 200: stored as 8, dark)"},
         check_dark),
        (dict(o("HI THERE!", 400), r=255, g=0, b=0, _note="(after shift 200 was asked for: dark)"), check_dark),
        ({"type": "U", "subtype": "B", "str": "0", "_note": "(back to no shift)"}, check_style),
        (dict(c("HI", 400), g=255, b=25, _note="(no shift again)"), check_chonky),
    ]
    # the beat flash onto a frame shorter than the strip, with a bright picture left by the style before
    beat += [(dict(m, _note="(beat at 128 a minute; %s)" % note), check_style) for m, note in (
        (dict({"type": "O", "str": "     #####", "dly": 100}, **WHITE), "leaves ##### on the right"),
        ({"type": "T", "str": "#####"}, "90 columns a frame; after the O"))]
    # Messages that are not what the app usually sends: no text, a style's own notices, nothing to
    # draw, a message the sign cannot read (it is nested deeper than the sign's reader goes)
    odd = [(m, check_style) for m in (
        {"type": "F", "_note": "(no text at all)"},
        {"type": "W", "_note": "(no text at all)"},
        {"type": "U", "subtype": "M", "str": "D", "_note": "(the microphone off: MIC OFF)"},
        {"type": "U", "subtype": "M", "str": "E", "_note": "(the microphone on: MIC ON)"},
        {"type": "U", "subtype": "Z", "str": "1", "_note": "(no such utility message: draws nothing)"},
        {"type": "T", "str": "", "_note": "(an empty text: draws nothing)"},
        {"type": "D", "_note": "(no text at all: only the stars)"},
        {"type": "K", "str": "", "mode": "N", "_note": "(an empty line)"},
        {"type": "Z", "str": "B#B", "_note": "(no such type: shown as a marquee)"},
        {"type": "F", "str": "X", "x": [[[[[[[[[[[[1]]]]]]]]]]]], "_error": True, "_stars": 600,
         "_note": "(cannot be read: the sign flashes the error's name, TooDeep, then stars)"})]
    if env["limit_both"] or env["limit_side"]:
        # An icon the sign does not have, with its text: INVALID ICON and the text, as a countdown.
        # (Without limits that text is written into the memory after a string constant and the
        # sign's memory goes: the fonts, the brightness shift. These cases would end that run.)
        odd += [(m, check_style) for m in (
            dict({"type": "I", "str": "Q", "_note": "(no such icon)"}, **WHITE),
            dict({"type": "I", "str": "#" * 321 + "\u00e9", "_note": "(no such icon, a long text)"}, **WHITE),
            {"type": "F", "str": "##########", "_note": "(after the long text of an unknown icon)"})]
        odd += [(dict({"type": "I", "str": "QQQQQQQQQ", "_note": "(no such icon, %d of 8)" % (k + 1)}, **WHITE),
                 check_style) for k in range(8)]
        odd += [({"type": "F", "str": "##########", "_note": "(after eight unknown icons)"}, check_style),
                (dict(c("8888888", 100), g=255, b=255, _note="(after eight unknown icons)"), check_chonky)]
    groups = [("one_by_one", one_by_one, ()), ("chonky", chonky, ()), ("rest", rest + quiet, ()),
              ("notices", notices, ()), ("beat", beat, ("--bpm", "128")), ("shift", shifted, ()),
              ("frames_not_texts", frames_not_texts, ()), ("odd", odd, ())]
    if env["limit_both"] or env["limit_side"]:
        # every style after every style, with the beat input running: only an image with limits has
        # anything to hold there (the table of the transitions is printed for it)
        groups.append(("transitions", transition_cases(), ("--bpm", "128")))
    if opts["--random"]:
        cases = fuzz_cases(opts["--random"], opts["--seed"])
        for i in range(0, len(cases), 20):                # 20 to a run, the runs side by side
            groups.append(("random_%d" % (i // 20), cases[i:i + 20], ("--bpm", "128")))
    if opts["--also"]:
        also = []
        for line in open(opts["--also"], encoding="utf-8"):
            if line.strip().startswith("{"):
                m = dict(json.loads(line), _note="(from %s)" % os.path.basename(opts["--also"]))
                also.append((m, {"O": check_one_by_one, "C": check_chonky}.get(m["type"], check_style)
                             if not m.get("scroll") else check_nudge))
        groups.append(("also", also, ()))
    results = [None] * len(groups)
    order = 0
    for _, cases, _ in groups:
        for message, _ in cases:
            message["_order"] = order   # keys starting with "_" are this script's and are not sent
            order += 1

    def work(i):
        name, cases, flags = groups[i]
        try:
            runs, startup = run_group(env, name, [m for m, _ in cases], port + i, flags)
            logs = []
            if i == 0:
                # (the starfield after the words is taken off them in limit_check, like any other)
                words = {"type": "-", "str": "I LIVE FOR THAT ENERGY!", "_order": -1,
                         "_note": "(the startup words, before the first request)"}
                logs.append(label(words))
                limit_check(logs, env, words, startup)
            for (message, checker), (start, end, frames, before) in zip(cases, runs):
                message["_before"] = before
                logs += checker(env, message, start, end, frames)
            if opts["--same-as"] and name in SAME_GROUPS:
                # the same messages again on the reference image, and the two dumps side by side
                ref_env = dict(env, hex=opts["--same-as"], work=os.path.join(env["work"], "same-as"))
                os.makedirs(ref_env["work"], exist_ok=True)
                ref_runs, _ = run_group(ref_env, name, [m for m, _ in cases], port + 100 + i, flags)
                logs.append("-- the same pictures as %s" % opts["--same-as"])
                logs += same_pictures(cases, runs, os.path.join(env["work"], name + ".jsonl"), ref_runs,
                                      os.path.join(ref_env["work"], name + ".jsonl"),
                                      "the image without limits")
            results[i] = logs
        except Exception as e:  # a run that could not be made or read is a failed test, not a crash
            failures.append("%s: %s" % (name, e))
            results[i] = ["  FAIL  %s: %r" % (name, e)]

    threads = [threading.Thread(target=work, args=(i,)) for i in range(len(groups))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for (name, _, _), logs in zip(groups, results):
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

    print("== what each message asks of the supply (estimate: 20 mA per colour channel at 255; an upper bound)")
    if env["limit_ma"] or env["limit_side_ma"] or env["ramp_frames"] > 1:
        print("   this image: limit %d mA both sides, %d mA one side, ramp %d frames"
              % (env["limit_ma"], env["limit_side_ma"], env["ramp_frames"]))
    print("  %-52s %10s %10s %14s %14s" % ("message", "peak both", "peak side", "biggest rise", "rise, one side"))
    with open(os.path.join(env["work"], "supply.tsv"), "w", encoding="utf-8") as out:
        out.write("message\tpeak_both_A\tpeak_side_A\trise_both_A\trise_side_A\n")
        for _, name, both, one, rise, rise_one in sorted(supply):
            print("  %-52s %8.2f A %8.2f A %12.2f A %12.2f A" % (name[:52], both, one, rise, rise_one))
            out.write("%s\t%.3f\t%.3f\t%.3f\t%.3f\n" % (name, both, one, rise, rise_one))

    if transitions:
        kinds = [m["type"] for m in EACH_STYLE]
        print("== every style after every style: the most one side asks for, in A, while the style of the COLUMN is "
              "shown straight after the style of the ROW (%s are not limited)" % " and ".join(EXEMPT))
        print("  %-8s %s" % ("after \\", " ".join("%5s" % k for k in kinds)))
        for a in kinds:
            print("  %-8s %s" % (a, " ".join("%5.2f" % transitions[(a, b)] if (a, b) in transitions else "    -"
                                              for b in kinds)))

    if failures:
        print("FAILED: %d check(s)" % len(failures))
        for f in failures:
            print("  " + f)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
