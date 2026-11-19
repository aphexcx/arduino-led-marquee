#!/usr/bin/env python3
"""Nothing of the picture before a message stays lit beside it, on the real firmware image.

A track id ('T') and the icons ('I') send frames that are shorter than the strip: a track id of
nine characters or fewer never reaches the right-hand end of side B, and a row of icons stops 1
to 17 columns short of it. What a frame does not reach keeps what the strip showed before. So
both styles clear the sign first; this test holds them to it:

  - every case comes after a white text that fills both sides ("the ad"), and the ad's last
    frame is checked to be that text, LED for LED
  - the message's first frame is the whole strip, dark
  - then every frame of the message is worked out here, from Fonts.h or Icons.h, and compared
    column for column: for a track id, 60 blank columns and the text, one column further left in
    each frame, as many frames as that row has columns; for the icons, the 98 frames of the
    animation (14 rows of 7 steps) in the message's colour
  - in every one of those frames the columns the frame did not write are dark
  - so a track id leaves the sign dark when it has passed

The default and the chonky marquee ('D', 'N') are here as they are: their first frames are longer
than the strip, so they never left anything, and they must not start with a dark frame.

usage: leftover_test.py <sign-emu> <firmware.hex> <repo root> <work dir> [port]
Python 3 standard library only. Uses two TCP ports from `port` (default 14470).
"""
import json
import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ads_test as A  # noqa: E402
import fixes_test as F  # noqa: E402

COLS, PANEL = A.COLS, A.PANEL
check = A.check
lit = F.lit

AD = {"type": "O", "str": "TEN CHARS!", "r": 255, "g": 255, "b": 255, "dly": 100}


def written(frame):
    """How many columns of the strip the frame wrote."""
    return min(frame.bits // 24, COLS)


def beyond(frames):
    """(frames with something lit in the columns they did not write, the most such columns)."""
    counts = [sum(1 for c in range(written(f), COLS) if f.mask[c]) for f in frames]
    return sum(1 for n in counts if n), max(counts or [0])


def ad_checks(env, log, ad_result):
    start, end, frames = ad_result
    ok = F.asked_again(log, start, end)
    last = frames[-1] if ok and frames else None
    cols = env["std"].columns(A.text_bytes(AD))[:-1]         # 59 columns: it fills a side
    expected = A.side(cols, 0, (PANEL - len(cols)) // 2)
    check(log, last is not None and last.mask[:PANEL] == expected and last.mask[PANEL:] == expected and
          all(last.colour(c) <= {"ffffff"} for c in range(COLS)),
          "the text before it is on the sign: %s on both sides, white, lit to the last column but one of side B"
          % json.dumps(AD["str"]))


def cleared_first(log, frames, wanted=True):
    first = frames[0] if frames else None
    dark = first is not None and first.bits == COLS * 24 and not lit(first)
    if wanted:
        check(log, dark, "its first frame is the whole strip, dark")
    else:
        check(log, first is not None and not dark, "its first frame is its own (no dark frame before it)")
    return dark


# ---- 'T' -------------------------------------------------------------------------------------------
def track_row(font, text):
    """What a track id is: a row of 60 blank columns (ten spaces) and the text, each character with
    its blank column. An empty text has no row."""
    return ([0] * PANEL + font.columns(text)) if text else []


def check_track(env, message, ad_result, result):
    start, end, frames = result
    text = A.text_bytes(message)
    log = ["%s %s, after a white text" % (message["type"], json.dumps(message["str"]))]
    ad_checks(env, log, ad_result)
    if not F.asked_again(log, start, end):
        return log
    own = frames[1:] if cleared_first(log, frames) else frames
    bad, most = beyond(own)
    check(log, not bad, "in none of its %d frames is a column lit that the frame did not write (%d frames, up to %d "
          "columns)" % (len(own), bad, most))
    # Frame number i shows the row from its column i on, to the row's end (or the strip's), and
    # there are as many frames as the row has columns.
    row = track_row(env["std"], text)
    check(log, len(own) == len(row), "one frame for every column of ten spaces and the text (%d frames, %d columns)"
          % (len(own), len(row)))
    lengths = sum(1 for i, f in enumerate(own) if f.bits // 24 >= min(COLS, len(row) - i))
    right = sum(1 for i, f in enumerate(own)
                if tuple(f.mask[:written(f)]) == tuple((row[i:] + [0] * COLS)[:written(f)]))
    check(log, lengths == len(own) and right == len(own),
          "every frame is that row from its own column on, as Fonts.h has the text, and reaches the row's end or "
          "the strip's (%d of %d are that; %d reach the end)" % (right, len(own), lengths))
    if own:
        check(log, not lit(own[-1]), "when it has passed, the sign is dark")
    return log


def check_marquee_as_it_is(env, message, ad_result, result):
    start, end, frames = result
    log = ["%s %s, after a white text" % (message["type"], json.dumps(message["str"]))]
    ad_checks(env, log, ad_result)
    if not F.asked_again(log, start, end):
        return log
    cleared_first(log, frames, wanted=False)
    check(log, bool(frames) and frames[0].bits >= COLS * 24, "and that frame is as long as the strip or longer: "
          "nothing of the text before it is left (%d columns)" % (frames[0].bits // 24 if frames else 0))
    return log


# ---- 'I' -------------------------------------------------------------------------------------------
def icon_table(src, name):
    """(width, the columns of its two pictures) of an icon of Icons.h."""
    body = src[src.index("\n%s = {" % name):]
    values = A.numbers(body[body.index("{") + 1:body.index("};")])
    width, data = values[0], values[1:]
    if len(data) != 2 * width:
        sys.exit("Icons.h: %s is not two pictures of %d columns (%d values)" % (name, width, len(data)))
    return width, data


def icon_frames(icon):
    """showIconInvaders(): the rows come in from the bottom and leave at the top (shifts -7 to 6),
    in each row seven steps of the left margin (1 to 7 columns on the odd shifts, 7 down to 1 on
    the even ones); a frame is the margin, then as many icons as fit the strip at five columns
    apart, each followed by four blank columns. The picture alternates with the margin."""
    width, data = icon
    count = COLS // (width + 5)
    out = []
    for shift in range(-7, 7):
        for margin in (range(1, 8) if shift & 1 else range(7, 0, -1)):
            picture = data[(margin & 1) * width:(margin & 1) * width + width]
            if shift < 0:
                cols = [(c << -shift) & 0xFE for c in picture]     # (the sign has no row for bit 0)
            else:
                cols = [(c >> shift) & 0xFE for c in picture]
            out.append([0] * margin + (cols + [0] * 4) * count)
    return out


def check_icon(env, message, ad_result, result):
    start, end, frames = result
    log = ["I %s, after a white text" % json.dumps(message["str"])]
    ad_checks(env, log, ad_result)
    if not F.asked_again(log, start, end):
        return log
    own = frames[1:] if cleared_first(log, frames) else frames
    bad, most = beyond(own)
    check(log, not bad, "in none of its %d frames is a column lit that the frame did not write (%d frames, up to %d "
          "columns)" % (len(own), bad, most))
    wanted = icon_frames(env["icons"][message["str"]])
    right = sum(1 for f, cols in zip(own, wanted)
                if f.bits // 24 == len(cols) and tuple(f.mask[:len(cols)]) == tuple(cols[:COLS]))  # (A: up to 121)
    check(log, len(own) == len(wanted) == 98 and right == 98,
          "the animation, frame for frame as Icons.h has the icon: 14 rows of 7 steps, each frame the margin and "
          "the row of icons and no column more (%d frames, %d of them right)" % (len(own), right))
    colours = set()
    for f in own:
        for c in range(written(f)):
            colours |= f.colour(c)
    check(log, colours == {"0000ff"}, "every LED the animation lights is in the message's colour, blue (%s)"
          % (",".join(sorted(colours)) or "nothing lit"))
    return log


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    root = sys.argv[3]
    fonts = open(os.path.join(root, "Fonts.h"), encoding="utf-8", errors="replace").read()
    icons_h = open(os.path.join(root, "Icons.h"), encoding="utf-8", errors="replace").read()
    std = A.table(fonts, "FontStd5x7[]")
    if len(std) != 95 * 5 + 7:
        sys.exit("Fonts.h is not what this test expects (%d values)" % len(std))
    names = {"1": "ENEMY1", "2": "ENEMY2", "X": "EXPLOSION", "A": "ANJUNA", "B": "BAAAHS", "D": "DREAMSTATE",
             "E": "DREAMSTATE"}                    # the sign draws E as D
    env = {"emu": sys.argv[1], "hex": sys.argv[2], "work": sys.argv[4], "std": A.Font(std, 5, 7),
           "icons": {k: icon_table(icons_h, v) for k, v in names.items()}}
    port = int(sys.argv[5]) if len(sys.argv) > 5 else 14470
    os.makedirs(env["work"], exist_ok=True)

    def t(text):
        return ({"type": "T", "str": text}, check_track)

    def icon(name):
        return ({"type": "I", "str": name, "r": 0, "g": 0, "b": 255}, check_icon)

    tracks = [t("A"), t("TRACK"), t("TRACK ID"), t("NINE CHAR"), t("TEN CHARS?"), t("A TRACK ID OF 22 CHARS"),
              t("I \u007f U"), t(" "), t(""),
              ({"type": "D", "str": "HELLO"}, check_marquee_as_it_is),
              ({"type": "N", "str": "CHONKY"}, check_marquee_as_it_is)]
    icons = [icon(name) for name in "12XABDE"]
    groups = [("tracks", tracks, 120), ("icons", icons, 120)]
    results = [None] * len(groups)

    def work(i):
        name, cases, seconds = groups[i]
        try:
            messages = []
            for message, _ in cases:
                messages += [dict(AD), message]
            runs = F.run(env, name, messages, port + i, seconds)
            logs = []
            for k, (message, checker) in enumerate(cases):
                logs += checker(env, message, runs[2 * k], runs[2 * k + 1])
            results[i] = logs
        except Exception as e:  # a run that could not be made or read is a failed test, not a crash
            A.failures.append("%s: %s" % (name, e))
            results[i] = ["  FAIL  %s: %r" % (name, e)]

    threads = [threading.Thread(target=work, args=(i,)) for i in range(len(groups))]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    for (name, _, _), logs in zip(groups, results):
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
