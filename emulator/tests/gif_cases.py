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
    if failed:
        print("FAILED: %d of %d GIF cases" % (failed, len(cases)))
        sys.exit(1)
    print("  ok    %d GIF cases, %d frames: every frame decodes strictly and matches its "
          "frames dump (the gate's case, 720 run lengths, 288 random pictures)"
          % (len(cases), frames))


if __name__ == "__main__":
    main()
