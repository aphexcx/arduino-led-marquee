#!/usr/bin/env python3
"""Chooser contract version 3.1 (the app pushes, the sign does not poll),
measured on the real firmware.

  push_test.py run <sign-emu> <firmware.hex> <Fonts.h> <work dir> [port] [--record out.gif]
      A scripted app: a 64-character chooser message pushed just as the sign
      asks for its next message, two changes pushed while it scrolls, a change
      that is NOT pushed (found at the next stand-still), and the app leaving
      chooser mode (attention and no message). Every moment is placed in
      simulated time, so the run repeats exactly. Then the checks below.

  push_test.py check <Fonts.h> <frames.jsonl>
      The same checks on the frames dump of any run (for example one driven by
      fake_app.py --contract 3). What the app sent is read from the dump.

The checks, all from the dump (sign-emu --frames):
  - a push is on the LEDs within 150 ms of the end of its break
  - between pushes, the time from one scrolled frame to the next varies by no
    more than 2 ms
  - the sign sends no byte while a text is moving, and never a '?'
  - a change that was not pushed is on the LEDs at the next stand-still
  - when the line goes LOW and no message follows, the sign asks 150 ms later
  - chooser frames are panel A only (60 columns); panel B stays dark

The bench build of the firmware (make firmware-debug, CHOOSER_DEBUG_PANEL_B)
also draws what it measured on panel B and sends the same numbers to the app
as a line of text in front of a request, "#SSS fFFF eEEE aAA bBB lLLL~". Both
are read here and held against what was on the wire.

Expected pictures are computed from Fonts.h. Python 3 standard library only.
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

from check_frames import PANEL, Frame, find_on_both_sides, load_font_std, text_columns

CYCLES_PER_MS = 16000
GAP_COLS = 18                 # CHOOSER_GAP_COLS in the sketch
STEP_MS = 37                  # the longest the sign goes between two looks at the line while it scrolls
BENCH_LINE = re.compile(r"#(\d{3}) f(\d{3}) e(\d{3}) a(\d{2}) b(\d{2}) l(\d{3})(?=~)")
BENCH_COLS = (PANEL - 1) * 24 + PANEL * 24      # bits of the frame that brings panel B up to date
PUSH_LIMIT_MS = 150.0
STEP_SPREAD_LIMIT_MS = 2.0
failures = []


def check(ok, text):
    print("  %s  %s" % ("ok  " if ok else "FAIL", text))
    if not ok:
        failures.append(text)


# ---- what a chooser message looks like ---------------------------------------
class Message:
    """A chooser message and every picture of it on panel A."""

    def __init__(self, font, obj):
        self.obj = obj
        self.text = obj.get("str", "")
        self.count = obj.get("flashy", "")
        count_cols = text_columns(font, self.count)[:PANEL]
        view = max(0, PANEL - 1 - len(count_cols))
        text_cols = text_columns(font, self.text)
        self.scrolls = view > 0 and len(text_cols) > view
        self.cycle = len(text_cols) + GAP_COLS
        head = count_cols + [0]
        self.pictures = {}
        if self.scrolls:
            strip = text_cols + [0] * GAP_COLS
            for pos in range(self.cycle):
                picture = (head + [strip[(pos + v) % self.cycle] for v in range(view)])[:PANEL]
                self.pictures.setdefault(tuple(picture), pos)
        else:
            # one that fits blinks: the text as it is, or faded to black
            for body in (text_cols, []):
                picture = (head + body + [0] * PANEL)[:PANEL]
                self.pictures.setdefault(tuple(picture), 0)

    def name(self):
        return '"%s" %s' % (self.text if len(self.text) <= 24 else self.text[:21] + "...", self.count)


def began(frame):
    """When the firmware began to send a frame, in seconds: its latch moment less the time
    it took to clock out and the latch time (50 us unless sign-emu was told otherwise)."""
    return frame.t - frame.d["send_us"] / 1e6 - 0.00005


def in_gap(dump, before, after, b):
    """True when break b belongs between frames number `before` and `after`: every frame up
    to `before` was begun before the line fell (give or take the moment between the sign's
    last look at the line and the frame's first bit), and `after` was begun later."""
    return began(dump.frames[before]) < b["t"] + 0.001 <= began(dump.frames[after])


class Dump:
    def __init__(self, path):
        self.frames, self.tx, self.rx, self.breaks = [], [], [], []
        self.end_t = 0.0
        for line in open(path, encoding="utf-8"):
            d = json.loads(line)
            kind = d["type"]
            if kind == "frame":
                self.frames.append(Frame(d))
            elif kind == "tx":
                self.tx.append(d)
            elif kind == "rx":
                self.rx.append(d)
            elif kind == "break":
                self.breaks.append(d)
            elif kind == "end":
                self.end_t = d["t"]
        self.rx.sort(key=lambda d: d["cycle"])
        self.breaks.sort(key=lambda d: d["cycle"])
        self.tx.sort(key=lambda d: d["cycle"])
        # What the sign sent that is not a request: the bench build's lines of numbers.
        # self.said: (the tx event of the '#', the six numbers). self.tx keeps only the rest.
        text = "".join(d.get("char", "\x00") for d in self.tx)
        self.said, line_bytes = [], set()
        for m in BENCH_LINE.finditer(text):
            self.said.append((self.tx[m.start()], [int(g) for g in m.groups()]))
            line_bytes.update(range(m.start(), m.end()))
        self.all_tx = self.tx
        self.tx = [d for k, d in enumerate(self.tx) if k not in line_bytes]

    def objects(self):
        """The JSON objects the app sent, in order: (first rx event, last rx event, object)."""
        found, i = [], 0
        text = bytes(d["byte"] for d in self.rx).decode("latin-1")
        decoder = json.JSONDecoder()
        while i < len(text):
            if text[i] != "{":
                i += 1
                continue
            try:
                obj, end = decoder.raw_decode(text, i)
            except ValueError:
                i += 1
                continue
            found.append((self.rx[i], self.rx[end - 1], obj))
            i = end
        return found


def analyse(dump, font, expect=None):
    """expect: for the scripted run, what must be found (counts of pushes and so on)."""
    objects = dump.objects()
    messages = {}
    for _, _, obj in objects:
        if obj.get("type") == "H":
            key = (obj.get("str", ""), obj.get("flashy", ""))
            if key not in messages:
                messages[key] = Message(font, obj)
    print("%d frames, %d bytes from the sign, %d objects and %d breaks from the app, %.3f s simulated"
          % (len(dump.frames), len(dump.tx), len(objects), len(dump.breaks), dump.end_t))
    check(bool(messages), "the app sent chooser messages (%d different ones)" % len(messages))
    if not messages:
        return

    # ---- which message and position every frame shows ------------------------
    bench = bool(dump.said)        # the bench build: numbers on panel B, lines to the app
    digits = {tuple(font[str(d)]): str(d) for d in range(10)}

    def panel_b_numbers(f):
        """The four numbers the bench build draws on panel B, None if that is not what is there."""
        out = []
        for col, n in ((60, 3), (78, 3), (96, 2), (108, 2)):
            text = "".join(digits.get(tuple(f.mask[col + 6 * k:col + 6 * k + 5]), "?") for k in range(n))
            if "?" in text:
                return None
            out.append(int(text))
        return out

    seen = []                      # (frame index, Message, position) for chooser frames
    for i, f in enumerate(dump.frames):
        if any(f.mask[PANEL:]) and not (bench and panel_b_numbers(f)):
            continue               # something else on panel B: not the chooser
        picture = tuple(f.mask[:PANEL])
        hits = [(m, m.pictures[picture]) for m in messages.values() if picture in m.pictures]
        if len(hits) == 1 and f.d["bits"] in ((PANEL * 24, BENCH_COLS) if bench else (PANEL * 24,)):
            seen.append((i, hits[0][0], hits[0][1]))
    check(len(seen) > 50, "frames that show a chooser message, as Fonts.h draws it: %d" % len(seen))
    if not seen:
        return
    first, last = seen[0][0], seen[-1][0]
    inside = dump.frames[first:last + 1]
    shown = {i for i, _, _ in seen}
    strays = [first + k for k, f in enumerate(inside)
              if first + k not in shown and (any(f.mask) or f.d["bits"] != 2 * PANEL * 24)]
    check(not strays, "from the first of them to the last, every frame is one of those pictures "
                      "whole, or the blanking of the strip before a new message (%d are not%s)"
          % (len(strays), ": first at t=%.3f" % dump.frames[strays[0]].t if strays else ""))
    blankings = [f for k, f in enumerate(inside) if first + k not in shown]
    sixty = [f.d["send_us"] / 1000.0 for i, f in enumerate(dump.frames)
             if i in shown and f.d["bits"] == PANEL * 24]
    if bench:
        longer = [f for i, f in enumerate(dump.frames) if i in shown and f.d["bits"] == BENCH_COLS]
        check(bool(longer) and all(panel_b_numbers(f) for i, f in enumerate(dump.frames) if i in shown),
              "the bench build: chooser frames are 60 columns (%.3f to %.3f ms), but for %d of 119 "
              "that bring the four numbers on panel B up to date (%.3f ms); panel B shows "
              "numbers and nothing else"
              % (min(sixty), max(sixty), len(longer), max(f.d["send_us"] for f in longer) / 1000.0))
    else:
        check(not any(any(f.mask[PANEL:]) for f in inside),
              "every chooser frame is panel A only: 1440 bits per strip (60 columns), clocked out "
              "in %.3f to %.3f ms; panel B stays dark" % (min(sixty), max(sixty)))
    if blankings:
        whole = [f.d["send_us"] / 1000.0 for f in blankings]
        print("        (the strip is blanked %d time(s), once per new message: 2880 bits in "
              "%.3f to %.3f ms)" % (len(blankings), min(whole), max(whole)))

    # ---- runs: one message scrolling undisturbed ------------------------------
    runs = []                      # lists of (frame index, Message, position)
    for item in seen:
        i, m, pos = item
        if runs:
            pi, pm, ppos = runs[-1][-1]
            follows = pm is m and (not m.scrolls or pos == (ppos + 1) % m.cycle) and \
                not any(in_gap(dump, pi, i, b) for b in dump.breaks) and \
                all(k in shown for k in range(pi, i))
            if follows:
                runs[-1].append(item)
                continue
        runs.append([item])
    print("steady scroll")
    all_steps, holds = [], []
    for run in runs:
        m = run[0][1]
        if not m.scrolls:
            continue
        steps = []
        for (i, _, pos), (j, _, _) in zip(run, run[1:]):
            dt = (dump.frames[j].t - dump.frames[i].t) * 1000.0
            (holds if pos == 0 else steps).append(dt)
        if len(steps) < 2:
            continue
        all_steps += steps
        spread = max(steps) - min(steps)
        check(spread <= STEP_SPREAD_LIMIT_MS,
              "%s from t=%.3f s: %d scrolled frames in a row, %.3f to %.3f ms apart: they vary "
              "by %.3f ms (limit %.0f)" % (m.name(), dump.frames[run[0][0]].t, len(steps),
                                           min(steps), max(steps), spread, STEP_SPREAD_LIMIT_MS))
        check(run[0][2] == 0, "    ... and it began at the start of the text")
    if all_steps:
        spread = max(all_steps) - min(all_steps)
        check(spread <= STEP_SPREAD_LIMIT_MS,
              "all %d scrolled frames of the run: %.3f to %.3f ms apart, vary by %.3f ms"
              % (len(all_steps), min(all_steps), max(all_steps), spread))
    else:
        check(False, "the run contains a message that scrolls")
    if holds:
        check(min(holds) >= 600.0 and max(holds) < 610.0,
              "the start of a text stands still for %.1f to %.1f ms (%d times)"
              % (min(holds), max(holds), len(holds)))

    # ---- pushes ---------------------------------------------------------------
    print("pushes")
    pushes, leaves = [], []        # a break with a message behind it; a break with none
    for b in dump.breaks:
        after = [o for o in objects if o[0]["cycle"] >= b["end"]]
        nxt = min((x["cycle"] for x in dump.breaks if x["cycle"] > b["cycle"]), default=None)
        asked = [d for d in dump.tx if d["cycle"] > b["end"]]
        spoke = [d for d in dump.all_tx if d["cycle"] > b["end"]]
        if after and (nxt is None or after[0][0]["cycle"] < nxt) and \
                not (asked and asked[0]["cycle"] < after[0][0]["cycle"]):
            pushes.append((b, after[0]))
        else:
            leaves.append((b, asked[0] if asked else None, spoke[0] if spoke else None))
    check(all(0 <= b["cycle"] - b["due"] <= 4 for b in dump.breaks),
          "every break fell at the first instruction boundary from its due cycle (%d breaks, %s ms)"
          % (len(dump.breaks), ", ".join(sorted({"%g" % b["ms"] for b in dump.breaks}))))
    guards = sorted({(o[0]["due"] - b["end"]) / CYCLES_PER_MS for b, o in pushes})
    check(bool(pushes),
          "%d of them are followed by a message unasked; its first start bit is due %s ms "
          "after the break ended" % (len(pushes), ", ".join("%g" % g for g in guards)))
    # 10 bit times a byte, each edge at the nearest whole cycle
    check(all(abs((z["due"] - a["due"]) - round((dump.rx.index(z) - dump.rx.index(a)) * 100000 / 9))
              <= 1 for _, (a, z, _) in pushes),
          "and its bytes follow one another back to back at 14400 baud")
    worst = 0.0
    for b, (_, last_byte, obj) in pushes:
        end_t = b["end"] / (CYCLES_PER_MS * 1000.0)
        if obj.get("type") == "H":
            m = messages[(obj.get("str", ""), obj.get("flashy", ""))]
            hit = [dump.frames[i].t for i, fm, pos in seen
                   if fm is m and pos == 0 and dump.frames[i].t >= end_t]
            what = m.name()
        else:
            cols = text_columns(font, obj.get("str", ""))
            hit = [f.t for f in dump.frames if f.t >= end_t and find_on_both_sides(f, cols)]
            what = 'type %s "%s" (it leaves chooser mode, and is drawn on both panels)' \
                % (obj.get("type"), obj.get("str", ""))
        if not hit:
            check(False, "the pushed %s is shown after its break at t=%.3f s" % (what, b["t"]))
            continue
        ms = (hit[0] - end_t) * 1000.0
        worst = max(worst, ms)
        check(ms <= PUSH_LIMIT_MS,
              "break at t=%.3f s: %s is on the LEDs %.1f ms after the end of the break (limit "
              "%.0f; its last byte arrived %.1f ms after it)"
              % (b["t"], what, ms, PUSH_LIMIT_MS,
                 (last_byte["cycle"] + 11112 - b["end"]) / CYCLES_PER_MS))

    # ---- attention with nothing behind it: the app has left chooser mode -------
    print("attention and no message (the app leaves chooser mode)")
    after_leave = set()
    for b, ask, spoke in leaves:
        if ask is None:
            check(False, "break at t=%.3f s with no message behind it: the sign asks" % b["t"])
            continue
        after_leave.add(ask["cycle"])
        # (the bench build's line of numbers goes out first, and the '~' ends it)
        waited = (spoke["cycle"] - b["end"]) / CYCLES_PER_MS
        drawn = [f for f in dump.frames if b["t"] + 0.001 < began(f) and f.t < ask["t"]]
        check(150.0 <= waited <= 175.0 and not drawn and ask.get("char") == "~",
              "break at t=%.3f s, nothing behind it: the picture stood still and the sign %s "
              "%.1f ms after the end of the break (it waits 150 for a message first)"
              % (b["t"], "asked '~'" if spoke is ask else "began its line of numbers and its '~'",
                 waited))
        answer = [o for o in objects if o[0]["cycle"] > ask["cycle"]]
        if answer and answer[0][2].get("type") != "H":
            cols = text_columns(font, answer[0][2].get("str", ""))
            hit = [f.t for f in dump.frames if f.t > ask["t"] and find_on_both_sides(f, cols)]
            if hit:
                print("        (the answer, type %s \"%s\", is on both panels %.1f ms after the "
                      "end of the break)" % (answer[0][2].get("type"), answer[0][2].get("str", ""),
                                             (hit[0] - b["end"] / (CYCLES_PER_MS * 1000.0)) * 1000.0))
    if not leaves:
        print("        (none in this run)")

    # ---- the bench build's own account, against the wire ------------------------
    if bench:
        print("the bench build's numbers (ms as the sign counted them)")
        numbered = [n[0] for _, n in dump.said]
        check(numbered == list(range(len(numbered))),
              "%d lines to the app, each 27 characters and the '~' that was due anyway, numbered "
              "%s" % (len(numbered), " ".join("%03d" % n for n in numbered)))
        for first_char, (seq, f_ms, e_ms, att, falls, low_ms) in dump.said:
            before = [b for b in dump.breaks if b["cycle"] < first_char["cycle"]]
            b = before[-1] if before else None
            frame = [o for bb, o in pushes if bb is b]
            line = "#%03d f%03d e%03d a%02d b%02d l%03d~" % (seq, f_ms, e_ms, att, falls, low_ms)
            if b is None:
                check(False, "%s at t=%.3f s follows an attention" % (line, first_char["t"]))
                continue
            # The sign first sees the line LOW at most one step after it fell.
            ok = b["ms"] - STEP_MS - 1 <= low_ms <= b["ms"] + 1 and falls <= att <= len(before)
            told = "the line LOW for %g ms (seen for %d)" % (b["ms"], low_ms)
            if frame:
                to_first = (frame[0][0]["cycle"] + 1111 - b["cycle"]) / CYCLES_PER_MS
                to_end = (frame[0][1]["cycle"] + 11112 - b["cycle"]) / CYCLES_PER_MS
                ok = ok and to_first - STEP_MS - 1 <= f_ms <= to_first + 2 \
                    and to_end - STEP_MS - 1 <= e_ms <= to_end + 4
                told += ", first byte %.1f ms after it fell (counted %d), end of the message " \
                        "%.1f (counted %d)" % (to_first, f_ms, to_end, e_ms)
            else:
                ok = ok and f_ms == 0 and e_ms == 0
                told += ", no message behind it (000 and 000)"
            check(ok, "t=%.3f s  %s  on the wire: %s" % (first_char["t"], line, told))
            shown_b = [panel_b_numbers(dump.frames[i]) for i, _, _ in seen
                       if dump.frames[i].t < first_char["t"]]
            if shown_b and shown_b[-1][2] == att:
                check(shown_b[-1] == [f_ms, e_ms, att, falls],
                      "            and panel B showed the same: %03d %03d %02d %02d" % tuple(shown_b[-1]))

    # ---- what the sign sent ---------------------------------------------------
    print("requests")
    chars = "".join(d.get("char", "?") for d in dump.tx)
    check(set(chars) <= {"~"}, "the sign sent only '~', never '?' and never an error text "
                               "(%d bytes: %s)%s" % (len(chars), chars,
                                                     "; the lines of numbers apart" if bench else ""))
    moving = []
    for d in dump.tx:
        if d["cycle"] in after_leave:
            continue               # the contract's one exception: attention, and no message came
        on = [(i, m, pos) for i, m, pos in seen if dump.frames[i].t <= d["t"]]
        if not on or on[-1][0] != max(k for k, f in enumerate(dump.frames) if f.t <= d["t"]):
            continue               # the chooser was not on the LEDs
        if on[-1][1].scrolls and on[-1][2] != 0:
            moving.append(d["t"])
    check(not moving, "no byte was sent while a text was moving: every other request found the "
                      "text standing on its start, or the chooser not on the LEDs (%d did not)"
          % len(moving))
    for run in runs:
        if not run[0][1].scrolls:
            continue
        scrolled = [i for i, _, pos in run if pos != 0]
        passes, start = [], None
        for i, _, pos in run:
            if pos == 1:
                start = i
            if pos == run[0][1].cycle - 1 and start is not None:
                passes.append((dump.frames[start].t, dump.frames[i].t))
                start = None
        if start is not None and scrolled:
            passes.append((dump.frames[start].t, dump.frames[scrolled[-1]].t))
        inside_pass = [d["t"] for d in dump.tx for a, z in passes if a <= d["t"] <= z]
        if inside_pass:
            check(False, "%s: %d byte(s) between the first scrolled frame and the last of a pass"
                  % (run[0][1].name(), len(inside_pass)))

    # ---- changes that were not pushed -----------------------------------------
    print("changes that were not pushed")
    unpushed = 0
    for before, after in zip(runs, runs[1:]):
        if before[-1][1] is after[0][1]:
            continue
        a, z = dump.frames[before[-1][0]].t, dump.frames[after[0][0]].t
        if any(in_gap(dump, before[-1][0], after[0][0], b) for b in dump.breaks):
            continue
        unpushed += 1
        m, old = after[0][1], before[-1][1]
        asked = [d["t"] for d in dump.tx if a <= d["t"] <= z]
        at_hold = old.scrolls and before[-1][2] == 0 and len(before) > 1 and \
            before[-2][2] == old.cycle - 1
        check(at_hold and len(asked) == 1 and after[0][2] == 0,
              "%s replaced %s without a push: the old text came round, the sign asked '~' with "
              "its start standing still, and the new message was on the LEDs %.1f ms after "
              "that frame" % (m.name(), old.name(), (z - a) * 1000.0))
    if not unpushed:
        print("        (none in this run)")

    if expect:
        print("this run")
        check(len(pushes) == expect["pushes"] and unpushed == expect["unpushed"]
              and len(leaves) == expect["leaves"] and len(dump.tx) == expect["requests"],
              "%d pushes, %d change that was not pushed, %d attention with no message, %d "
              "requests in all (got %d, %d, %d, %d)"
              % (expect["pushes"], expect["unpushed"], expect["leaves"], expect["requests"],
                 len(pushes), unpushed, len(leaves), len(dump.tx)))
        into = (dump.breaks[0]["due"] - dump.tx[0]["cycle"]) / CYCLES_PER_MS if dump.breaks else -1
        listening = [d for d in dump.tx if d["cycle"] > dump.breaks[0]["cycle"]] if dump.breaks else []
        check(0 < into < 20 and len(dump.tx) > 1 and
              pushes and pushes[0][0] is dump.breaks[0] and
              listening[0]["cycle"] > pushes[0][1][1]["cycle"],
              "the first push fell into the sign's own read, %.1f ms after its '~': the sign "
              "read on past the break and took the pushed message as the answer, without "
              "asking again" % into)
        check(all(b["ms"] == expect["break_ms"] for b in dump.breaks) and guards == [expect["guard_ms"]],
              "every break is exactly %g ms and every guard exactly %g ms of simulated time"
              % (expect["break_ms"], expect["guard_ms"]))
        check(worst <= PUSH_LIMIT_MS, "the slowest push took %.1f ms from the end of its break" % worst)


# ---- the scripted run ----------------------------------------------------------
def chooser(text, count):
    assert len(text) == 64 and text == text.strip(), (len(text), text)
    return json.dumps({"type": "H", "str": text, "flashy": count}, separators=(",", ":")).encode("ascii")


def push(at_ms, message):
    """A push placed at_ms after the moment these bytes reach the emulator."""
    return b"@%d\n@break50\n@after20\n" % at_ms + message


def run(emu, hexfile, font_path, work, port, record):
    os.makedirs(work, exist_ok=True)
    dump, log = os.path.join(work, "push.jsonl"), os.path.join(work, "push.log")
    one = chooser("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG AND KEEPS ON RUNNING", "1/4")
    two = chooser("PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS, THEN DANCE TILL SUNRISE", "2/4")
    three = chooser("SPHINX OF BLACK QUARTZ, JUDGE MY VOW: THE BASS DROPS AT MIDNIGHT", "3/4")
    four = chooser("HOW VEXINGLY QUICK DAFT ZEBRAS JUMP WHEN THE LIGHTS COME BACK ON", "4/4")
    bye = b'{"type":"F","str":"BYE"}'
    # Each answer is ONE write, so that everything in it reaches the emulator while it holds the
    # simulated clock for the request: the pushes are then placed from that request, not from
    # how fast this computer is.
    plan = [
        # request 1 (after the startup animation; the sign is in loop()'s own read): the app
        # enters chooser mode at that very moment and pushes the first message. The break
        # falls 5 ms after the request, while the sign is listening for its answer.
        # Then two changes pushed while a text scrolls: 4 s later, and 4 s after that.
        b"@break50\n@after20\n" + one + push(4000, two) + push(8000, three),
        # request 2 (the sign's own read, when the third text has come round and stands still):
        # the app is on a fourth message that it never pushed. 3 s later it leaves chooser
        # mode: the line LOW, and nothing behind it.
        four + b"@3000\n@break50\n",
        # request 3 (the sign asking, because no message followed that attention)
        bye,
    ]
    flags = ["--headless", "--port", str(port), "--wait-client", "30", "--link-directives",
             "--until-requests", str(len(plan) + 1), "--duration", "120", "--frames", dump]
    if record:
        flags += ["--record", record, "--scale", "4"]
    with open(log, "w") as err:
        proc = subprocess.Popen([emu, *flags, hexfile], stderr=err)
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.monotonic() > deadline or proc.poll() is not None:
                    raise
                time.sleep(0.05)
        sock.settimeout(120)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        seen = 0
        try:
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                for byte in data:
                    if byte == ord("~") and seen < len(plan):
                        sock.sendall(plan[seen])
                    seen += byte == ord("~")
        except (ConnectionResetError, BrokenPipeError):
            pass                       # the emulator reached its last request and hung up
        sock.close()
        proc.wait(timeout=180)
    finally:
        if proc.poll() is None:
            proc.kill()
    text = open(log).read()
    sys.stdout.write(text)
    check(proc.returncode == 0 and "WARNING" not in text and "NOTE" not in text,
          "sign-emu ran to the end without a warning (exit status %s)" % proc.returncode)
    analyse(Dump(dump), load_font_std(font_path),
            {"pushes": 3, "unpushed": 1, "leaves": 1, "requests": 4, "break_ms": 50,
             "guard_ms": 20})


def main():
    args = sys.argv[1:]
    record = None
    if "--record" in args:
        at = args.index("--record")
        record = args[at + 1]
        del args[at:at + 2]
    if len(args) >= 5 and args[0] == "run":
        run(args[1], args[2], args[3], args[4], int(args[5]) if len(args) > 5 else 14430, record)
    elif len(args) == 3 and args[0] == "check":
        analyse(Dump(args[2]), load_font_std(args[1]))
    else:
        sys.exit(__doc__)
    if failures:
        print("FAILED: %d check(s)" % len(failures))
        sys.exit(1)
    print("push test: all checks passed")


if __name__ == "__main__":
    main()
