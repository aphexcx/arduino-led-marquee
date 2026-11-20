#!/usr/bin/env python3
"""A test of the checks of fixes_test.py and leftover_test.py themselves. No emulator, no firmware
image: frames in memory.

fixes_test.py says that a message is its parts in their order, each as long as worked out and
each frame the picture worked out. This holds it to that:

  THE FINDINGS of the reviews of these tests, by name: the frames a review described (a part
  missing, cut to one frame or dark; bits behind a frame's last column; a text before a case that
  left the strip's last columns dark; a marquee that is one dark frame) do not pass, and a right
  picture a check had refused (a keyboard line whose characters light nothing) does.

  THE MUTATION TABLE, for every message of fixes_test.py's lists: its right frames are made
  (below), then one part of it is changed, and the message's check must FAIL. For every part:
     missing          the part is not there
     one frame        only its first frame is
     half             only its first half is
     dark             every frame of it is dark
     moved            it comes after the message's other parts (the last part: before them)
     flash            its first frame is right and the rest of it is dark
     stuck            its first frame, as often as the part is long
     one short        its last frame is not there
     one more         its last frame comes twice
     backwards        its frames in the opposite order
     a bit over       its last frame has one bit more behind it than its columns have
     a column short   its first frame stops one column early (its picture is not touched)
     a column right   every frame's picture is one column further right on the strip
     half as bright   every colour byte of its frames is halved

  THE REVERSE TABLE. Right pictures must PASS:
     - the right frames of every message
     - a change of the table above that leaves every frame as it was (a dark part made dark, a
       part of one frame cut to one frame, a clear moved among dark frames): the sign shows the
       same, so the check has nothing to refuse
     - the edges of what the sign can be sent, which must be among the lists' messages: an empty
       text in each style, a space alone, a space before the cursor, a text wider than the strip
       (its count is not seen), the longest icon name, the brightness shifts 0 and 8

"The right frames" are made from fixes_test.py's own working out, written as a frames dump has
them and read back by ads_test.py's Frame like any dump. That this working out is what the
firmware draws is not shown here: fixes_test.py shows it, on the image. Shown here is that the
checks let nothing else through. Two lists of frames are "the same" when every frame has the
same LEDs lit in the same colours and is as many bits long: all a check looks at.

usage: fixes_selftest.py <repo root> [--cells]        (--cells: one line for every cell)
Python 3 standard library only.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ads_test as A  # noqa: E402
import fixes_test as T  # noqa: E402
import leftover_test as L  # noqa: E402

COLS, PANEL = T.COLS, T.PANEL


# ---- frames in memory -------------------------------------------------------------------------------
class Frames:
    """Makes frames as a dump has them, and knows which of them are the same."""
    def __init__(self):
        self.cache, self.sights, self.sight_of, self.kept, self.made_from = {}, {}, {}, [], {}

    def keep(self, d):
        f = A.Frame(d, self.cache)
        self.made_from[id(f)] = d
        sight = (tuple(f.mask), tuple(f.colours), f.bits)
        self.sight_of[id(f)] = self.sights.setdefault(sight, len(self.sights))
        self.kept.append(f)             # (so that no other frame gets its id)
        return f

    def frame(self, cols, sent, t, over=0):
        """cols: [(rows, 'rrggbb' or None)] for the strip's 120 columns; `sent`: the columns the
        frame sends; `over`: bits sent after its last whole column."""
        strips = [[px if rows & (0x80 >> r) else "000000" for r in range(A.ROWS)] for rows, px in cols]
        return self.keep({"t": t, "bits": sent * 24 + over, "marginal": 0, "partial_bits": 0, "gap_us": 20.0,
                          "rows": ["".join(row) for row in zip(*strips)]})

    def changed(self, f, bits=0, rows=None):
        """The frame f with `bits` more bits behind it (no LED differs), or with each of its rows
        (720 hex digits) put through `rows`."""
        d = dict(self.made_from[id(f)])
        d["bits"] += bits
        if rows:
            d["rows"] = [rows(row) for row in d["rows"]]
        return self.keep(d)

    def dark(self, f):
        return self.changed(f, rows=lambda row: "0" * len(row))

    def right(self, f):
        return self.changed(f, rows=lambda row: "000000" + row[:-6])

    def dim(self, f):
        return self.changed(f, rows=lambda row: "".join("%02x" % (int(row[i:i + 2], 16) >> 1)
                                                          for i in range(0, len(row), 2)))

    def sight(self, frames):
        return tuple(self.sight_of[id(f)] for f in frames)


def right_frames(env, made, message, checker):
    """The right frames of a message: (start, end, frames, [(part, first frame, frames)])."""
    start = 10.0
    if hasattr(checker, "model"):
        t, frames, parts = start, [], []
        for p in checker.model(env, message):
            parts.append((p.what, len(frames), len(p.wants)))
            for w in p.wants:
                t += 0.004
                frames.append(made.frame(w.cols, w.sent, t))
        return start, (t if frames else start + 0.02) + 0.002, frames, parts
    # The messages after a case, checked by ads_test.py's own checks: those want the frames at
    # their pace too, and the next request dly after the complete text (and the wait for a quiet
    # line that the firmware may make before every request: REQUEST_QUIET_MS, see ads_test.py).
    if message["type"] == "O":
        clear, letters = T.model_letters(env, message)
        frames = [made.frame(clear.wants[0].cols, COLS, start + 0.005)]
        for i, w in enumerate(letters.wants):
            frames.append(made.frame(w.cols, COLS, start + 0.010 + i * env["letter"] / 1000.0))
        return start, frames[-1].t + (message["dly"] + env["request_quiet"]) / 1000.0, frames, [
            ("the clear", 0, 1), ("the letters, one more in every frame", 1, len(letters.wants))]
    if message["type"] == "C":
        # showChonkySlideStyleOnBothPanels() for a text that fits, as ads_test.py describes it: the
        # text from column 0 of each panel; red is the low byte of dly, less 10 per frame, plus 10
        # per column; a frame every AD_WAVE_FRAME_MS; every colour byte carried
        text = A.side(env["chonk"].columns(A.text_bytes(message)), 0)
        dly, frames = message["dly"], []
        for k in range((dly - 2) // env["wave"] + 1):
            cols = [(rows, "%02x%02x%02x" % tuple(A.carried(v) for v in (
                        (dly - 10 * k + 10 * c) & 0xFF, message["g"], message["b"]))) if rows else (0, None)
                    for c, rows in enumerate(text)]
            frames.append(made.frame(cols + cols, COLS, start + 0.005 + k * env["wave"] / 1000.0))
        return start, frames[0].t + (dly + env["request_quiet"]) / 1000.0, frames, [("the text with its wave", 0, len(frames))]
    raise ValueError("no right frames for %r" % message)


def passes(run):
    """True when the check `run` calls has nothing against its frames. A check that cannot read
    them (an exception) has not passed them."""
    before = len(A.failures)
    try:
        run()
        ok = len(A.failures) == before
    except Exception:  # noqa: BLE001
        passes.crashed += 1
        ok = False
    del A.failures[before:]
    return ok


passes.crashed = 0


# ---- the findings of the reviews, one by one ------------------------------------------------------------
def findings(env, made, cell, root):
    lists = {}
    for _, list_, _ in T.groups(env):
        for m, c in list_:
            lists.setdefault((m["type"], m.get("str"), m["mode"] if m["type"] == "K" else m.get("_shift")), (m, c))

    def held(key, name, change=None):
        message, checker = lists[key]
        start, end, frames, parts = right_frames(env, made, message, checker)
        parts = {what: (s, n) for what, s, n in parts}
        cell("finding", passes(lambda: checker(env, dict(message), (start, end, frames))),
             name + ": the right frames pass")
        if change:
            changed = change(frames, parts)
            cell("finding", not passes(lambda: checker(env, dict(message), (start, end, changed))),
                 name + ": does not pass")

    blink, fade = "the last blink, 0.00 fading with the text", "the fade, from its brightest down, as often as it fits"
    count = "the count running down from 0.99 to 0.00, the text fading under it"

    def without(what):
        def change(frames, parts):
            s, n = parts[what]
            return frames[:s] + frames[s + n:]
        return change

    def one_flash(frames, parts):
        s, _ = parts[fade]
        return frames[:s + 1] + [made.dark(f) for f in frames[s + 1:]]

    def last_fifth_dark(frames, parts):
        s, n = parts["the starfield"]
        return frames[:s + n - n // 5] + [made.dark(f) for f in frames[s + n - n // 5:]]

    held(("W", "", None), 'W "" that goes from 0.00 straight to the starfield, its last blink missing', without(blink))
    held(("I", "Z" * 520, None), "the longest unknown icon name as a clear and one frame of its text",
         lambda frames, parts: frames[:2])
    held(("I", "Q" * 62, None), "an unknown icon name of 62 characters with its last blink missing", without(blink))
    held(("I", "Q" * 62, None), "an unknown icon name of 62 characters with its count missing", without(count))
    held(("U", "0", 0), "BRIGHT=0 as a clear, one frame of the text and 299 dark frames", one_flash)
    held(("K", " _", "N"), 'the keyboard line " _" (a space lights nothing)')
    held(("K", " ", "N"), 'the keyboard line " " (nothing is lit in it at all)')
    held(("F", "", None), 'F "" with the last fifth of its starfield dark', last_fifth_dark)
    held(("F", "", None), 'F "" with every star one column further right',
         lambda frames, parts: frames[:parts["the starfield"][0]] + [made.right(f) for f in
                                                                     frames[parts["the starfield"][0]:]])
    held(("W", "NEW", None), 'W "NEW" with the red of its count in the other spelling (80 for c0: a third less light)',
         lambda frames, parts: frames[:1] + [made.changed(f, rows=lambda row: row.replace("c00000", "800000"))
                                             for f in frames[1:]])

    # leftover_test.py: the text before each case, an icon frame's length, the marquees
    icons_h = open(os.path.join(root, "Icons.h"), encoding="utf-8", errors="replace").read()
    lenv = dict(env, icons={"1": L.icon_table(icons_h, "ENEMY1"), "A": L.icon_table(icons_h, "ANJUNA")})

    def ad(text, dark_from=COLS):
        """The last frame of the text before a case: centred, or its end when it is wider than a side."""
        cols = env["std"].columns(text.encode("ascii"))[:-1]
        one = A.side(cols, max(0, len(cols) - PANEL), (PANEL - len(cols)) // 2 if len(cols) < PANEL else 0)
        both = [(rows, "ffffff") if rows and c < dark_from else (0, None) for c, rows in enumerate(one + one)]
        return 1.0, 2.0, [made.frame(both, COLS, 1.9)]

    def icon(name, over=0, at=50):
        """The icons' frames: the clear, then the animation; frame `at` of it with `over` bits more."""
        frames = [made.frame([(0, None)] * COLS, COLS, 2.004)]
        for i, cols in enumerate(L.icon_frames(lenv["icons"][name])):
            shown = [(rows, "0000ff") if rows else (0, None) for rows in (cols + [0] * COLS)[:COLS]]
            frames.append(made.frame(shown, len(cols), 2.1 + i * 0.05, over if i == at else 0))
        return 2.0, 8.0, frames

    def leftover(name, run, must_pass):
        cell("finding", passes(run) == must_pass, "leftover: %s: %s" % (name, "passes" if must_pass else "does not pass"))

    def icons(name, ad_result, result, must_pass, icon_name="1"):
        message = {"type": "I", "str": icon_name, "r": 0, "g": 0, "b": 255}
        leftover(name, lambda: L.check_icon(lenv, message, ad_result, result), must_pass)

    right_ad = ad(L.AD["str"])
    icons("the text before a case and the icons after it, as they must be", right_ad, icon("1"), True)
    icons("the same with the icon whose frames are longer than the strip", right_ad, icon("A"), True, "A")
    icons('the text before a case is "TEN CHARS!", which leaves the strip\'s last three columns dark',
          ad("TEN CHARS!"), icon("1"), False)
    icons("the text before a case with the strip's last column dark", ad(L.AD["str"], COLS - 1), icon("1"), False)
    for over in (1, 12, 23):
        icons("an icon frame with %d bits behind its last column" % over, right_ad, icon("1", over), False)
    icons("an icon frame one column longer", right_ad, icon("1", 24), False)
    icons("the first icon frame with a bit behind it", right_ad, icon("A", 1, 0), False, "A")

    # the marquees: D and N as they are, T after its clear
    marquees = [({"type": "T", "str": "TRACK"}, L.check_track), ({"type": "D", "str": "HELLO"}, L.check_marquee_as_it_is),
                ({"type": "N", "str": "CHONKY"}, L.check_marquee_as_it_is)]
    L.marquee_state(marquees)
    for message, checker in marquees:
        if message["type"] == "T":
            strip = T.Strip()
            parts = [T.Part("the clear", [strip.clear()])] + L.marquee_parts(lenv, message, strip)
        else:
            parts = L.marquee_parts(lenv, message, L.after_the_ad(lenv)[0])
        frames = [made.frame(w.cols, w.sent, 2.0 + 0.02 * i) for i, w in enumerate(w for p in parts for w in p.wants)]
        n = len(parts[-2].wants) if message["type"] != "T" else len(frames)       # the frames before the stars
        what = "%s %s" % (message["type"], json.dumps(message["str"]))

        def run(frames_, message=message, checker=checker):
            return lambda: checker(lenv, dict(message), right_ad, (2.0, 2.0 + 0.02 * len(frames) + 0.01, frames_))
        leftover(what + " as it must be", run(frames), True)
        leftover(what + " as its first frame and nothing after it", run(frames[:1]), False)
        leftover(what + " as its first frame, dark, and nothing after it", run([made.dark(frames[0])]), False)
        leftover(what + " in a colour half as bright", run([made.dim(f) for f in frames]), False)
        leftover(what + " with one frame of its marquee dark", run(frames[:n // 2] + [made.dark(frames[n // 2])]
                                                                  + frames[n // 2 + 1:]), False)
        if n < len(frames):
            leftover(what + " without its stars", run(frames[:n]), False)
            leftover(what + " with its stars dark", run(frames[:n] + [made.dark(f) for f in frames[n:]]), False)


# ---- the tables -------------------------------------------------------------------------------------
def changes(made):
    def moved(f, s, n):
        rest = f[:s] + f[s + n:]
        return rest + f[s:s + n] if s + n < len(f) else f[s:s + n] + rest

    return [
        ("missing", lambda f, s, n: f[:s] + f[s + n:]),
        ("one frame", lambda f, s, n: f[:s + 1] + f[s + n:]),
        ("half", lambda f, s, n: f[:s + n // 2] + f[s + n:]),
        ("dark", lambda f, s, n: f[:s] + [made.dark(x) for x in f[s:s + n]] + f[s + n:]),
        ("moved", moved),
        ("flash", lambda f, s, n: f[:s + 1] + [made.dark(x) for x in f[s + 1:s + n]] + f[s + n:]),
        ("stuck", lambda f, s, n: f[:s] + [f[s]] * n + f[s + n:]),
        ("one short", lambda f, s, n: f[:s + n - 1] + f[s + n:]),
        ("one more", lambda f, s, n: f[:s + n] + f[s + n - 1:s + n] + f[s + n:]),
        ("backwards", lambda f, s, n: f[:s] + f[s:s + n][::-1] + f[s + n:]),
        ("a bit over", lambda f, s, n: f[:s + n - 1] + [made.changed(f[s + n - 1], 1)] + f[s + n:]),
        ("a column short", lambda f, s, n: f[:s] + [made.changed(f[s], -24)] + f[s + 1:]),
        ("a column right", lambda f, s, n: f[:s] + [made.right(x) for x in f[s:s + n]] + f[s + n:]),
        ("half as bright", lambda f, s, n: f[:s] + [made.dim(x) for x in f[s:s + n]] + f[s + n:]),
    ]


def cases(env, made):
    """Every message of fixes_test.py's lists with its right frames; of those whose right frames
    are the same under the same check, the first. Gives (group, label, message, checker, start,
    end, frames, parts) and counts the messages in cases.messages."""
    seen = set()
    cases.messages = 0
    for group, list_, _ in T.groups(env):
        for message, checker in list_:
            cases.messages += 1
            start, end, frames, parts = right_frames(env, made, message, checker)
            key = (getattr(checker, "model", message["type"]), made.sight(frames))
            if key in seen:
                continue
            seen.add(key)
            text = json.dumps(message.get("str", ""))
            label = "%s %s%s%s" % (message["type"] + message.get("subtype", ""),
                                   text if len(text) < 30 else "%s... (%d characters)" % (text[:20], len(text) - 2),
                                   " stars %d" % message["stars"] if "stars" in message else "",
                                   " shift %d" % message["_shift"] if "_shift" in message else "")
            yield group, label, message, checker, start, end, frames, parts


def cells(env, made):
    """The mutation table: (group, label, part, change, message, checker, start, end, frames,
    same) for every part of every case and every change. `same`: the changed frames are the
    right ones still. A change that gives the frames another change of the same case gave
    already is left out."""
    for group, label, message, checker, start, end, frames, parts in cases(env, made):
        right = made.sight(frames)
        seen = set()
        for what, s, n in parts:
            for change, f in changes(made):
                changed = f(frames, s, n)
                sight = made.sight(changed)
                if sight != right and sight in seen:
                    continue
                seen.add(sight)
                yield group, label, what, change, message, checker, start, end, changed, sight == right


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    env, made, show = T.environment(sys.argv[1]), Frames(), "--cells" in sys.argv[2:]
    wrong = []

    def cell(table, ok, text):
        cell.count += 1
        if show or not ok or table == "finding":
            print("  %s  %s: %s" % ("ok  " if ok else "FAIL", table, text))
        if not ok:
            wrong.append("%s: %s" % (table, text))

    cell.count = 0
    print("== the findings of the reviews of these tests, one by one")
    findings(env, made, cell, sys.argv[1])
    named = cell.count

    print("== the reverse table: right pictures must pass")
    all_cases = list(cases(env, made))
    for group, label, message, checker, start, end, frames, parts in all_cases:
        cell("right", passes(lambda: checker(env, dict(message), (start, end, frames))), "%s: %s" % (group, label))
    # ... and its edges are among the messages
    listed = {(m["type"], m.get("str"), m.get("_shift")) for _, list_, _ in T.groups(env) for m, _ in list_}
    longest = max(len(m[1] or "") for m in listed if m[0] == "I")
    edges = [("an empty flashy text", ("F", "", None)), ("an empty countdown text", ("W", "", None)),
             ("an empty keyboard line", ("K", "", None)), ("an icon with no name", ("I", "", None)),
             ("a space alone", ("K", " ", None)), ("a space before the cursor", ("K", " _", None)),
             ("the brightness shift 0", ("U", "0", 0)), ("the brightness shift 8", ("U", "8", 8)),
             ("a white text at shift 0", ("O", "HI", 0)), ("a white text at shift 8", ("O", "HI", 8))]
    for name, key in edges:
        cell("edge", key in listed, "%s is among the messages" % name)
    cell("edge", longest >= 500, "the longest icon name is as long as a message carries (%d characters)" % longest)
    wide = [m for m in listed if m[0] == "W" and len(T.string_columns(env["std"], m[1].encode(), (1, 1, 1))) >= COLS]
    cell("edge", bool(wide), "a countdown text wider than the strip, its count not seen, is among the messages")

    print("== the mutation table: a message with one part changed must fail its check")
    table, same_cells, by_change = 0, 0, {}
    for group, label, what, change, message, checker, start, end, frames, same in cells(env, made):
        ok = passes(lambda: checker(env, dict(message), (start, end, frames)))
        text = "%s: %s: %s: %s" % (group, label, what, change)
        if same:
            same_cells += 1
            cell("right", ok, text + " (the same frames)")
        else:
            table += 1
            by_change.setdefault(change, [0, 0])[0] += 1
            by_change[change][1] += 0 if ok else 1
            cell("changed", not ok, text + (" PASSES its check" if ok else ""))

    print("== counted")
    print("  %d findings of the reviews, by name" % named)
    print("  %d messages in fixes_test.py's lists, %d of them different (right frames or check)"
          % (cases.messages, len(all_cases)))
    print("  the reverse table: %d right pictures (%d messages, %d changes that leave the frames as they were), "
          "and %d edges" % (len(all_cases) + same_cells, len(all_cases), same_cells, len(edges) + 2))
    print("  the mutation table: %d cells" % table)
    for change, _ in changes(made):
        cells_, failed = by_change.get(change, [0, 0])
        print("    %-14s %5d cells, %5d fail their check" % (change, cells_, failed))
    if passes.crashed:
        print("  (%d times a check could not read the frames it was given: counted as not passed)" % passes.crashed)
        wrong.append("%d checks raised an exception" % passes.crashed)
    if wrong:
        print("FAILED: %d" % len(wrong))
        for line in wrong[:60]:
            print("  " + line)
        sys.exit(1)
    print("all cells as they must be")


if __name__ == "__main__":
    main()
