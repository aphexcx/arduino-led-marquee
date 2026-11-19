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
from that dump. What a picture must be is computed here from Fonts.h, not copied from a run.
Every list has a limit of simulated time, so a message that never ends is a failed check and not
a test that never ends.

usage: fixes_test.py <sign-emu> <firmware.hex> <repo root> <work dir> [port]
Python 3 standard library only. Uses seven TCP ports from `port` (default 14460).
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ads_test as A  # noqa: E402

COLS, PANEL = A.COLS, A.PANEL
check = A.check


# ---- running the emulator -------------------------------------------------------------------------
def run(env, name, messages, port, seconds):
    """Serves `messages` once, for at most `seconds` of simulated time. Returns for each message
    (request time, next request time, frames); the times are None from where the sign stopped
    asking. Bytes the sign sent that are not a request are an error, and so is an emulator that
    does not end with status 0 (3 is a firmware that crashed)."""
    dump = os.path.join(env["work"], name + ".jsonl")
    log = os.path.join(env["work"], name + ".log")
    with open(log, "w") as err:
        emu = subprocess.Popen(
            [env["emu"], "--headless", "--port", str(port), "--wait-client", "30",
             "--until-requests", str(len(messages) + 1), "--duration", str(seconds), "--frames", dump,
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
        if emu.poll() is None:
            emu.kill()
    if status != 0:
        raise RuntimeError("%s: sign-emu exited with status %s: %s"
                           % (name, status, open(log).read().strip()[-300:]))
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
    for i in range(len(messages)):
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


# ---- what the sketch's own senders draw ---------------------------------------------------------
def glyph(font, ch):
    return font.columns(bytes([ch]))[:-1]


def send_string(font, text):
    """sendString(): the first character and its blank column, then at most 20 more, each with a
    blank column unless it is the text's last. An empty text is one blank character."""
    out = glyph(font, text[0] if text else 0) + [0]
    for i in range(1, min(len(text), 21)):
        out += glyph(font, text[i])
        if i + 1 < len(text):
            out.append(0)
    return out


def strip(cols):
    """What the strip shows of the columns of a frame, when it was dark before."""
    return tuple((cols + [0] * COLS)[:COLS])


def count_frames(font, text, counts):
    """The frames of the countdown style with its count: text, count, text, count from column 0.
    A dictionary from the picture to the count it shows."""
    s = send_string(font, text)
    dark = [0] * len(s)          # the text fades: at the bottom of a fade only the count is lit
    out = {}
    for n in counts:
        d = font.columns(("%d.%02d" % (n // 100, n % 100)).encode("ascii"))[:-1]
        out[strip(s + d + s + d)] = n
        out[strip(dark + d + dark + d)] = n
    return out


def lit(frame):
    return sum(bin(m).count("1") for m in frame.mask)


def probe_checks(env, log, message, result, checker):
    """A message after the one under test: it is drawn as it always was."""
    start, end, frames = result
    if not asked_again(log, start, end):
        return
    for line in checker(env, message, start, end, frames)[1:]:
        log.append(line)


# ---- 'U' 'B' ---------------------------------------------------------------------------------------
# (text, the shift it must leave stored). An empty text keeps the shift; it comes after "3".
SHIFTS = [("0", 0), ("1", 1), ("2", 2), ("3", 3), ("", 3), ("4", 4), ("5", 5), ("6", 6), ("7", 7), ("8", 8),
          ("9", 8), ("200", 8), ("255", 8), ("256", 8), ("264", 8), ("32767", 8), ("32768", 8), ("32776", 8),
          ("65535", 8), ("65536", 8), ("65544", 8), ("4294967304", 8), ("99999999999999999999", 8),
          ("-1", 0), ("7", 7), ("-32769", 0), ("7", 7), ("x", 0), ("8x", 8), (" 5", 5), ("+6", 6), ("0", 0)]


def brightness_cases():
    out = []
    for text, shift in SHIFTS:
        out.append(({"type": "U", "subtype": "B", "str": text, "_shift": shift}, "set"))
        out.append(({"type": "O", "str": "HI", "r": 255, "g": 255, "b": 255, "dly": 0, "_shift": shift}, "probe"))
    return out


def check_brightness(env, cases, results):
    logs = []
    font, top = env["std"], env["gamma"][255]
    for (message, kind), (start, end, frames) in zip(cases, results):
        shift = message["_shift"]
        if kind == "set":
            log = ['U B %s: the brightness shift is %d after it' % (json.dumps(message["str"]), shift)]
            logs.append(log)
            if not asked_again(log, start, end):
                continue
            if shift < 8:
                cols = font.columns(("BRIGHT=%d" % shift).encode("ascii"))[:-1]
                expected = A.side(cols, 0, (PANEL - len(cols)) // 2)
                hits = [f for f in frames if f.mask[:PANEL] == expected and f.mask[PANEL:] == expected]
                check(log, bool(hits), 'the sign says "BRIGHT=%d" (%d frames)' % (shift, len(hits)))
            else:
                check(log, not any(lit(f) for f in frames), "the sign is dark while it says so (shift 8)")
            continue
        log = logs[-1]
        if not asked_again(log, start, end):
            continue
        last = frames[-1] if frames else None
        if shift >= 8:
            check(log, last is not None and not any(lit(f) for f in frames),
                  "a white text after it is dark: nothing is left of a value shifted by 8")
            continue
        cols = font.columns(b"HI")[:-1]
        expected = A.side(cols, 0, (PANEL - len(cols)) // 2)
        check(log, last is not None and last.mask[:PANEL] == expected and last.mask[PANEL:] == expected,
              'a white text after it is drawn ("HI")')
        if last is None:
            continue
        x = top >> shift
        seen = set()
        for c in range(COLS):
            seen |= last.colour(c)
        ok = bool(seen) and "mixed" not in seen and all(
            x <= int(px[i:i + 2], 16) <= (x | (x >> 1)) for px in seen for i in (0, 2, 4))
        check(log, ok, "its LEDs send %d of 255 a colour (%d to %d as sent): white shifted by %d, no more and "
              "no less (seen: %s)" % (x, x, x | (x >> 1), shift, ",".join(sorted(seen)) or "nothing"))
    return [line for log in logs for line in log]


# ---- 'K' -------------------------------------------------------------------------------------------
def line_of(n):
    """A keyboard line of n characters as the app sends it: what was typed, then the cursor."""
    typed = "".join("ABCDEFGHJKLMNPQRSTUVWXYZ23456789#\u007f"[i % 34] for i in range(n - 1))
    return typed + "_"


def check_keyboard(env, message, result):
    start, end, frames = result
    text = A.text_bytes(message)
    warning = message["mode"] == "W"
    log = ['K, a line of %d characters%s' % (len(text), ", mode W (every character blinks)" if warning else "")]
    # The line takes its time to arrive and to be read (measured: 0.8 ms a character); drawing it
    # does not depend on its length.
    if not asked_again(log, start, end, within=0.15 + len(text) * 0.001):
        return log
    if not text:
        check(log, len(frames) == 3 and not any(lit(f) for f in frames),
              "an empty line: the sign is cleared and stays dark (%d frames)" % len(frames))
        return log
    cols = env["std"].columns(text[-10:]) + [0]     # the characters, each with its blank column, and one more
    expected = strip(cols)
    shown = [f for f in frames if lit(f)]
    check(log, len(shown) == 2 and all(tuple(f.mask) == expected for f in shown),
          "its last ten characters from column 0 as Fonts.h has them, and nothing else lit on the sign "
          "(%d frames with light, %d of them exactly that)"
          % (len(shown), sum(1 for f in shown if tuple(f.mask) == expected)))
    # White, and the blinking colour of the two frames the style draws: red full, green and blue
    # 2 and then 0 of 255. (Green goes out as it is or carried, by what the code before it left.)
    last = len(env["std"].columns(text[-10:][:-1]))          # where the last character begins
    def colours(f, lo, hi):
        out = set()
        for c in range(lo, hi):
            out |= f.colour(c)
        return out
    white = set() if warning else set().union(*[colours(f, 0, last) for f in shown] or [set()])
    check(log, white <= {"ffffff"}, "the characters before the last are white (%s)" % (",".join(sorted(white)) or "none lit"))
    blink = [colours(f, 0 if warning else last, len(cols)) for f in shown]
    ok = len(blink) == 2 and all(b and "mixed" not in b for b in blink) and \
        all(px[:2] == "ff" and px[2:4] in ("02", "03") and px[4:] == "03" for px in blink[0]) and blink[1] <= {"ff0000"}
    check(log, ok, "%s red with a rest of green and blue, then red (%s, then %s)"
          % ("every character is" if warning else "the last character is",
             ",".join(sorted(blink[0])) if blink else "?", ",".join(sorted(blink[1])) if len(blink) > 1 else "?"))
    return log


# ---- 'F' and 'W' -----------------------------------------------------------------------------------
def check_count_style(env, message, result, text, counts, what):
    """Every frame with more than a star's light in it is: the text, the count, the text, the count."""
    start, end, frames = result
    log = [A.label(message)]
    if not asked_again(log, start, end):
        return log
    pictures = count_frames(env["std"], text, counts)
    shown = [f for f in frames if lit(f) >= 2]
    wrong = [f for f in shown if tuple(f.mask) not in pictures]
    seen = {pictures[tuple(f.mask)] for f in shown if tuple(f.mask) in pictures}
    check(log, bool(shown) and not wrong,
          "%s: every lit frame is that, then the count, twice, and nothing else (%d frames, %d are not)"
          % (what, len(shown), len(wrong)))
    # Every count is shown, down to 0.00. (Where the text leaves no room on the strip for the
    # count's last digits, several counts are one picture: every picture there is must be seen.)
    wanted = set(pictures.values())
    check(log, seen == wanted, "the count runs to its end: %d of its %d different pictures are seen"
          % (len(seen & wanted), len(wanted)))
    return log


def check_empty_flashy(env, message, result):
    start, end, frames = result
    log = [A.label(message)]
    if not asked_again(log, start, end):
        return log
    most = max([lit(f) for f in frames] or [0])
    check(log, bool(frames) and most <= 1, "an empty text lights nothing: no frame has more than the one LED of a "
          "star of the starfield after it (most: %d)" % most)
    return log


# ---- 'S' -------------------------------------------------------------------------------------------
def check_stars(env, message, result):
    start, end, frames = result
    stars = message["stars"]
    log = ["S with %d stars" % stars]
    wanted = max(stars, 0)
    if not asked_again(log, start, end, within=0.05 + wanted * 0.02):
        return log
    # (a star in the row the sign does not have lights nothing: about one in eight)
    check(log, len(frames) == wanted and all(lit(f) <= 1 for f in frames),
          "%d frames, at most one LED lit in each (%d frames, %d with a star)"
          % (wanted, len(frames), sum(1 for f in frames if lit(f) == 1)))
    return log


# ---- the lists ---------------------------------------------------------------------------------------
def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    root = sys.argv[3]
    fonts = open(os.path.join(root, "Fonts.h"), encoding="utf-8", errors="replace").read()
    sketch = open(os.path.join(root, "LED_Marquee.ino"), encoding="utf-8", errors="replace").read()
    gamma = A.table(open(os.path.join(root, "Gamma.h"), encoding="utf-8").read(), "gamma[]")
    std, chonk = A.table(fonts, "FontStd5x7[]"), A.table(fonts, "FontChonk[]")
    if len(std) != 95 * 5 + 7 or len(chonk) != 96 * 8 or len(gamma) != 256:
        sys.exit("Fonts.h or Gamma.h is not what this test expects (%d, %d, %d values)"
                 % (len(std), len(chonk), len(gamma)))
    env = {
        "emu": sys.argv[1], "hex": sys.argv[2], "work": sys.argv[4],
        "std": A.Font(std, 5, 7), "chonk": A.Font(chonk, 8, 8), "gamma": gamma,
        "letter": A.setting(sketch, "CHARS_ONEBYONE_DELAY"), "step": A.setting(sketch, "AD_SCROLL_STEP_MS"),
        "hold": A.setting(sketch, "AD_SCROLL_HOLD_MS"), "wave": A.setting(sketch, "AD_WAVE_FRAME_MS"),
    }
    port = int(sys.argv[5]) if len(sys.argv) > 5 else 14460
    os.makedirs(env["work"], exist_ok=True)

    order = [0]

    def probe_o():
        order[0] += 1
        return {"type": "O", "str": "TEN CHARS!", "r": 255, "g": 212, "b": 59, "dly": 100, "_order": order[0]}

    def probe_c():
        order[0] += 1
        return {"type": "C", "str": "CHONK", "r": 29, "g": 161, "b": 242, "dly": 100, "_order": order[0]}

    def k(n, mode="N"):
        return {"type": "K", "str": line_of(n) if n else "", "mode": mode}

    def icon(text):
        return {"type": "I", "str": text, "r": 255, "g": 255, "b": 255}

    def brightness(name, port):
        cases = brightness_cases()
        return check_brightness(env, cases, run(env, name, [m for m, _ in cases], port, 240))

    def cases_group(cases, seconds):
        """cases: (message, function of (env, message, result) giving log lines)"""
        def work(name, port):
            results = run(env, name, [m for m, _ in cases], port, seconds)
            logs = []
            for (message, checker), result in zip(cases, results):
                logs += checker(env, message, result)
            return logs
        return work

    def probe(message, checker):
        def f(env_, message_, result):
            log = ["  then %s" % A.label(message_)]
            probe_checks(env_, log, message_, result, checker)
            return log
        return (message, f)

    def invalid(text, counts=range(100)):
        shown = b"INVALID ICON: " + text.encode("utf-8")[:21]
        # (the strip has room for "INVALID ICON: " and six more characters; what the name's 21 are
        # for is the buffer, which the pictures cannot show: the messages after this one can)
        return (icon(text), lambda e, m, r: check_count_style(
            e, m, r, shown, counts, 'the sign says "INVALID ICON: " and the start of the icon\'s name'))

    def pair():
        return [probe(probe_o(), A.check_one_by_one), probe(probe_c(), A.check_chonky)]

    keyboard = ([(k(n), check_keyboard) for n in (0, 1, 2, 9, 10, 11, 12, 15, 19, 20, 21, 64, 255)]
                + [(k(10, "W"), check_keyboard), (k(20, "W"), check_keyboard)] + pair())
    keyboard_long = [(k(256), check_keyboard)] + pair() + [(k(300), check_keyboard), (k(520), check_keyboard)] + pair()
    empty = [
        # (the text first: what follows an empty text in the sign's memory is then the rest of the
        # message, and a sender that runs past the end of the text draws it)
        ({"str": "", "type": "F"}, check_empty_flashy),
        ({"str": "", "type": "W"}, lambda e, m, r: check_count_style(e, m, r, b"", range(600), "an empty text")),
        ({"type": "W", "str": "NEW"}, lambda e, m, r: check_count_style(e, m, r, b"NEW", range(600), '"NEW"')),
    ] + pair()
    stars = [({"type": "S", "str": "", "stars": n}, check_stars) for n in (5, 0, -1, -32768, 3)] + pair()
    # (the sign has the icons 1 2 X A B D E and looks at the first character only)
    icons = ([invalid("Q")] + pair() + [invalid("Q" * 62)] + pair() + [invalid("Q" * 70)] + pair()
             + [invalid("FGHIJKLMNOPQRSTUVWXYZ01234" * 8)] + pair())
    # many short ones: what one of them left behind must not add up
    icons_many = ([invalid("\U0001F525")] + pair()     # an emoji the app has no icon for: four bytes
                  + [invalid(chr(ord("F") + i % 18)) for i in range(70)] + pair())

    groups = [("brightness", brightness), ("keyboard", cases_group(keyboard, 60)),
              ("keyboard_long", cases_group(keyboard_long, 60)), ("empty_text", cases_group(empty, 90)),
              ("stars", cases_group(stars, 60)), ("unknown_icon", cases_group(icons, 120)),
              ("unknown_icon_many", cases_group(icons_many, 400))]
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
