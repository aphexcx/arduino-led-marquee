#!/usr/bin/env python3
"""Checks a frames dump (sign-emu --frames) from a run of playlists/all_types.jsonl.

Expected pictures are computed here from Fonts.h, not copied from an earlier
run, so the checks hold the emulator to the font data the firmware was built
from. Python 3 standard library only.

usage: check_frames.py <Fonts.h> <frames.jsonl> [<clip.gif>]
"""
import json
import re
import sys

import gif_check

COLS, PANEL, ROWS = 120, 60, 7
failures = []


def check(ok, text):
    print("  %s  %s" % ("ok  " if ok else "FAIL", text))
    if not ok:
        failures.append(text)


# ---- the standard font, from the firmware's header ---------------------------
def load_font_std(path):
    """Returns {char: [5 column bytes]}. Bit 7 of a column byte is the top row."""
    src = open(path, encoding="utf-8", errors="replace").read()
    body = src[src.index("FontStd5x7[]"):]
    body = body[body.index("{") + 1:body.index("};")]
    body = re.sub(r"//[^\n]*", "", body)
    values = [int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+|0[bB][01]+", body)]
    return {chr(0x20 + i): values[i * 5:i * 5 + 5] for i in range(95)}


def text_columns(font, text):
    """Column bytes of text in the standard font, one blank column between characters."""
    cols = []
    for i, ch in enumerate(text):
        if i:
            cols.append(0)
        cols += font[ch]
    return cols


# ---- frames -------------------------------------------------------------------
class Frame:
    def __init__(self, d):
        self.t = d["t"]
        self.d = d
        self.px = [[r[c * 6:c * 6 + 6] for c in range(COLS)] for r in d["rows"]]
        # One byte per column, bit 7 = top row, like the font data.
        self.mask = [sum((0x80 >> row) for row in range(ROWS) if self.px[row][c] != "000000")
                     for c in range(COLS)]

    def colours(self, first, last):
        return {self.px[row][c] for row in range(ROWS) for c in range(first, last)} - {"000000"}


def find_alone(side, cols):
    """Column at which one side (60 column bytes) shows exactly cols and nothing else."""
    n = len(cols)
    for off in range(PANEL - n + 1):
        if side[off:off + n] == cols and not any(side[:off]) and not any(side[off + n:]):
            return off
    return None


def find_on_both_sides(frame, cols):
    a, b = find_alone(frame.mask[:PANEL], cols), find_alone(frame.mask[PANEL:], cols)
    return None if a is None or b is None else (a, b)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    font = load_font_std(sys.argv[1])
    frames, tx, replies, end_t = [], [], [], 0.0
    pending = bytearray()
    for line in open(sys.argv[2], encoding="utf-8"):
        d = json.loads(line)
        end_t = max(end_t, d.get("t", 0.0))
        if d["type"] == "frame":
            frames.append(Frame(d))
        elif d["type"] == "tx":
            if pending:
                replies.append((tx[-1]["t"] if tx else 0.0, bytes(pending)))
                pending.clear()
            tx.append(d)
        elif d["type"] == "rx":
            pending.append(d["byte"])
    if pending:
        replies.append((tx[-1]["t"], bytes(pending)))

    print("frames dump: %d frames, %d bytes from the sign, %d replies, %.3f s simulated"
          % (len(frames), len(tx), len(replies), end_t))

    print("frames")
    check(len(frames) > 100, "frames were latched (%d)" % len(frames))
    bad = [f for f in frames if f.d["marginal"] or f.d["partial_bits"]]
    check(not bad, "every WS2812 pulse is a clean 0 or 1 and every frame is whole pixels")
    t0 = {tuple(f.d["t0h_cycles"]) for f in frames} - {(0, 0)}
    t1 = {tuple(f.d["t1h_cycles"]) for f in frames} - {(0, 0)}
    check(t0 == {(5, 5)} and t1 == {(13, 13)},
          "0 bits are high for 5 CPU cycles and 1 bits for 13, as PixelBitBanging.h says "
          "(seen %s and %s)" % (sorted(t0), sorted(t1)))

    print("startup animation, against Fonts.h")
    first_request = tx[0]["t"] if tx else end_t
    last_seen = 0.0
    for word in ["I", "LIVE", "FOR", "THAT", "ENERGY!"]:
        cols = text_columns(font, word)
        hits = [(f, find_on_both_sides(f, cols)) for f in frames if last_seen < f.t < first_request]
        hits = [(f, where) for f, where in hits if where is not None]
        if hits:
            f, (a, b) = hits[0]
            last_seen = f.t
            note = "" if a == b else (" (side B is one column to the right: the firmware sends "
                                      "%d columns per side for this string)" % (PANEL + b - a))
            check(True, '"%s" drawn exactly as the standard font has it, alone on each side, at '
                        'column %d of side A and %d of side B, in %d frames from t=%.3f s%s'
                  % (word, a, b, len(hits), f.t, note))
        else:
            check(False, '"%s" drawn exactly as the standard font has it, alone on each side, '
                         'after the previous word' % word)

    print("serial link")
    check(bool(tx) and tx[0].get("char") == "~", "the sign sent ~ (first at t=%.3f s)" % first_request)
    others = [d for d in tx if d.get("char") not in ("~", "?")]
    check(not others, "the sign sent only request bytes: it parsed every reply "
                      "(%d other bytes)" % len(others))
    parsed = []
    for t, raw in replies:
        try:
            parsed.append((t, json.loads(raw.decode("ascii"))))
        except ValueError:
            parsed.append((t, None))
    check(len(parsed) == len(tx) - 1 and all(m is not None for _, m in parsed),
          "each request but the last (which ends the run) was answered with one JSON object "
          "(%d requests, %d replies)" % (len(tx), len(parsed)))
    types = [m.get("type") for _, m in parsed if m]
    wanted = list("OCHUKFSITNDW")
    check(all(t in types for t in wanted),
          "the playlist covered every message type (%s)" % "".join(sorted(set(types))))

    def window(msg_type):
        """(start, end) of the first message of that type: its request to the next request."""
        for i, (t, m) in enumerate(parsed):
            if m and m.get("type") == msg_type:
                later = [d["t"] for d in tx if d["t"] > t]
                return t, (later[0] if later else end_t), m
        return None

    print("chooser message, against Fonts.h")
    w = window("H")
    if w:
        start, end, m = w
        count_cols = text_columns(font, m["flashy"])
        text_cols = text_columns(font, m["str"])
        expected = count_cols + [0] + text_cols
        n = len(expected)
        inside = [f for f in frames if start < f.t <= end + 0.001]
        exact = [f for f in inside if f.mask[:n] == expected]
        check(bool(exact), 'a frame shows "%s", one blank column, then "%s" from column 0 '
                           '(%d of %d frames; the rest are the fade to black)'
              % (m["flashy"], m["str"], len(exact), len(inside)))
        if exact:
            red = exact[0].colours(0, len(count_cols))
            check(all(c[2:] == "0000" and c[:2] != "00" for c in red),
                  "the count is pure red (%s)" % ",".join(sorted(red)))
            check(not any(exact[0].mask[n:PANEL]), "the rest of side A is dark in that frame")
    else:
        check(False, "the run contains a chooser message")

    print("marquee direction")
    w = window("D")
    if w:
        start, end, m = w
        # Whole frames with text in them. The starfield that follows the marquee
        # lights one LED per frame, so frames with fewer than two lit LEDs are out.
        inside = [f for f in frames if start < f.t <= end and f.d["bits"] >= COLS * 24
                  and sum(bin(m).count("1") for m in f.mask) >= 2]
        pairs = list(zip(inside, inside[1:]))
        left = sum(1 for a, b in pairs if b.mask[:COLS - 1] == a.mask[1:])
        check(len(pairs) > 50 and left == len(pairs),
              '"%s" moves one column towards column 0 in every frame: it enters at the right '
              'edge of side B and leaves at the left edge of side A (%d of %d frame pairs)'
              % (m["str"], left, len(pairs)))
        text_cols = text_columns(font, m["str"][:6])
        reads = any(f.mask[c:c + len(text_cols)] == text_cols
                    for f in inside for c in range(COLS - len(text_cols)))
        check(reads, 'its first characters "%s" read left to right as the standard font has them'
              % m["str"][:6])
    else:
        check(False, "the run contains a default marquee message")

    if len(sys.argv) > 3:
        print("recording")
        try:
            n, seconds = gif_check.compare(sys.argv[3], sys.argv[2])
            check(n > 10, "all %d GIF frames decode with a strict LZW decoder, and every pixel "
                          "and frame delay is what the frames dump gives" % n)
            check(abs(seconds - end_t) <= 0.05,
                  "the GIF plays for %.2f s, the simulated time it covers (%.2f s)" % (seconds, end_t))
        except gif_check.GifError as e:
            check(False, "the GIF decodes and matches the frames dump: %s" % e)

    if failures:
        print("FAILED: %d check(s)" % len(failures))
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
