#!/usr/bin/env python3
"""ad_display_test over the settings: every wave interval, with scroll steps that are and are not
multiples of it.

AdDisplay.h accepts some pairs of AD_SCROLL_STEP_MS and AD_WAVE_FRAME_MS and refuses the others
at compile time. The host test must go with it both ways: it passes for every pair the header
accepts, and for a pair the header refuses the build stops in AdDisplay.h, not in the test.
Which pairs are accepted is worked out here, from the header's rules as its comments state them,
and held against what the compiler says.

  for every wave interval W from 5 to 101 ms (6 to 100 are in range):
    the steps 5 and 251 (out of range), 6, 35 (the sign's), 97, 149, 249, 250,
    and around every place where the number of frames per column changes or the step is a
    multiple: W/2, W/2 + 1, W - 1, W, W + 1, 3W/2 - 1, 3W/2, 3W/2 + 1, 2W - 1, 2W, 2W + 1, 5W/2,
    3W, 4W + 1

usage: test/ad_display_grid.py [--jobs N] [--waves A-B]      (from anywhere; about 1900 builds)
Prints one line per pair that is not as worked out, then a summary; exit status 1 if there was one.
A command line it cannot run as written (an option it does not know, a number that is none, a
range from a larger to a smaller interval or outside 1 to 1000 ms, an option given twice,
anything that would test no pair) is exit status 2 and a line that says why: a run that tested nothing never looks like a pass.
Python 3 standard library and clang++ only.
"""
import concurrent.futures
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TEST = os.path.join(HERE, "ad_display_test.cpp")


def accepted(step, wave):
    """What AdDisplay.h lets through: both in range, at least one frame per scrolled column, and
    the frames of a column at least 6 ms apart (the clock counts 250 a millisecond)."""
    if not (6 <= wave <= 100 and 6 <= step <= 250):
        return False
    frames = (step + wave // 2) // wave
    return frames >= 1 and step * 250 // frames >= 6 * 250


def steps_for(wave):
    w = wave
    wanted = {5, 251, 6, 35, 97, 149, 249, 250, w // 2, w // 2 + 1, w - 1, w, w + 1, 3 * w // 2 - 1, 3 * w // 2,
              3 * w // 2 + 1, 2 * w - 1, 2 * w, 2 * w + 1, 5 * w // 2, 3 * w, 4 * w + 1}
    return sorted(s for s in wanted if 1 <= s <= 260)


def one(pair):
    step, wave, out = pair
    exe = os.path.join(out, "t_%d_%d" % (step, wave))
    c = subprocess.run(["clang++", "-std=c++11", "-O1", "-DAD_SCROLL_STEP_MS=%d" % step,
                        "-DAD_WAVE_FRAME_MS=%d" % wave, "-o", exe, TEST], capture_output=True, text=True)
    want = accepted(step, wave)
    if c.returncode:
        errors = [l for l in c.stderr.splitlines() if ": error:" in l]      # the compiler's own lines
        by_header = bool(errors) and all("AdDisplay.h:" in l.split(": error:")[0] and "static assertion" in l
                                         for l in errors)
        if want:
            return step, wave, "accepted by the header's rules, but does not build: %s" % (errors[0][-200:] if errors else "?")
        if not by_header:
            return step, wave, "refused, but not by AdDisplay.h alone: %s" % " | ".join(l[-120:] for l in errors[:3])
        return step, wave, None
    if not want:
        return step, wave, "refused by the header's rules, but it builds"
    r = subprocess.run([exe], capture_output=True, text=True)
    os.remove(exe)
    if r.returncode:
        last = (r.stdout + r.stderr).strip().splitlines()[-2:]
        return step, wave, "accepted, and the test fails: %s" % " | ".join(l[-170:] for l in last)
    return step, wave, None


def refuse(why):
    sys.stderr.write("ad_display_grid: %s\n"
                     "usage: test/ad_display_grid.py [--jobs N] [--waves A-B]   (nothing was tested)\n" % why)
    sys.exit(2)


def number(option, text):
    if not (text.isascii() and text.isdigit()):      # (the digits 0 to 9 only: no sign, no blank, not empty)
        refuse("%s wants a whole number, not %r" % (option, text))
    return int(text)


def arguments(args):
    """(jobs, first wave interval, last wave interval), or refuse()."""
    jobs, lo, hi = 1, 5, 101
    args, given = list(args), set()
    while args:
        a = args.pop(0)
        if a not in ("--jobs", "--waves"):
            refuse("%r is not an option of this tool" % a)
        if a in given:
            refuse("%s is given twice: which of the two is meant?" % a)
        given.add(a)
        if not args:
            refuse("%s wants a value after it" % a)
        value = args.pop(0)
        if a == "--jobs":
            jobs = number(a, value)
            if jobs < 1:
                refuse("--jobs %d: at least one build at a time" % jobs)
        else:
            ends = value.split("-")
            if len(ends) != 2:
                refuse("--waves wants two wave intervals in ms as A-B, not %r" % value)
            lo, hi = number(a, ends[0]), number(a, ends[1])
            if lo > hi:
                refuse("--waves %d-%d runs from the larger interval to the smaller: no pair would be tested" % (lo, hi))
            if lo < 1 or hi > 1000:
                refuse("--waves %d-%d: wave intervals are 1 to 1000 ms here (the header accepts 6 to 100)" % (lo, hi))
    return jobs, lo, hi


def main():
    jobs, lo, hi = arguments(sys.argv[1:])
    with tempfile.TemporaryDirectory() as out:
        pairs = [(s, w, out) for w in range(lo, hi + 1) for s in steps_for(w)]
        if not pairs:
            refuse("waves %d to %d ms give no pair of step and wave to test" % (lo, hi))
        bad = []
        ran = refused = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for step, wave, problem in pool.map(one, pairs):
                if problem:
                    bad.append((step, wave, problem))
                    print("step %3d ms, wave %3d ms: %s" % (step, wave, problem), flush=True)
                elif accepted(step, wave):
                    ran += 1
                else:
                    refused += 1
    print("ad_display_grid: %d pairs of step and wave (waves %d to %d ms): %d accepted by the header, built and "
          "passed; %d refused by the header, as worked out; %d not as worked out"
          % (len(pairs), lo, hi, ran, refused, len(bad)))
    if ran + refused + len(bad) != len(pairs):
        print("ad_display_grid: %d pairs were not run" % (len(pairs) - ran - refused - len(bad)))
        sys.exit(1)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
