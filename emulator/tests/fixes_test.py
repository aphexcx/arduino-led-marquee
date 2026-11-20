#!/usr/bin/env python3
"""Messages the sign must survive and draw as they read, on the real firmware image:

  - 'U' 'B' (brightness shift): 0 to 8 are stored as they come, more than 8 is 8 (dark) however
    many digits the number has, below 0 and "not a number" are 0, an empty text changes nothing
  - 'K' (keyboard echo): a line longer than 10 characters shows its last ten and nothing else; a
    line of 256 characters or more ends
  - 'F' and 'W' with an empty text draw nothing beside what the style adds (the count)
  - 'S' with a negative number of stars draws none
  - 'I' with an icon the sign does not have says so, any number of times and at any length, and
    the sign goes on reading messages

Like ads_test.py: a scripted peer answers each of the sign's requests with the next message of a
list, sign-emu runs the firmware headless and writes a frames dump, and everything is checked
from that dump. Every list has a limit of simulated time, so a message that never ends is a
failed check and not a test that never ends.

HOW A MESSAGE IS CHECKED. What a message draws is worked out here, frame for frame, by doing what
the sketch's senders do (sendChar, sendString, showAsCountdownStyle, showAsInputStyle, ...) with
Fonts.h, Gamma.h and the brightness shift: a message is a list of PARTS in their order (the clear,
the fade, the last blink, the starfield, ...), a part is a list of frames, and a frame is the
whole strip: for each of its 120 columns the rows that are lit and their colour. The frames the
sign drew are held against that list one for one. So a part that is missing, shorter, longer,
dark, in another place or running the other way is a failed check; and so is a picture that is
right but not there in every frame it must be in. tests/fixes_selftest.py holds these checks to
that: it feeds them the right frames with one part changed, and each must fail.

A star of the starfield is where the sign's random numbers put it, and those are worked out
too: the sketch never seeds them, so they are avr-libc's random() from its first number on, the
same in every run, and each list counts the stars drawn before a message (the 300 of the
starfield at power-on first).

usage: fixes_test.py <sign-emu> <firmware.hex> <repo root> <work dir> [port]
Python 3 standard library only. Uses seven TCP ports from `port` (default 14460).
"""
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))
import sign_emu  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ads_test as A  # noqa: E402

COLS, PANEL, VT = A.COLS, A.PANEL, A.VT
check = A.check

# The lengths the sketch calls its animations with. They are not #defines of LED_Marquee.ino; a
# change of one of them changes how long a message is on the sign, and fails this test on purpose.
FLASHY_FRAMES = 200         # showAsFlashyStyle(): `time`, for 'F' and for the answer to 'U'
COUNT_FRAMES = 600          # showAsCountdownStyle(): `count`, for 'W'
ICON_COUNT_FRAMES = 100     # the unknown icon: showAsCountdownStyle(invalid, 100)
ICON_NAME_BYTES = 21        # ... and how much of the icon's name goes into its text (strncat)
BLINK_FRAMES = 100          # showAsCountdownStyle(): "One last farewell blink"
STARS_AFTER = 300           # showstarfield(), after 'F' and 'W'
KEYBOARD_COUNTS = (1, 0)    # showAsInputStyle(): `count` in its two frames
KEYBOARD_CHARS = 10         # MAX_CHARS_PER_PANEL
COUNT_RED = 0x80            # the red of the count while it runs
STAR_RGB = (0x40, 0x40, 0xFF)   # "Starlight blue"
STARS_AT_START = 300        # setup(): showstarfield(), before the sign reads its first message
SHIFT_MAX = 8               # BRIGHTNESS_SHIFT_MAX


# ---- running the emulator -------------------------------------------------------------------------
def run(env, name, messages, port, seconds):
    """Serves `messages` once, for at most `seconds` of simulated time. Returns for each message
    (request time, next request time, frames); the times are None from where the sign stopped
    asking. Bytes the sign sent that are not a request are an error, and so is an emulator that
    does not end with status 0 (3 is a firmware that crashed)."""
    dump = os.path.join(env["work"], name + ".jsonl")
    log = os.path.join(env["work"], name + ".log")
    emu = sign_emu.Emu(
        [env["emu"], "--headless", "--port", str(port), "--wait-client", "30",
         "--until-requests", str(len(messages) + 1), "--duration", str(seconds), "--frames", dump,
         env["hex"]], port, log=log)
    try:
        sock = emu.connect()

        def serve():
            sent = 0
            try:
                while sent < len(messages):
                    data = sock.recv(4096)
                    if not data:
                        return
                    for byte in data:
                        if byte == ord("~") and sent < len(messages):
                            wire = json.dumps({k: v for k, v in messages[sent].items() if not k.startswith("_")},
                                              separators=(",", ":"), ensure_ascii=True)
                            sock.sendall(wire.encode("ascii"))
                            sent += 1
            except OSError:
                pass

        peer = threading.Thread(target=serve, daemon=True)
        peer.start()
        status = emu.wait(timeout=900)
        sock.close()
    finally:
        emu.end()
    if status != 0:
        raise RuntimeError("%s: sign-emu exited with status %s: %s"
                           % (name, status, open(log).read().strip()[-300:]))
    return read_dump(name, dump, len(messages))


def read_dump(name, dump, messages):
    """The frames of each of `messages` messages from a frames dump: see run()."""
    requests, frames, cache = [], [], {}
    for line in open(dump, encoding="utf-8"):
        d = json.loads(line)
        if d["type"] == "frame":
            frames.append(A.Frame(d, cache))
        elif d["type"] == "tx":
            if d.get("char") != "~":
                raise RuntimeError("%s: the sign sent %r: it could not read a message" % (name, d.get("char")))
            requests.append(d["t"])
    out = []
    for i in range(messages):
        start = requests[i] if i < len(requests) else None
        end = requests[i + 1] if i + 1 < len(requests) else None
        inside = [] if start is None else [f for f in frames if start < f.t <= (end if end is not None else 1e18)]
        out.append((start, end, inside))
    return out


def asked_again(log, start, end, within=None):
    """The message ended: the sign asked for the next one."""
    if start is None:
        check(log, False, "the sign asked for this message (it had stopped asking before)")
        return False
    if end is None:
        check(log, False, "the message ends: the sign asks for the next one (it never did)")
        return False
    took = end - start
    if within is None:
        check(log, True, "the message ends: the sign asks for the next one after %.2f s" % took)
    else:
        check(log, took <= within, "the message ends: the sign asks for the next one after %.3f s "
              "(at most %.2f s)" % (took, within))
    return True


# ---- a frame as worked out, and a frame as drawn --------------------------------------------------
def as_sent(rgb, shift, green_carried=False):
    """The three bytes an LED asked to be (r, g, b) goes out as, 'rrggbb', as this firmware has
    always sent them. sendColumnRGB() shifts each value right by the brightness shift and hands it
    to sendBitx8 (PixelBitBanging.h), which walks a bit down the byte with `ror`. When it starts
    with the carry flag set, a second bit walks along and the byte goes out as v | v >> 1. The
    flag is what the code before left:
      - with a shift of 1 or more, the last bit the shift pushed out of the value;
      - with no shift, what the byte before left: red and blue follow a sendBitx8, which ends with
        the flag set; green, the first of the three, follows the code between two columns, which
        leaves it clear, except in the starfield, where it is left set (`green_carried`).
    (ads_test.py holds the ads' colours to the same: red and blue carried, green as it is.)"""
    if shift == 0:
        flags = (True, green_carried, True)
    else:
        flags = tuple(v >> (shift - 1) & 1 for v in rgb)
    return "%02x%02x%02x" % tuple(A.carried(v >> shift) if c else v >> shift for v, c in zip(rgb, flags))


class Want:
    """What the strip must show when one frame has latched: for each of its 120 columns the rows
    lit (bit 7 is the top row, as in Fonts.h) and their colour as sent. `sent` is the number of
    columns the frame sends, 24 bits each: a frame is that long to the bit (columns past the
    strip's 120th are sent and fall off its end; a frame shorter than the strip leaves the
    columns it does not reach as they were). `whole` says of a frame that it is clear()."""
    __slots__ = ("cols", "rows", "lit", "sent", "whole")

    def __init__(self, cols, sent, whole=False):
        self.cols = cols                                   # [(rows, 'rrggbb' or None)] * 120
        self.rows = [rows for rows, _ in cols]
        self.lit = [(c, rgb) for c, (rows, rgb) in enumerate(cols) if rows]
        self.sent, self.whole = sent, whole

    def why(self, f):
        """None when the frame f is this picture, else what is wrong with it."""
        if not f.clean:
            return "its bits are not clean"
        if f.bits != self.sent * 24:
            return "it is %d bits long, not the %d of its %d columns" % (f.bits, self.sent * 24, self.sent)
        if f.mask != self.rows:
            c = next(c for c in range(COLS) if f.mask[c] != self.rows[c])
            return "column %d has the rows %02x lit, not %02x" % (c, f.mask[c], self.rows[c])
        for c, rgb in self.lit:
            if f.colours[c] != rgb:
                return "column %d is %s, not %s" % (c, f.colours[c], rgb)
        return None


class Part:
    """A part of a message: its frames in their order, each a Want."""
    def __init__(self, what, wants):
        self.what, self.wants = what, wants


def check_parts(log, frames, parts):
    """The frames of a message are its parts, in their order, each as long as worked out and
    each of its frames the picture worked out for it."""
    total = sum(len(p.wants) for p in parts)
    check(log, len(frames) == total, "%d frames: %s (drawn: %d)"
          % (total, " + ".join("%d of %s" % (len(p.wants), p.what) for p in parts) or "it draws nothing",
             len(frames)))
    at = 0
    for p in parts:
        got = frames[at:at + len(p.wants)]
        at += len(p.wants)
        wrong = [(i, why) for i, why in ((i, w.why(f)) for i, (f, w) in enumerate(zip(got, p.wants))) if why]
        notes = []
        if len(got) != len(p.wants):
            notes.append("%d of its frames are there" % len(got))
        if wrong:
            notes.append("%d are not; the first is its frame %d: %s" % (len(wrong), wrong[0][0] + 1, wrong[0][1]))
        check(log, not notes, "%s: %d frame%s, each as worked out%s"
              % (p.what, len(p.wants), "" if len(p.wants) == 1 else "s", ": " + "; ".join(notes) if notes else ""))


# ---- what the sketch's own senders put on the strip -------------------------------------------------
class Strip:
    """The strip as the sketch's senders leave it, frame after frame. Every colour value is
    shifted right by the brightness shift before it is sent (sendColumnRGB); an LED whose three
    values are 0 is dark. A column that a frame does not reach keeps what it showed."""
    def __init__(self, shift=0):
        self.shift = shift
        self.now = [None] * COLS                # not known before the first clear()

    def frame(self, columns, whole=False, green_carried=False):
        """One frame: `columns` are the calls of sendColumnRGB(), (rows, (r, g, b)) each. Those
        past the strip's 120th column fall off its end. `green_carried`: see as_sent()."""
        for c, (rows, rgb) in enumerate(columns[:COLS]):
            sent = as_sent(rgb, self.shift, green_carried)
            rows &= 0xFE                        # (bit 0 has no strip)
            self.now[c] = (rows, sent) if rows and sent != "000000" else (0, None)
        if None in self.now:
            raise ValueError("a frame is worked out before the strip was cleared")
        return Want(list(self.now), len(columns), whole)

    def clear(self):
        """clear()"""
        return self.frame([(0, (0, 0, 0))] * COLS, whole=True)


def char_columns(font, ch, rgb, space=True):
    """sendChar(): the character's columns (none for a vertical tab; a byte the font does not
    have is a space), then its blank column unless told not to."""
    return [(rows, rgb) for rows in font.columns(bytes([ch]))[:-1]] + ([(0, rgb)] if space else [])


def string_columns(font, text, rgb):
    """sendString(): the first character with its blank column, then at most as many more as
    there are characters on the strip (20), each with a blank column unless it is the text's
    last. An empty text is one blank character."""
    out = char_columns(font, text[0] if text else 0, rgb)
    for i in range(1, min(len(text), COLS // (font.cols + 1) + 1)):
        out += char_columns(font, text[i], rgb, space=i + 1 < len(text))
    return out


def text_cols(font, text):
    """textCols() of TextWindow.h: the width of a text, with no blank column after its last character."""
    return max(0, sum((0 if ch == VT else font.heart_cols if ch == A.HEART else font.cols) + 1 for ch in text) - 1)


def fade(env, count):
    """The white of the fading styles at their frame `count`: GAMMA(((count % 100) * 256) / 100).
    The count runs DOWN, so a fade begins at its brightest; its last 11 frames are dark (the
    gamma table is 0 up to 27)."""
    return env["gamma"][(count % 100) * 256 // 100]


def countdown_style(env, strip, text, count, show_count):
    """showAsCountdownStyle(): the clear, `count` frames with the count running down to 0, then
    the 100 frames of the last blink. With the count: the text, the count in red, and both again
    straight after (not on the other panel: where the first copy ends). Without it
    (showAsFlashyStyle): the text in the middle of each panel."""
    font = env["std"]
    width = text_cols(font, text)
    spare = 0 if width >= PANEL else PANEL - width      # textCentre()
    dark = (0, (0, 0, 0))

    def frame(n, digits, red):
        white = (fade(env, n),) * 3
        half = string_columns(font, text, white)
        if show_count:
            for i, ch in enumerate(digits):
                half += char_columns(font, ch, (red, 0, 0), space=i + 1 < len(digits))
        else:
            half = [dark] * (spare // 2) + half + [dark] * (spare - spare // 2)
        return strip.frame(half + half)

    def digits(n):
        # (as the sketch works them out: the hundreds can be any character from '0' on)
        return bytes([n // 100 + 48, ord("."), n % 100 // 10 + 48, n % 10 + 48])

    clear = Part("the clear", [strip.clear()])
    running = [frame(n, digits(n), COUNT_RED) for n in reversed(range(count))]
    blink = [frame(n, b"0.00", fade(env, n)) for n in reversed(range(BLINK_FRAMES))]
    if show_count:
        return [clear, Part("the count running down from %s to 0.00, the text fading under it"
                            % digits(count - 1).decode("ascii"), running),
                Part("the last blink, 0.00 fading with the text", blink)]
    return [clear, Part("the fade, from its brightest down, as often as it fits", running),
            Part("the last blink", blink)]


_randoms = [1]          # avr-libc's random(): its state after each call, from the state at power-on


def random_number(n):
    """The sign's n-th random number (0 is the first after power-on), as avr-libc works it out:
    x = 16807 * x mod (2^31 - 1), from 1, by Schrage's method in 32 bits. The sketch never calls
    randomSeed()."""
    while len(_randoms) <= n + 1:
        x = _randoms[-1]
        x = 16807 * (x % 127773) - 2836 * (x // 127773)
        _randoms.append(x + 0x7FFFFFFF if x < 0 else x)
    return _randoms[n + 1]


def starfield(strip, stars, before):
    """showstarfieldcustom(): for each star one random number below 960 (Arduino's random(960):
    the number modulo 960), its column the number / 8 and its row the number's last three bits;
    a frame is 121 columns, all dark but that one LED in starlight blue. One row in eight is not
    on the sign (the eighth row would be bit 8 of a byte): that frame is dark. `before`: how many
    random numbers the sign has drawn before this starfield."""
    dark, wants = (0, (0, 0, 0)), []
    for i in range(stars):
        r = random_number(before + i) % (COLS * 8)
        wants.append(strip.frame([dark] * (r // 8) + [((2 << (r & 7)) & 0xFF, STAR_RGB)] + [dark] * (COLS - r // 8),
                                 green_carried=True))
    return Part("the starfield", wants)


def model_brightness(env, message):
    """'U' 'B': the shift is stored, then the sign says what it is, in the flashy style and
    already at that brightness."""
    shift = message["_shift"]
    return countdown_style(env, Strip(shift), b"BRIGHT=%d" % shift, FLASHY_FRAMES, False)


def model_letters(env, message):
    """'O' with a text that fits and no dly: the clear, then one frame per letter, the letters so
    far in the middle of each panel, in the message's colour through Gamma.h."""
    strip, font, text, g = Strip(message.get("_shift", 0)), env["std"], A.text_bytes(message), env["gamma"]
    rgb = (g[message["r"]], g[message["g"]], g[message["b"]])
    # (with a brightness shift the bytes as sent are worked out for white only: see as_sent())
    assert strip.shift == 0 or rgb == (255, 255, 255), "model_letters() with a shift is for a white text"
    width = len(font.columns(text)) - 1
    assert 0 < width < PANEL, "model_letters() is for a text that fits"
    clear = Part("the clear", [strip.clear()])
    letters = []
    for shown in range(1, len(text) + 1):
        one = A.side(font.columns(text, shown), 0, (PANEL - width) // 2)
        letters.append(strip.frame([(rows, rgb) for rows in one + one]))
    return [clear, Part("the letters, one more in every frame", letters)]


def model_flashy(env, message):
    """'F'"""
    strip = Strip()
    return (countdown_style(env, strip, A.text_bytes(message), FLASHY_FRAMES, False)
            + [starfield(strip, STARS_AFTER, message["_stars_before"])])


def model_countdown(env, message):
    """'W'"""
    strip = Strip()
    return (countdown_style(env, strip, A.text_bytes(message), COUNT_FRAMES, True)
            + [starfield(strip, STARS_AFTER, message["_stars_before"])])


def model_unknown_icon(env, message):
    """'I' with an icon the sign does not have: its name behind "INVALID ICON: ", counted down
    from 0.99, and nothing after it. (The strip has room for 20 characters: where the name leaves
    none for the count, the count is not seen, and the frames are the text fading and no more.)"""
    text = b"INVALID ICON: " + A.text_bytes(message)[:ICON_NAME_BYTES]
    return countdown_style(env, Strip(), text, ICON_COUNT_FRAMES, True)


def model_keyboard(env, message):
    """'K', showAsInputStyle(): the clear, then twice the line's last ten characters from column
    0, each with its blank column, and one blank column more. White, but the last character (in
    mode W every character) red with a rest of green and blue in the first frame, red in the
    second. Nothing is sent to the columns after the line."""
    strip, font = Strip(), env["std"]
    shown = A.text_bytes(message)[-KEYBOARD_CHARS:]
    white = (env["gamma"][255],) * 3
    clear = Part("the clear", [strip.clear()])
    frames = []
    for count in KEYBOARD_COUNTS:
        rest = (count % 100) * 256 // 100
        columns = []
        for i, ch in enumerate(shown):
            blinks = message["mode"] == "W" or i == len(shown) - 1
            columns += char_columns(font, ch, (0xFF, rest, rest) if blinks else white)
        frames.append(strip.frame(columns + [(0, (0, 0, 0xFF))]))
    return [clear, Part("the line's last %d characters, the last one (mode W: all) red: with a rest of "
                        "green and blue, then without" % KEYBOARD_CHARS, frames)]


def model_stars(env, message):
    """'S': as many frames of starfield as it asks for, none for a number below 0."""
    stars = max(message["stars"], 0)
    if not stars:
        return []
    strip = Strip()
    strip.clear()       # (a starfield's frames reach the whole strip themselves: what was there is gone)
    return [starfield(strip, stars, message["_stars_before"])]


# ---- the checkers -----------------------------------------------------------------------------------
def by_parts(model, label, within=None, then=False):
    """A checker: the message ends (in `within(message)` seconds, if given), and its frames are
    the parts model(env, message) works out. `then`: its lines go under the message before it."""
    def checker(env, message, result):
        start, end, frames = result
        log = [("  then " if then else "") + label(message)]
        if asked_again(log, start, end, within(message) if within else None):
            check_parts(log, frames, model(env, message))
        return log
    checker.model = model
    return checker


def keyboard_label(message):
    text = A.text_bytes(message)
    return "K, a line of %d characters%s%s" % (
        len(text), "" if len(text) > 12 else " " + json.dumps(message["str"]),
        ", mode W (every character blinks)" if message["mode"] == "W" else "")


def probe(message, checker):
    """A message after the one under test: it is drawn as it always was, by ads_test.py's own
    checks of its style."""
    def f(env, message_, result):
        start, end, frames = result
        log = ["  then %s" % A.label(message_)]
        if asked_again(log, start, end):
            log += checker(env, message_, start, end, frames)[1:]
        return log
    return (message, f)


def check_chonky_whole(env, message, start, end, frames):
    """ads_test.py's check of a chonky text, and the number of its frames. (ads_test.py counts
    them for a dly of 1000 and of 0 only; its rule, for any dly of a text that fits: a frame is
    begun every AD_WAVE_FRAME_MS for as long as the text stays, the first one 4.4 to 4.9 ms
    before dly begins, and none when fewer than 6 ms of it are left.)"""
    log = A.check_chonky(env, message, start, end, frames)
    wanted = (message["dly"] - 2) // env["wave"] + 1
    check(log, len(frames) == wanted, "dly %d is %d frames, one every %d ms (%d)"
          % (message["dly"], wanted, env["wave"], len(frames)))
    return log


# ---- the lists ---------------------------------------------------------------------------------------
# (text, the shift it must leave stored). An empty text keeps the shift; it comes after "3".
SHIFTS = [("0", 0), ("1", 1), ("2", 2), ("3", 3), ("", 3), ("4", 4), ("5", 5), ("6", 6), ("7", 7), ("8", 8),
          ("9", 8), ("200", 8), ("255", 8), ("256", 8), ("264", 8), ("32767", 8), ("32768", 8), ("32776", 8),
          ("65535", 8), ("65536", 8), ("65544", 8), ("4294967304", 8), ("99999999999999999999", 8),
          ("-1", 0), ("7", 7), ("-32769", 0), ("7", 7), ("x", 0), ("8x", 8), (" 5", 5), ("+6", 6), ("0", 0)]


def line_of(n):
    """A keyboard line of n characters as the app sends it: what was typed, then the cursor."""
    typed = "".join("ABCDEFGHJKLMNPQRSTUVWXYZ23456789#\u007f"[i % 34] for i in range(n - 1))
    return typed + "_"


def groups(env):
    """The lists: (name, [(message, checker(env, message, result) giving log lines)], the limit
    of simulated seconds). Keys of a message that begin with "_" are this test's and are not sent."""
    order = [0]

    def probe_o():
        order[0] += 1
        return {"type": "O", "str": "TEN CHARS!", "r": 255, "g": 212, "b": 59, "dly": 100, "_order": order[0]}

    def probe_c():
        order[0] += 1
        return {"type": "C", "str": "CHONK", "r": 29, "g": 161, "b": 242, "dly": 100, "_order": order[0]}

    def pair():
        return [probe(probe_o(), A.check_one_by_one), probe(probe_c(), check_chonky_whole)]

    # 'U' 'B', and a white text after each: what the shift does to a colour
    set_shift = by_parts(model_brightness, lambda m: "U B %s: the brightness shift is %d after it, and the sign "
                         "says so at that brightness" % (json.dumps(m["str"]), m["_shift"]))
    white_text = by_parts(model_letters, lambda m: "a white text, O \"HI\": white shifted by %d, no more and no "
                          "less (%d of 255 a colour; dark at %d)" % (m["_shift"], 255 >> m["_shift"], SHIFT_MAX),
                          then=True)
    brightness = []
    for text, shift in SHIFTS:
        brightness.append(({"type": "U", "subtype": "B", "str": text, "_shift": shift}, set_shift))
        brightness.append(({"type": "O", "str": "HI", "r": 255, "g": 255, "b": 255, "dly": 0, "_shift": shift},
                           white_text))

    # 'K'. The line takes its time to arrive and to be read (measured: 0.8 ms a character); drawing
    # it does not depend on its length.
    key_line = by_parts(model_keyboard, keyboard_label, within=lambda m: 0.15 + len(A.text_bytes(m)) * 0.001)

    def k(text, mode="N"):
        return ({"type": "K", "str": text, "mode": mode}, key_line)

    keyboard = ([k(line_of(n)) for n in (1, 2, 9, 10)]
                + [k("")]                           # after a line that left the sign lit
                + [k(line_of(n)) for n in (11, 12, 15, 19, 20, 21, 64, 255)]
                + [k(line_of(10), "W"), k(line_of(20), "W")]
                # lines whose right picture has dark characters in it: a space before the cursor, a
                # space alone, spaces among the last ten of a longer line, in mode W, a byte the
                # font does not have (two: an e with an accent), and a heart (seven columns)
                + [k(" _"), k(" "), k("A  B _"), k("SPACES    IN   IT _"), k(" _", "W"), k("é_"),
                   k("I \u007f U_")]
                + pair())
    keyboard_long = [k(line_of(256))] + pair() + [k(line_of(300)), k(line_of(520))] + pair()

    flashy = by_parts(model_flashy, A.label)
    countdown = by_parts(model_countdown, A.label)
    texts = [
        # (the text first: what follows an empty text in the sign's memory is then the rest of the
        # message, and a sender that runs past the end of the text draws it)
        probe(probe_o(), A.check_one_by_one),       # the sign is lit before the empty text
        ({"str": "", "type": "F"}, flashy),
        ({"str": "", "type": "W"}, countdown),
        ({"type": "W", "str": "NEW"}, countdown),
        ({"type": "F", "str": "NEW MSG"}, flashy),
        # wider than the strip: the count is past its end, and what is seen is the text fading
        ({"type": "W", "str": "A TEXT THAT LEAVES NO ROOM"}, countdown),
    ] + pair()

    stars_of = by_parts(model_stars, lambda m: "S with %d stars" % m["stars"],
                        within=lambda m: 0.05 + max(m["stars"], 0) * 0.02)
    stars = [({"type": "S", "str": "", "stars": n}, stars_of) for n in (64, 0, -1, -32768, 40, 5, 1)] + pair()

    unknown = by_parts(model_unknown_icon, A.label)

    def invalid(text):
        return ({"type": "I", "str": text, "r": 255, "g": 255, "b": 255}, unknown)

    # (the sign has the icons 1 2 X A B D E and looks at the first character only)
    icons = ([invalid("Q")] + pair() + [invalid("Q" * 62)] + pair() + [invalid("Q" * 70)] + pair()
             + [invalid("FGHIJKLMNOPQRSTUVWXYZ01234" * 8)] + pair()
             # no name at all; then where the count leaves the strip: a name of two characters is
             # the longest with the whole count on it, with three its last digit is cut, with six
             # one column of it is left, with seven none; and a name as long as a message carries
             + [invalid(""), invalid("ZZ"), invalid("ZZZ"), invalid("SIXSIX"), invalid("SEVENS7"),
                invalid("Z" * 520)] + pair())
    # many short ones: what one of them left behind must not add up
    icons_many = ([invalid("\U0001F525")] + pair()     # an emoji the app has no icon for: four bytes
                  + [invalid(chr(ord("F") + i % 18)) for i in range(70)] + pair())

    lists = [("brightness", brightness, 240), ("keyboard", keyboard, 60), ("keyboard_long", keyboard_long, 60),
             ("empty_text", texts, 120), ("stars", stars, 60), ("unknown_icon", icons, 150),
             ("unknown_icon_many", icons_many, 400)]
    # Each list is a run of its own from power-on: how many random numbers the sign has drawn
    # before each message's starfield. (No other message of these lists draws one.)
    for _, cases, _ in lists:
        drawn = STARS_AT_START
        for message, _ in cases:
            message["_stars_before"] = drawn
            drawn += {"F": STARS_AFTER, "W": STARS_AFTER, "S": max(message.get("stars", 0), 0)}.get(message["type"], 0)
    return lists


def environment(root, emu=None, image=None, work=None):
    """What the checks need of the sources the firmware was built from."""
    fonts = open(os.path.join(root, "Fonts.h"), encoding="utf-8", errors="replace").read()
    sketch = open(os.path.join(root, "LED_Marquee.ino"), encoding="utf-8", errors="replace").read()
    gamma = A.table(open(os.path.join(root, "Gamma.h"), encoding="utf-8").read(), "gamma[]")
    std, chonk = A.table(fonts, "FontStd5x7[]"), A.table(fonts, "FontChonk[]")
    if len(std) != 95 * 5 + 7 or len(chonk) != 96 * 8 or len(gamma) != 256:
        sys.exit("Fonts.h or Gamma.h is not what this test expects (%d, %d, %d values)"
                 % (len(std), len(chonk), len(gamma)))
    return {
        "emu": emu, "hex": image, "work": work,
        "std": A.Font(std, 5, 7), "chonk": A.Font(chonk, 8, 8), "gamma": gamma,
        "letter": A.setting(sketch, "CHARS_ONEBYONE_DELAY"), "step": A.setting(sketch, "AD_SCROLL_STEP_MS"),
        "hold": A.setting(sketch, "AD_SCROLL_HOLD_MS"), "wave": A.setting(sketch, "AD_WAVE_FRAME_MS"),
        "quiet": A.setting(sketch, "REQUEST_QUIET_MS", 0),     # (see ads_test.py: the wait before a request)
    }


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    env = environment(sys.argv[3], sys.argv[1], sys.argv[2], sys.argv[4])
    port = int(sys.argv[5]) if len(sys.argv) > 5 else 14460
    os.makedirs(env["work"], exist_ok=True)
    lists = groups(env)
    results = [None] * len(lists)

    def work(i):
        name, cases, seconds = lists[i]
        try:
            logs = []
            for (message, checker), result in zip(cases, run(env, name, [m for m, _ in cases], port + i, seconds)):
                logs += checker(env, message, result)
            results[i] = logs
        except Exception as e:  # a run that could not be made or read is a failed test, not a crash
            A.failures.append("%s: %s" % (name, e))
            results[i] = ["  FAIL  %s: %r" % (name, e)]

    threads = [threading.Thread(target=work, args=(i,)) for i in range(len(lists))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for (name, _, _), logs in zip(lists, results):
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
