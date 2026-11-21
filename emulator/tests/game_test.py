#!/usr/bin/env python3
"""The jump game's frame ('G') on the real firmware image.

  pictures  every frame of a list is drawn as its text reads: the runner (the Dreamstate icon of
            Icons.h) at its column and its rise, cut off at the top edge and at the sides; the
            obstacles at their columns; the colours; the two effects; the moving stars, moved by
            the frame's number, in no column of the play; the score's digits at the top right, in
            the obstacles' colour. One frame of 120 columns for each, both faces equal. Then a
            message of another kind ends the game and is shown.
  misses    ONE table of answers that are not a good frame (absent, torn, no JSON, an object that
            is no message, a frame the sign cannot read, late). For every row: nothing is drawn and
            the picture before stays, the sign says nothing to the app but its request, it asks
            again when the row says, and the frames after it are drawn. And ONE table of good
            frames with bytes that are nobody's in them (noise: what one LOW pulse is received as,
            outside a text and in it): up to CHOOSER_MAX_NOISE of them are dropped and the frame is
            drawn, and one more makes the miss row above. Then no answers at all, and an answer
            that never ends: the sign leaves the game after GAME_LEAVE_MS and asks the ordinary way.
  pace      a scripted game at three reaction times of the app: without the moving stars and
            the score, with the stars, and with the stars and the score: frames a second, and the frame delivery latency: the time from the app having
            something new (a button press) to the frame that carries it being on the LEDs. (When
            the runner is seen to move is the app's matter: its jump has no row yet in the frames
            of its first milliseconds.)

A scripted peer answers each of the sign's requests with the next item of a list; sign-emu runs the
firmware headless and writes a frames dump; everything is checked from that dump. What a picture
must be is worked out here from Icons.h, the shapes, stars and digits below (the contract note's)
and the palette in GameDisplay.h.

usage: game_test.py <sign-emu> <firmware.hex> <repo root> <work dir> [port]
Python 3 standard library only. Uses five TCP ports from `port` (default 14470).
"""
import json
import math
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))
import sign_emu  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ads_test as A  # noqa: E402

COLS, PANEL = A.COLS, A.PANEL
check = A.check
BYTE_S = 10 / 14400.0                   # a byte on the wire
LATENCY_MS = 5                          # sign-emu's reply latency unless a run says otherwise
# The obstacles (the contract note): how many rows each column is high, from the ground
SHAPES = {"a": [1, 2, 1], "b": [3, 3], "c": [1, 2, 3, 2, 1]}
# The moving stars at 0 (the contract note): the column and the row of each; and their colour's place
STARS = [(2, 7), (9, 5), (19, 6), (25, 4), (33, 7), (40, 5), (47, 4), (55, 6)]
STAR_COLOUR = 8
# The score's font (the contract note): each digit's rows from the top row (row 7) down, '#' lit.
# The last digit's right column is 59, a dark column between two digits.
DIGITS = ["### #.# #.# ###", ".#. ##. .#. ###", "##. ..# .#. ###", "### .## ..# ###", "#.# #.# ### ..#",
          "### ##. ..# ##.", "#.. ### #.# ###", "### ..# .#. .#.", "### #.# ### ###", "### #.# ### ..#"]
ITEM = r"[abcr]-?[0-9]{1,2}|p[0-7]{2}|e[0-2]|s[0-9]{1,2}|n[0-9]{1,3}"


def frame_bytes(text, keys=2):
    """The frame as the app sends it. With `keys` above 2: with more keys, which the sign has no use for."""
    message = {"str": text, "type": "G"}
    message.update(("k%d" % n, n) for n in range(keys - 2))
    return json.dumps(message, separators=(",", ":")).encode("ascii")


# ---- what a frame's text draws -----------------------------------------------------------------
def picture(env, text):
    """One face of the frame: for each of the 60 columns (row byte, bit 1 the bottom row; the
    colour digit; the right shift of its colour). None for a text that is no frame."""
    m = re.fullmatch(r"([0-7])([01])((?:%s)*)" % ITEM, text)
    if not m:
        return None
    items = re.findall(ITEM, m.group(3))
    kinds = [i[0] for i in items]
    if any(kinds.count(k) > 1 for k in "rpesn") or sum(k in "abc" for k in kinds) > 4:
        return None
    rise, pose = int(m.group(1)), int(m.group(2))
    runner_col, colours, effect, stars, score = 2, "62", 0, None, ""
    for i in items:
        if i[0] == "r":
            runner_col = int(i[1:])
        elif i[0] == "p":
            colours = i[1:]
        elif i[0] == "e":
            effect = int(i[1:])
        elif i[0] == "n":
            score = i[1:]
        elif i[0] == "s":
            stars = int(i[1:])
            if stars >= PANEL:
                return None
    lit = [set() for _ in range(PANEL)]          # the lit rows of each column, 1 = the bottom row
    colour = [0] * PANEL
    icon = env["runner"][pose * env["runner_cols"]:(pose + 1) * env["runner_cols"]]
    for i, byte in enumerate(icon):
        rows = {r + rise for r in range(1, 8) if byte >> r & 1 and r + rise <= 7}
        if rows and 0 <= runner_col + i < PANEL:
            lit[runner_col + i] |= rows
            colour[runner_col + i] = int(colours[0])
    for i in items:
        if i[0] in SHAPES:
            for n, height in enumerate(SHAPES[i[0]]):
                c = int(i[1:]) + n
                if 0 <= c < PANEL:
                    lit[c] |= set(range(1, height + 1))
                    colour[c] = int(colours[1])
    for d, digit in enumerate(score):          # (its columns are the obstacles': in their colour)
        left = PANEL - 3 - 4 * (len(score) - 1 - d)
        for top, line in enumerate(DIGITS[int(digit)].split()):
            for j, led in enumerate(line):
                if led == "#":
                    lit[left + j].add(7 - top)
                    colour[left + j] = int(colours[1])
    shift = 0
    if effect == 2:
        # the colours first, then the negative: what colour 0 leaves dark is lit like all that is dark
        lit = [set(range(1, 8)) - (rows if colour[c] else set()) for c, rows in enumerate(lit)]
        colour = [int(colours[1])] * PANEL
        shift = env["flash_shift"]
    elif stars is not None:
        # a star only in a column where the play has no row (whatever its colour)
        for column, row in STARS:
            c = (column - stars) % PANEL
            if not lit[c]:
                lit[c], colour[c] = {row}, STAR_COLOUR
    return [(sum(1 << r for r in rows), colour[c], shift) for c, rows in enumerate(lit)], effect


def as_sent(env, digit, shift):
    return "".join("%02x" % (v >> shift) for v in env["palette"][digit * 3:digit * 3 + 3])


def frame_is(env, frame, want, stars=0):
    """None when the frame is this picture on both faces, LED for LED and colour for colour, or
    says what is not. With `stars`: up to that many more LEDs, one to a column the picture leaves
    dark, each in one of the colours 1 to 7; the same on both faces."""
    if frame.bits != COLS * 24 or not frame.clean:
        return "not 120 columns of clean bits (%d bits)" % frame.bits
    if not frame.equal:
        return "the two faces differ"
    if frame.pause > A.MAX_PAUSE_US:
        return "a pause of %.1f us inside the frame" % frame.pause
    extra = 0
    for c, (rows, digit, shift) in enumerate(want):
        sent = as_sent(env, digit, shift)
        if sent == "000000":
            rows = 0
        if stars and not rows and frame.mask[c]:
            extra += 1
            if bin(frame.mask[c]).count("1") != 1 or frame.colours[c] not in env["star_colours"]:
                return "column %d: a star of %s in %s" % (c, bin(frame.mask[c]), frame.colours[c])
            continue
        if frame.mask[c] != rows:
            return "column %d: rows %s, not %s" % (c, bin(frame.mask[c]), bin(rows))
        if rows and frame.colours[c] != sent:
            return "column %d: colour %s, not %s" % (c, frame.colours[c], sent)
    if extra > stars:
        return "%d stars" % extra
    return None


# ---- running the emulator ------------------------------------------------------------------------
class Run:
    """`answers`: what the peer writes for the sign's first request, its second, and so on (bytes;
    with --link-directives a line like @none or @200 in front places it). The run ends at the
    request after the last of them, or after `seconds` of simulated time."""

    def __init__(self, env, name, answers, port, seconds, options=()):
        dump = os.path.join(env["work"], name + ".jsonl")
        log = os.path.join(env["work"], name + ".log")
        emu = sign_emu.Emu(
            [env["emu"], "--headless", "--port", str(port), "--wait-client", "30", "--link-directives",
             "--until-requests", str(len(answers) + 1), "--duration", str(seconds), "--frames", dump]
            + list(options) + [env["hex"]], port, log=log)
        try:
            sock = emu.connect()

            def serve():
                sent = 0
                try:
                    while sent < len(answers):
                        data = sock.recv(4096)
                        if not data:
                            return
                        for byte in data:
                            if byte == ord("~") and sent < len(answers):
                                sock.sendall(answers[sent])
                                sent += 1
                except OSError:
                    pass

            threading.Thread(target=serve, daemon=True).start()
            status = emu.wait(timeout=900)
            sock.close()
        finally:
            emu.end()
        if status != 0:
            raise RuntimeError("%s: sign-emu exited with status %s: %s"
                               % (name, status, open(log).read().strip()[-300:]))
        self.requests, self.said, self.frames, cache = [], "", [], {}
        for line in open(dump, encoding="utf-8"):
            d = json.loads(line)
            if d["type"] == "frame":
                self.frames.append(A.Frame(d, cache))
            elif d["type"] == "tx":
                if d.get("char") == "~":
                    self.requests.append(d["t"])
                else:
                    self.said += d.get("char") or "?"
        self.requests.append(float("inf"))         # (the run ended; nothing was asked after it)

    def asked(self, i):
        return i < len(self.requests) - 1

    def between(self, i, j=None):
        """The frames latched after request i and up to request j (i + 1 unless given)."""
        j = i + 1 if j is None else j
        if not self.asked(i):
            return []
        end = self.requests[min(j, len(self.requests) - 1)]
        return [f for f in self.frames if self.requests[i] < f.t <= end]

    def gap_ms(self, i):
        return (self.requests[i + 1] - self.requests[i]) * 1000 if self.asked(i + 1) else None


def probe():
    return {"type": "O", "str": "TEN CHARS!", "r": 255, "g": 212, "b": 59, "dly": 100, "_order": 1}


def wire(message):
    return json.dumps({k: v for k, v in message.items() if not k.startswith("_")}, separators=(",", ":")).encode("ascii")


def probe_checks(env, log, run, i, message):
    """Request i was answered with an 'O' message: it is drawn as it always was."""
    if not run.asked(i + 1):
        check(log, False, "the %s after it is shown and the sign asks again (it did not)" % A.label(message))
        return
    log += ["  then %s" % A.label(message)] + A.check_one_by_one(
        env, message, run.requests[i], run.requests[i + 1], run.between(i))[1:]


# ---- pictures ----------------------------------------------------------------------------------------
PICTURES = (
    ["%d%d" % (rise, pose) for pose in (0, 1) for rise in range(8)]                 # cut off at the top edge
    + ["30a17b45", "50c30a55", "00a10b20c30a40", "00b30c8a-1c57"]                   # the obstacles
    + ["00a-2", "00a-3", "00c-4", "00c-5", "00b58", "00b59", "00b60", "00c56", "00a99", "00b-99"]   # ... cut at the sides
    + ["00r0", "00r-5", "20r-12", "00r-13", "00r47", "30r50", "00r59", "00r60", "00r-99", "00r99"]  # the runner's column
    + ["00a4", "20c10", "70b2", "00b14a1"]                                           # an obstacle under the runner
    + ["00p34a20", "00p56c20", "00p71b20", "00p11a8", "00p07a20", "00p70a8", "00p00a8", "10p22"]    # colours
    + ["00e0a20", "30e2", "00e2a20b40", "20e2p35c12", "00e2p10", "71e2r-99"]        # the flash
    + ["00p02e2", "20p02e2a20", "00p02e2b8"]         # ... of a runner in colour 0 (the app's hit show): no hole
    + ["00e1", "00e1", "30e1a20b40", "30e1a20b40", "00e1p34r20", "00e1"]            # the sparkle
    + ["70s0", "70s23", "00s0", "00s1", "30a17b45s12", "50s59c30a55", "00c5s7b24", "20r40s33"]     # the moving stars
    + ["00s5e1", "00s6e1", "71e1s30", "00s40e2a20", "00p02s12", "00s12p20b9", "00p00s48a45"]       # ... with the rest
    + ["00e0p21r5a30", "10b7p45e0r30", "71a-99b-99c-99a-99r-99p77e2"]               # any order; the longest
    + ["70n%d" % d for d in range(10)] + ["70n42", "70n999", "70n007"]              # the score: every digit; 2, 3
    + ["00a57n7", "00p34b56n85", "00c48n123", "30r47n123", "20r46p51n88"]           # ... over an obstacle, the runner
    + ["70n999s3", "70p20n58s3", "00p60n9a20", "00e1n7", "00e1n999", "30e1a20n64"]  # ... the stars, colour 0, sparkle
    + ["00e2n36", "00e2p35n9a57", "30e2p02n147s12", "00n5a20s12", "11b30c45e0r5p34n678"]  # ... the flash; any order
    + ["71a-99b-99c-99a-99r-99p77e2s59n999"]                                        # the longest with the score
)


def pictures(env, name, port):
    probe_message = probe()
    answers = [frame_bytes(t) for t in PICTURES] + [wire(probe_message)]
    run = Run(env, name, answers, port, 60)
    log = ["%d game frames, each drawn once as its text reads, on both faces" % len(PICTURES)]
    wrong, starry = [], []
    for i, text in enumerate(PICTURES):
        want, effect = picture(env, text)
        frames = run.between(i)
        problem = "%d frames" % len(frames) if len(frames) != 1 else \
            frame_is(env, frames[0], want, env["stars"] if effect == 1 else 0)
        if problem:
            wrong.append("%s: %s" % (text, problem))
        elif effect == 1:
            # (the stars of this frame: what is lit where the picture itself has nothing)
            starry.append(frozenset((c, frames[0].mask[c]) for c, (rows, _, _) in enumerate(want)
                                    if not rows and frames[0].mask[c]))
    check(log, not wrong, "every frame is its picture and nothing else (%d are not%s)"
          % (len(wrong), ": " + "; ".join(wrong[:4]) if wrong else ""))
    at0, moved = [run.between(PICTURES.index(t))[0].mask[:PANEL] for t in ("70s0", "70s23")]
    check(log, sum(bin(m).count("1") for m in at0) == len(STARS) and not any(m & 0x0E for m in at0)
          and all(moved[c] == at0[(c + 23) % PANEL] for c in range(PANEL)),
          "the moving stars: %d LEDs a face, none in rows 1 to 3, and at 23 the picture at 0 moved 23 columns "
          "to the left and around" % sum(bin(m).count("1") for m in at0))
    check(log, len(starry) > 1 and len(set(starry)) == len(starry) and sum(map(len, starry)) >= 2 * len(starry),
          "the sparkle has stars, and other ones in every frame (%d stars in its %d frames, %d different sets of them)"
          % (sum(map(len, starry)), len(starry), len(set(starry))))
    check(log, run.said == "", "the sign sends nothing but its requests (%r)" % run.said)
    probe_checks(env, log, run, len(PICTURES), probe_message)
    return log


# ---- misses ------------------------------------------------------------------------------------------
# What the app writes in place of a good frame, and when the sign must ask again, counted from its
# request: the third column is how many bytes arrive (after the app's reaction time), the fourth
# the silence it then waits out. An answer that ends as a whole object needs only the quiet time; one that stops, or
# never begins, needs the wait for a byte first.
def noisy(text, n):
    """The frame of `text` with the first n of nine bytes that are nobody's in it, in this order:
    0xFF in front of the brace, 0x01 behind it, 0x02 in front of the colon, 0x03 behind it, 0x80
    inside the text (behind its first character: it begins no UTF-8 character), 0x7F behind the
    text, 0x04 behind the comma, 0x05 and 0x06 in front of the closing brace."""
    t = text.encode("ascii")
    noise = [b"\xff", b"\x01", b"\x02", b"\x03", b"\x80", b"\x7f", b"\x04", b"\x05", b"\x06"]
    pieces = [b"{", b'"str"', b":", b'"' + t[:1], t[1:] + b'"', b",", b'"type":"G"', b"", b"}"]
    return b"".join((noise[k] if k < n else b"") + piece for k, piece in enumerate(pieces))


# Good frames with bytes that are nobody's in them. The sign reads a frame as it reads every message
# (ChooserReader): it drops up to CHOOSER_MAX_NOISE such bytes from a frame before the frame is read,
# so each of these is DRAWN, and the sign asks again at once. (Master before lm-blf read the game's
# frames with a reader of their own, and each of these was a miss.) Nine are a miss: miss_table.
def noise_table():
    def one_in_text(t):
        return b'{"str":"' + t[:1].encode("ascii") + b"\x80" + t[1:].encode("ascii") + b'","type":"G"}'
    return [
        ("one LOW pulse in front of the brace (0xFF)", lambda t: b"\xff" + frame_bytes(t)),
        ("a control byte between the two keys (0x01)", lambda t: frame_bytes(t).replace(b'",', b'",\x01', 1)),
        ("a heart behind the brace, outside a text (0x7F)", lambda t: b"{\x7f" + frame_bytes(t)[1:]),
        ("a byte in the text that begins no UTF-8 character (0x80)", one_in_text),
        ("eight of them, outside the text and in it: the most one frame may lose", lambda t: noisy(t, 8)),
    ]


def miss_table(env):
    miss, quiet = env["miss"], env["quiet"]
    whole = [
        ("no JSON", b'{"str"="30a17","type":"G"}'),
        ("an object that is no message", b'{"c":4660}'),
        ("an empty object", b'{}'),
        ("a frame without a text", b'{"type":"G"}'),
        ("a type that is no text", b'{"str":"30a17","type":7}'),
        ("an empty type", b'{"str":"30a17","type":""}'),
        ("a frame with nine keys", frame_bytes("30a17", 9)),
        ("a frame with no rise", frame_bytes("")),
        ("a rise of 8", frame_bytes("80a17")),
        ("a shape the sign does not have", frame_bytes("30d17")),
        ("stars moved by 60", frame_bytes("30a17s60")),
        ("five obstacles", frame_bytes("30a1a9b20b30c40")),
        ("a column of three digits", frame_bytes("30a100")),
        ("a score of four digits", frame_bytes("30a17n1234")),
        ("nine bytes that are nobody's, outside the text and in it: one more than a frame may lose",
         noisy("30a17", 9)),
    ]
    rows = [("absent", b"@none\n", 0, miss + quiet),
            # (a byte that is dropped gives the next one the time any byte has: so noise and then
            # silence is a miss that much behind the noise, as a torn answer is)
            ("noise and then nothing: one LOW pulse (0xFF), no frame", b"\xff", 1, miss + quiet),
            ("torn in the text", b'{"str":"30a17b4', len(b'{"str":"30a17b4'), miss + quiet),
            ("torn after the text", b'{"str":"30a17b45","ty', len(b'{"str":"30a17b45","ty'), miss + quiet)]
    rows += [(what, data, len(data), quiet) for what, data in whole]
    return rows


def misses(env, name, port):
    rows = miss_table(env)
    late_ms = env["miss"] + env["quiet"] + 30         # past the wait for an answer, into the next request
    leave = math.ceil(env["leave"] / (env["miss"] + env["quiet"])) + 1
    probe_message = probe()
    # A good frame, then for every row: its answer and a good frame. Every good frame is different.
    good = ["%d0a%d" % (n % 7, 12 + 2 * n) for n in range(len(rows) + 8)]
    answers, at = [frame_bytes(good[0])], []
    for n, (_, data, _, _) in enumerate(rows):
        at.append(len(answers))
        answers += [data, frame_bytes(good[n + 1], 8)]      # (eight keys are the most a frame may have)
    # The good frames with noise in them, each followed by another good frame (other shapes than
    # the frames above, so that every picture of the run is different).
    noise = noise_table()
    noisy_texts = ["%d0b%d" % (n % 7, 14 + 4 * n) for n in range(len(noise))]
    noisy_next = ["%d1c%d" % (n % 7, 16 + 4 * n) for n in range(len(noise))]
    noisy_at = []
    for n, (_, make) in enumerate(noise):
        noisy_at.append(len(answers))
        answers += [make(noisy_texts[n]), frame_bytes(noisy_next[n], 8)]
    late_at = len(answers)
    after_late = good[len(rows) + 1:len(rows) + 7]
    answers += [b"@%d\n" % late_ms + frame_bytes(after_late[0])] + [frame_bytes(t) for t in after_late[1:]]
    last_good = len(answers) - 1
    answers += [b"@none\n"] * leave + [wire(probe_message)]
    run = Run(env, name, answers, port, 90)

    def drawn(i, text):
        frames = run.between(i)
        return len(frames) == 1 and frame_is(env, frames[0], picture(env, text)[0]) is None

    log = ["answers that are not a good frame: nothing is drawn, nothing is said, the sign asks again"]
    check(log, run.said == "", "in the whole run the sign sends nothing but its requests: no error text (%r)" % run.said)
    check(log, drawn(0, good[0]), "the first frame is drawn")
    for n, (what, data, size, silence) in enumerate(rows):
        i = at[n]
        gap = run.gap_ms(i)
        # the request byte, the app's reaction, the bytes, the silence. The sign counts the silence in
        # the milliseconds of its own clock (so up to 2.5 ms less) and has up to 4 ms of work of its own.
        due = (1 + size) * BYTE_S * 1000 + (LATENCY_MS if size else 0) + silence
        ok = (gap is not None and due - 2.5 <= gap <= due + 4 and not run.between(i) and drawn(i + 1, good[n + 1]))
        check(log, ok, "%s: no frame, asked again after %s ms (%.0f to %.0f), then the next frame is drawn%s"
              % (what, "%.1f" % gap if gap is not None else "never", due - 2.5, due + 4,
                 "" if ok or gap is None else " (%d frames in it; the next frame %s)"
                 % (len(run.between(i)), "drawn" if drawn(i + 1, good[n + 1]) else "NOT drawn")))

    for n, (what, make) in enumerate(noise):
        i = noisy_at[n]
        gap = run.gap_ms(i)
        ok = drawn(i, noisy_texts[n]) and drawn(i + 1, noisy_next[n]) and gap is not None and gap < env["miss"]
        check(log, ok, "a good frame with %s (%d bytes): the frame is drawn, the sign asks again at once (%s ms) "
              "and draws the next" % (what, len(make(noisy_texts[n])), "%.1f" % gap if gap is not None else "never"))

    # Late: the answer comes when the sign has given it up and asked again. It is a good frame
    # and may be drawn; whatever is drawn after it is a frame that was sent, in the order sent; and
    # the last frames are drawn each in its own exchange again.
    sent = [picture(env, t)[0] for t in after_late]
    shown, order_ok = [], True
    for f in run.between(late_at, last_good + 1):
        which = [k for k, want in enumerate(sent) if frame_is(env, f, want) is None]
        order_ok = order_ok and bool(which) and (not shown or which[0] > shown[-1])
        shown += which[:1]
    in_step = drawn(last_good - 1, after_late[-2]) and drawn(last_good, after_late[-1])
    took = (run.requests[last_good + 1] - run.requests[late_at]) * 1000 if run.asked(last_good + 1) else None
    check(log, order_ok and in_step and not run.between(late_at),
          "an answer %d ms late: nothing is drawn while it is awaited, every frame drawn after it is one that "
          "was sent, in the order sent (frames %s of 0 to %d), and the last two are drawn in their own exchanges "
          "again (%s ms for the %d requests)"
          % (late_ms, ",".join(map(str, shown)) or "none", len(sent) - 1,
             "%.0f" % took if took is not None else "?", len(after_late)))

    # No answers at all: the sign asks quickly until GAME_LEAVE_MS is over, then the ordinary way
    # (it waits a second for the answer and shows stars), and reads the message after that.
    quick = [k for k in range(last_good + 1, last_good + 1 + leave) if (run.gap_ms(k) or 1e9) < 500]
    ordinary = last_good + 1 + len(quick)
    drawn_at = run.between(last_good)[-1].t if run.between(last_good) else None
    left_after = (run.requests[ordinary] - drawn_at) * 1000 if drawn_at is not None and run.asked(ordinary) else None
    # (the sign counts in the milliseconds of its own clock: up to 2.5 ms less, as in the rows above)
    check(log, left_after is not None
          and env["leave"] - 2.5 <= left_after <= env["leave"] + env["miss"] + env["quiet"] + 10
          and not run.between(last_good + 1, ordinary),
          "no answers: the last picture stays and the sign asks every %s ms, %d times; %s ms after the last good "
          "frame (%.1f to %d) it leaves the game"
          % ("/".join(sorted({"%.0f" % run.gap_ms(k) for k in quick})) or "?", len(quick),
             "%.1f" % left_after if left_after is not None else "?", env["leave"] - 2.5,
             env["leave"] + env["miss"] + env["quiet"] + 10))
    stars = run.between(ordinary)
    gap = run.gap_ms(ordinary)
    check(log, gap is not None and gap >= 1000 and len(stars) > 100 and all(sum(map(bool, f.mask)) <= 1 for f in stars),
          "then it asks the ordinary way: it waits a second for an answer and shows stars (%s ms, %d frames)"
          % ("%.0f" % gap if gap is not None else "?", len(stars)))
    probe_checks(env, log, run, ordinary + 1, probe_message)

    # An answer that never ends: bytes that keep coming and never make an object. The sign stops
    # reading GAME_LEAVE_MS after the last good frame, whatever arrives, and asks the ordinary way.
    # (A run of its own: what the ordinary request then reads is the rest of these bytes.)
    endless = b'{"str":"30a17",' + b" " * int((env["leave"] + 500) / 1000 / BYTE_S)
    run = Run(env, name + "_endless", [frame_bytes(good[0]), endless], port, 30)
    drawn_at = run.between(0)[-1].t if drawn(0, good[0]) else None
    left_after = (run.requests[2] - drawn_at) * 1000 if drawn_at is not None and run.asked(2) else None
    # When the sign stops reading is seen in the time of its next request, the ordinary one: a
    # firmware that asks at once asks GAME_LEAVE_MS after the last good frame. A firmware that waits
    # for a quiet line before every request (REQUEST_QUIET_MS) cannot ask while these bytes are still
    # arriving: it asks when they are over and it has listened CHOOSER_ATTN_FIRST_BYTE_MS behind the
    # last LOW it saw. Its request then says only that the game asked nothing more and drew nothing.
    low, high, what = env["leave"] - 2.5, env["leave"] + 6, "the sign reads no more of it and asks the ordinary way"
    if env["request_quiet"] and drawn_at is not None and run.asked(1):
        over = (run.requests[1] - drawn_at) * 1000 + LATENCY_MS + (1 + len(endless)) * BYTE_S * 1000
        low, high = max(low, over + env["attn"] - 2.5), max(high, over + env["attn"] + 6)
        what = ("the game has asked nothing more, and the ordinary request waits for the line: it goes out "
                "%d ms behind the blanks' end (%.1f ms)" % (env["attn"], over))
    check(log, left_after is not None and low <= left_after <= high and not run.between(1) and run.said == "",
          "an answer that never ends (blanks for %.2f s): the last picture stays, nothing is said, and %s ms after "
          "the last good frame (%.0f to %.0f) %s"
          % (len(endless) * BYTE_S, "%.1f" % left_after if left_after is not None else "?", low, high, what))
    return log


# ---- pace --------------------------------------------------------------------------------------------
def game_script(n, stars=False, score=False):
    """A game as the app would send it: two obstacles coming in from the right, a jump over each,
    the runner's two pictures in turn. With `stars`: and the moving stars, a column every 4 frames.
    With `score`: and a score in every frame, the frame's number (one, two and three digits)."""
    jump = [1, 3, 5, 6, 7, 7, 7, 7, 6, 5, 3, 1]
    out = []
    for k in range(n):
        first, second = 59 - k % 65, 59 - (k + 33) % 65
        text = "%d%d" % (max([jump[j] for j in range(len(jump)) if 20 - j in (first, second)] or [0]), k // 4 % 2)
        text += "abc"[k // 65 % 3] + str(first) + "bca"[(k + 33) // 65 % 3] + str(second)
        out.append(text + ("s%d" % (k // 4 % PANEL) if stars else "") + ("n%d" % k if score else ""))
    return out


def pace(env, name, port, latency):
    return (pace_of(env, name, port, latency, False, False) + pace_of(env, name + "_stars", port, latency, True, False)
            + pace_of(env, name + "_stars_score", port, latency, True, True))


def pace_of(env, name, port, latency, stars, score):
    script = game_script(240, stars, score)
    run = Run(env, name, [frame_bytes(t) for t in script], port, 60, ("--link-latency-ms", str(latency)))
    log = ["a game of %d frames%s%s, the app answering %d ms after each request"
           % (len(script), " with the moving stars" if stars else "", " and the score" if score else "", latency)]
    wrong = [t for i, t in enumerate(script) if len(run.between(i)) != 1
             or frame_is(env, run.between(i)[0], picture(env, t)[0]) is not None]
    check(log, not wrong and run.said == "", "every frame is drawn once, as its text reads (%d are not)" % len(wrong))
    check(log, run.asked(len(script)), "after the last frame the sign asks for the next one")
    if wrong or not run.asked(len(script)):
        return log
    shown = [run.between(i)[0].t for i in range(len(script))]
    heard = [run.requests[i] + BYTE_S for i in range(len(script))]         # the app has the request
    rate = (len(shown) - 1) / (shown[-1] - shown[0])
    exchange = [(b - a) * 1000 for a, b in zip(shown, shown[1:])]
    # Frame delivery latency. What the app has before it hears request i (a press) goes into
    # answer i and is on the LEDs when that frame is latched. For every moment of the game alike:
    soonest = min(s - h for s, h in zip(shown[1:], heard[1:])) * 1000
    latest = max(s - h for s, h in zip(shown[1:], heard)) * 1000
    span = heard[-1] - heard[0]
    mean = sum((b - a) * (s - (a + b) / 2) for a, b, s in zip(heard, heard[1:], shown[1:])) / span * 1000
    sizes = [len(frame_bytes(t)) for t in script]
    log.append("        frames of %d to %d bytes: %.1f frames a second (a frame every %.1f to %.1f ms); frame "
               "delivery latency (the app has a press -> the frame with it is on the LEDs) %.0f to %.0f ms, "
               "%.0f ms on average"
               % (min(sizes), max(sizes), rate, min(exchange), max(exchange), soonest, latest, mean))
    env["measured"][latency, stars, score] = (rate, soonest, mean, latest)
    check(log, rate >= env["least_rate"] and latest <= 2000 / env["least_rate"],
          "at least %d frames a second, and no frame is delivered later than two frames at that rate (%d ms)"
          % (env["least_rate"], 2000 / env["least_rate"]))
    return log


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    root = sys.argv[3]

    def read(name):
        return open(os.path.join(root, name), encoding="utf-8", errors="replace").read()

    fonts, sketch, display = read("Fonts.h"), read("LED_Marquee.ino"), read("GameDisplay.h")
    gamma = A.table(read("Gamma.h"), "gamma[]")
    std = A.table(fonts, "FontStd5x7[]")
    icon = A.table(read("Icons.h"), "DREAMSTATE =")
    palette = A.table(display, "gamePalette[")
    flash = re.search(r"^#define\s+GAME_FLASH_SHIFT\s+(\d+)\s*$", display, re.M)
    stars = re.search(r"^#define\s+GAME_SPARKLE_STARS\s+(\d+)\s*$", display, re.M)
    if len(icon) != 1 + 2 * icon[0] or len(palette) != 27 or not flash or not stars or len(std) != 95 * 5 + 7:
        sys.exit("Icons.h, GameDisplay.h or Fonts.h is not what this test expects")
    env = {
        "emu": sys.argv[1], "hex": sys.argv[2], "work": sys.argv[4],
        "std": A.Font(std, 5, 7), "gamma": gamma,
        "letter": A.setting(sketch, "CHARS_ONEBYONE_DELAY"), "step": A.setting(sketch, "AD_SCROLL_STEP_MS"),
        "hold": A.setting(sketch, "AD_SCROLL_HOLD_MS"), "wave": A.setting(sketch, "AD_WAVE_FRAME_MS"),
        "miss": A.setting(sketch, "GAME_MISS_MS"), "quiet": A.setting(sketch, "GAME_QUIET_MS"),
        "leave": A.setting(sketch, "GAME_LEAVE_MS"),
        # (the wait for a quiet line before every request, which ads_test.py's checks of the probe
        # want; and how long such a firmware listens behind the last LOW it saw before it asks)
        "request_quiet": A.request_quiet(sketch), "attn": A.setting(sketch, "CHOOSER_ATTN_FIRST_BYTE_MS", 0),
        "runner": icon[1:], "runner_cols": icon[0], "palette": palette,
        "flash_shift": int(flash.group(1)), "stars": int(stars.group(1)),
        "measured": {}, "least_rate": 20,
    }
    env["star_colours"] = {as_sent(env, d, 0) for d in range(1, 8)}
    port = int(sys.argv[5]) if len(sys.argv) > 5 else 14470
    os.makedirs(env["work"], exist_ok=True)

    groups = [("pictures", pictures), ("misses", misses)] + [
        ("pace_%d" % ms, lambda name, port, ms=ms: pace(env, name, port, ms)) for ms in (3, 5, 9)]
    groups = [(name, f if name.startswith("pace") else (lambda n, p, f=f: f(env, n, p))) for name, f in groups]
    results = [None] * len(groups)

    def work(i):
        name, f = groups[i]
        try:
            results[i] = f(name, port + i)
        except Exception as e:  # a run that could not be made or read is a failed test, not a crash
            A.failures.append("%s: %s" % (name, e))
            results[i] = ["  FAIL  %s: %r" % (name, e)]

    threads = [threading.Thread(target=work, args=(i,)) for i in range(len(groups))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for (name, _), logs in zip(groups, results):
        print("== %s" % name)
        print("\n".join(logs))

    if A.failures:
        print("FAILED: %d check(s)" % len(A.failures))
        for f in A.failures:
            print("  " + f)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
