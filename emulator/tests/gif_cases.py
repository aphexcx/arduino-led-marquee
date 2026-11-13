#!/usr/bin/env python3
"""Puts chosen and random pictures through the recorder's GIF encoder
(build/tests/gif_case) and decodes each result with the strict decoder in
gif_check.py, comparing every pixel and delay with the frames dump.

usage: gif_cases.py <gif_case program> <work dir>
Python 3 standard library only.
"""
import os
import subprocess
import sys

import gif_check


def run_case(program, work, name, commands):
    gif, dump = os.path.join(work, name + ".gif"), os.path.join(work, name + ".jsonl")
    script = "open %s %s %s\n%s" % (gif, dump, commands[0], "".join(c + "\n" for c in commands[1:]))
    subprocess.run([program], input=script.encode(), check=True)
    return gif_check.compare(gif, dump)


def main():
    program, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    cases = []

    # The gate's case: stacked layout, gamma 1, scale 4, tick 2; one picture with
    # row 0, columns 0-42 white at cycle 0; closed at cycle 320000. The last
    # pixel code leaves the decoder's dictionary exactly full for 9-bit codes.
    cases.append(("gate", ["4 2 0 1", "fill 0 0 0 42 ffffff", "close 320000"]))

    # Every run length of white on row 0, at three scales and both layouts: the
    # number of codes in a frame walks across the code-width boundaries.
    for scale in (2, 4, 6):
        for band in (0, 1):
            commands = ["%d 2 %d 1" % (scale, band)]
            for last in range(120):
                commands.append("fill %d 0 0 %d ffffff" % (last * 320000, last))
            commands.append("close %d" % (120 * 320000))
            cases.append(("runs-s%d-b%d" % (scale, band), commands))

    # Random pictures: few colours to many (more than 256 makes the recorder
    # drop low bits), sparse to dense (dense frames fill the 4096-entry
    # dictionary and force clear codes in the middle of a frame).
    seed = 1
    for colours in (1, 2, 3, 7, 60, 250, 400, 1000):
        for percent in (2, 30, 100):
            commands = ["3 2 %d 2.2" % (seed % 2)]
            for i in range(12):
                commands.append("rand %d %d %d %d" % (i * 640000, seed, colours, percent))
                seed += 1
            commands.append("close %d" % (12 * 640000))
            cases.append(("random-c%d-p%d" % (colours, percent), commands))

    failed = frames = 0
    for name, commands in cases:
        try:
            n, _ = run_case(program, work, name, commands)
            frames += n
        except gif_check.GifError as e:
            failed += 1
            print("  FAIL  %s: %s" % (name, e))
    if not failed:
        print("  ok    %d GIF cases, %d frames: every frame decodes strictly and matches its "
              "frames dump (the first gate's case, 720 run lengths, 288 random pictures)"
              % (len(cases), frames))

    # Long holds. One GIF delay field holds at most 655.35 s, so a picture that
    # stays longer must be split, and the pieces must add up. Each case names
    # the simulated seconds and the fewest frames the format allows.
    second = 16000000
    holds = [
        # (name, settings, commands after the first picture, total seconds, frames)
        ("the second gate's case: 1000 s", "2 2 0 1", [], 1000, 2),
        ("exactly 655.35 s", "2 1 0 1", [], "655.35", 1),
        ("655.36 s, one hundredth more", "2 1 0 1", [], "655.36", 2),
        ("655.35 s with a tick of 5/100 s", "2 5 0 1", [], "655.35", 1),
        ("1310.70 s, twice the field", "2 1 0 1", [], "1310.70", 2),
        ("1310.71 s", "2 1 0 1", [], "1310.71", 3),
        ("6553.50 s, ten times the field", "2 2 0 1", [], "6553.50", 10),
        ("100000 s", "2 2 0 1", [], 100000, 153),
        ("a 2000 s hold in the middle", "2 2 0 1",
         ["fill %d 1 0 9 ff0000" % (2000 * second), "fill %d 2 0 9 00ff00" % (2001 * second)],
         2002, 4 + 1 + 1),
        ("a short picture, then a 2000 s hold to the end", "2 2 0 1",
         ["fill %d 1 0 9 ff0000" % second], 2001, 1 + 4),
    ]
    held = 0
    for i, (name, settings, more, seconds, want_frames) in enumerate(holds):
        whole, _, hundredths = str(seconds).partition(".")
        close = int(whole) * second + int(hundredths or 0) * (second // 100)
        commands = [settings, "fill 0 0 0 0 ffffff", *more, "close %d" % close]
        try:
            n, played = run_case(program, work, "hold%d" % i, commands)
            if n != want_frames:
                raise gif_check.GifError("%d frames; the fewest the format allows is %d"
                                         % (n, want_frames))
            if abs(played - float(seconds)) > 1e-9:
                raise gif_check.GifError("plays for %.2f s" % played)
            held += 1
        except gif_check.GifError as e:
            failed += 1
            print("  FAIL  long hold, %s: %s" % (name, e))
    if held == len(holds):
        print("  ok    %d long holds keep their whole duration, in the fewest frames the GIF "
              "delay field allows (655.35 s exactly, one hundredth more, multiples, 1000 s, "
              "100000 s, a hold in the middle, a hold at the end)" % held)
    # Two LED values that are drawn alike are one picture. At gamma 2.2 the
    # values 254 and 255 are both drawn as 255, so the second frame here changes
    # nothing that is drawn: one GIF frame for the whole run, not two alike.
    # (compare() fails on two frames where the format needs one.)
    # Then a frame stamped earlier than the one before it: the recorder must
    # keep time running forward, not write a negative stay.
    extra = [
        ("LED values 255 and 254 are one picture",
         ["4 2 0 2.2", "fill 0 0 0 0 ffffff", "fill 640000 0 0 0 feffff", "close 1280000"], 1),
        ("LED values 255 and 200 are two",
         ["4 2 0 2.2", "fill 0 0 0 0 ffffff", "fill 640000 0 0 0 c8c8c8", "close 1280000"], 2),
        ("a frame stamped before the previous one",
         ["2 2 0 1", "fill 640000 0 0 0 ffffff", "fill 1280000 1 0 9 ff0000",
          "fill 0 2 0 9 00ff00", "close 1920000"], 3),
    ]
    passed = 0
    for i, (name, commands, want_frames) in enumerate(extra):
        try:
            n, _ = run_case(program, work, "extra%d" % i, commands)
            if n != want_frames:
                raise gif_check.GifError("%d frames, expected %d" % (n, want_frames))
            passed += 1
        except (gif_check.GifError, subprocess.CalledProcessError) as e:
            failed += 1
            print("  FAIL  %s: %s" % (name, e))
    if passed == len(extra):
        print("  ok    a change of LED values that changes nothing drawn writes no new frame; a "
              "real change does; a frame stamped out of order does not break the recording")
    if failed:
        print("FAILED: %d GIF case(s)" % failed)
        sys.exit(1)


if __name__ == "__main__":
    main()
