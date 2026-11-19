#!/usr/bin/env python3
"""Nothing of the picture before a message stays lit beside it, on the real firmware image.

A track id ('T') and the icons ('I') send frames that are shorter than the strip: a track id of
nine characters or fewer never reaches the right-hand end of side B, and a row of icons stops 1
to 17 columns short of it. What a frame does not reach keeps what the strip showed before. So
both styles clear the sign first; this test holds them to it:

  - every case comes after a white text that fills both sides to their last column ("the ad":
    eleven characters, so it has moved left until its last character, an H, stands at the right
    edge), and the ad's last frame is checked to be that text, LED for LED, with all seven LEDs
    of the strip's last column lit: what a message leaves behind there is seen
  - the message's first frame is the whole strip, dark
  - then every frame of the message is worked out here, from Fonts.h or Icons.h, and compared
    column for column: for a track id, 60 blank columns and the text, one column further left in
    each frame, as many frames as that row has columns; for the icons, the 98 frames of the
    animation (14 rows of 7 steps) in the message's colour. Each frame is as many bits as its
    columns have, to the bit: 24 a column, no pixel begun and not finished
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

# Eleven characters are 65 columns: the text moves left five, and its last character ends at the
# last column of each side. That is an H: its last column is lit in all seven rows.
AD = {"type": "O", "str": "I FILL BOTH", "r": 255, "g": 255, "b": 255, "dly": 100}
ALL_ROWS = 0xFE


def lit(frame):
    return sum(bin(m).count("1") for m in frame.mask)


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
    cols = env["std"].columns(A.text_bytes(AD))[:-1]         # 65 columns: its end fills a side
    expected = A.side(cols, len(cols) - PANEL)
    check(log, last is not None and last.mask[:PANEL] == expected and last.mask[PANEL:] == expected and
          all(last.colour(c) <= {"ffffff"} for c in range(COLS)),
          "the text before it is on the sign: the end of %s on both sides, white" % json.dumps(AD["str"]))
    # (asked of the frame itself, not of what was worked out: this is what the cases stand on)
    check(log, last is not None and last.mask[PANEL - 1] == ALL_ROWS and last.mask[COLS - 1] == ALL_ROWS and
          last.colour(COLS - 1) == {"ffffff"},
          "and the last column of each side is lit, all seven LEDs: the strip's last column is white before "
          "the message (rows %s)" % ("%02x" % last.mask[COLS - 1] if last is not None else "?"))


def cleared_first(log, frames, wanted=True):
    first = frames[0] if frames else None
    dark = first is not None and first.bits == COLS * 24 and not lit(first)
    if wanted:
        check(log, dark, "its first frame is the whole strip, dark")
    else:
        check(log, first is not None and not dark, "its first frame is its own (no dark frame before it)")
    return dark


# ---- 'T', 'D', 'N': the marquees ------------------------------------------------------------------
MARQUEES = {"T": ("std", False), "D": ("std", True), "N": ("chonk", True)}     # the font; all of the blanks before?
STARS_AFTER_MARQUEE = {"T": 0, "D": 100, "N": 100}


def marquee_state(cases):
    """Gives every marquee of a list what it starts from: `_colour`, the sketch's `sector` and
    `colorStep` (they go on from one marquee to the next: from 1 and 0 at power-on), and
    `_stars_before`, the random numbers the sign has drawn (300 at power-on)."""
    sector, step, drawn = 1, 0, F.STARS_AT_START
    for message, _ in cases:
        if message["type"] not in MARQUEES:
            continue
        message["_colour"], message["_stars_before"] = (sector, step), drawn
        sector = (sector + 1) % 3               # (every call of marquee() moves on a sector first)
        text = A.text_bytes(message)
        if text:
            font, padded = MARQUEES[message["type"]]
            blanks = COLS // ({"std": 5, "chonk": 8}[font] + 1)
            for _ in range((blanks if padded else blanks - blanks // 2) + len(text)):
                sector, step = ((sector + 1) % 3, 0) if step == 255 else (sector, step + 1)
        drawn += STARS_AFTER_MARQUEE[message["type"]]


def marquee_parts(env, message, strip):
    """marquee() with sendPaddedString(), frame for frame. The row is blanks (as many characters as
    the strip holds: 20, of the chonky font 13; a track id starts with half of them gone) and the
    text. It is walked a character at a time and each character a column at a time (as many steps
    as the font is wide and one more; the heart's width when the heart is at the front). A frame
    is what is left of the character at the front, the blanks still to come, the text's first
    character, and as many characters more as the strip holds less the blanks sent. Every
    character has its blank column; a vertical tab is that column alone, at every step.
    The colour goes round with the characters: one step of 255 per character, in three sectors
    (red up and green down, red down and blue up, green up and blue down), through Gamma.h; no
    beat comes in to whiten it. Then the stars, if the style has them."""
    font_name, padded = MARQUEES[message["type"]]
    font, text, gamma = env[font_name], A.text_bytes(message), env["gamma"]
    holds = COLS // (font.cols + 1)
    blanks = holds if padded else holds - holds // 2
    sector, step = message["_colour"]
    sector = (sector + 1) % 3

    def cell(ch, skip=0):
        glyph = font.columns(bytes([ch]))[:-1]
        return glyph[min(skip, len(glyph)):] + [0]

    wants = []
    for at in range(blanks + len(text) if text else 0):
        sector, step = ((sector + 1) % 3, 0) if step == 255 else (sector, step + 1)
        up, down = gamma[step], gamma[255 - step]
        rgb = ((up, down, 0), (down, 0, up), (0, up, down))[sector]
        left, front = max(0, blanks - at), max(0, at - blanks)
        for skip in range((font.heart_cols if not left and text[front] == A.HEART else font.cols) + 1):
            more = holds
            if left:
                rows = cell(0x20, skip) + cell(0x20) * (left - 1) + cell(text[0])
                more -= left - 1
            else:
                rows = cell(text[front], skip)
            for ch in text[front + 1:front + 1 + more]:
                rows += cell(ch)
            wants.append(strip.frame([(r, rgb) for r in rows]))
    parts = [F.Part("the marquee: the blanks and the text, a column further in every frame", wants)]
    if STARS_AFTER_MARQUEE[message["type"]]:
        parts.append(F.starfield(strip, STARS_AFTER_MARQUEE[message["type"]], message["_stars_before"]))
    return parts


def after_the_ad(env):
    """The strip as the text before a case leaves it."""
    cols = env["std"].columns(A.text_bytes(AD))[:-1]
    one = A.side(cols, len(cols) - PANEL)
    strip = F.Strip()
    strip.clear()
    return strip, strip.frame([(rows, (255, 255, 255)) for rows in one + one])


def check_track(env, message, ad_result, result):
    """A track id: the clear, then its marquee, each frame worked out. What a frame does not
    reach is dark, because the clear made it so; and when it has passed, the sign is dark."""
    start, end, frames = result
    log = ["%s %s, after a white text" % (message["type"], json.dumps(message["str"]))]
    ad_checks(env, log, ad_result)
    if not F.asked_again(log, start, end):
        return log
    cleared_first(log, frames)
    strip = F.Strip()
    parts = [F.Part("the clear", [strip.clear()])] + marquee_parts(env, message, strip)
    F.check_parts(log, frames, parts)
    check(log, bool(frames) and not lit(frames[-1]), "when it has passed, the sign is dark")
    return log


def check_marquee_as_it_is(env, message, ad_result, result):
    """The default and the chonky marquee: no clear, and none needed. Their frames are worked out
    on top of what the text before them left on the strip, and then their 100 stars."""
    start, end, frames = result
    log = ["%s %s, after a white text" % (message["type"], json.dumps(message["str"]))]
    ad_checks(env, log, ad_result)
    if not F.asked_again(log, start, end):
        return log
    cleared_first(log, frames, wanted=False)
    check(log, bool(frames) and frames[0].bits >= COLS * 24, "and that frame is as long as the strip or longer: "
          "nothing of the text before it is left (%d columns)" % (frames[0].bits // 24 if frames else 0))
    strip, _ = after_the_ad(env)
    F.check_parts(log, frames, marquee_parts(env, message, strip))
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
    # (to the bit: a frame with part of one more pixel behind it is not that frame. A's frames are
    # up to 121 columns, one more than the strip has.)
    right = sum(1 for f, cols in zip(own, wanted)
                if f.bits == 24 * len(cols) and tuple(f.mask[:len(cols)]) == tuple(cols[:COLS]))
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
    fenv = F.environment(root)
    names = {"1": "ENEMY1", "2": "ENEMY2", "X": "EXPLOSION", "A": "ANJUNA", "B": "BAAAHS", "D": "DREAMSTATE",
             "E": "DREAMSTATE"}                    # the sign draws E as D
    env = {"emu": sys.argv[1], "hex": sys.argv[2], "work": sys.argv[4], "std": A.Font(std, 5, 7),
           "chonk": fenv["chonk"], "gamma": fenv["gamma"],
           "icons": {k: icon_table(icons_h, v) for k, v in names.items()}}
    port = int(sys.argv[5]) if len(sys.argv) > 5 else 14470
    os.makedirs(env["work"], exist_ok=True)

    def t(text):
        return ({"type": "T", "str": text}, check_track)

    def icon(name):
        return ({"type": "I", "str": name, "r": 0, "g": 0, "b": 255}, check_icon)

    tracks = [t("A"), t("TRACK"), t("TRACK ID"), t("NINE CHAR"), t("TEN CHARS?"), t("A TRACK ID OF 22 CHARS"),
              t("I \u007f U"), t("\u007f FIRST"), t("A\u000bB"), t("\u00e9"), t(" "), t(""),
              ({"type": "D", "str": "HELLO"}, check_marquee_as_it_is),
              ({"type": "N", "str": "CHONKY"}, check_marquee_as_it_is)]
    marquee_state(tracks)
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
