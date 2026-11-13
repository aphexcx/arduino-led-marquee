#!/usr/bin/env python3
"""The command line: values that are not numbers, or are outside what the
emulator's arithmetic and file formats can hold, are refused before anything
runs (exit status 2, one line naming the option), and a firmware file that
cannot be loaded is an error (exit status 1).

usage: cli_test.py <sign-emu> <firmware.hex> <work dir>
Python 3 standard library only.
"""
import json
import os
import subprocess
import sys

import gif_check

failures = []


def check(ok, text):
    print("  %s  %s" % ("ok  " if ok else "FAIL", text))
    if not ok:
        failures.append(text)


def main():
    emu, hexfile, work = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(work, exist_ok=True)

    def run(*args):
        p = subprocess.run([emu, "--headless", "--port", "0", *args], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=60)
        return p.returncode, p.stderr.decode("utf-8", "replace")

    refused = [
        ("--duration", "abc"), ("--duration", "-1"), ("--duration", "1e300"), ("--duration", "nan"),
        ("--until-requests", "0"), ("--until-requests", "1.5"),
        ("--scale", "1"), ("--scale", "100000"), ("--tick-cs", "0"), ("--tick-cs", "70000"),
        ("--gamma", "0"), ("--port", "70000"), ("--port", "-1"), ("--speed", "-2"),
        ("--link-latency-ms", "-5"), ("--link-wait-ms", "1e12"), ("--wait-client", "x"),
        ("--bpm", "0"), ("--bpm", "99999"), ("--reset-us", "0"), ("--reset-us", "1e12"),
    ]
    bad = []
    for option, value in refused:
        status, err = run(option, value, "--duration", "0.01", hexfile)
        first = err.splitlines()[0] if err else ""
        if status != 2 or option not in first or "expected" not in first:
            bad.append("%s %s (status %d: %s)" % (option, value, status, first))
    check(not bad, "%d values that are not numbers or are out of range are each refused with "
                   "exit status 2 and a line naming the option%s"
          % (len(refused), "" if not bad else "; not refused: " + "; ".join(bad)))

    status, err = run("--duration")
    check(status == 2 and "needs a value" in err, "an option without its value is refused")
    status, err = run("--no-such-option", hexfile)
    check(status == 2 and "unknown option" in err, "an unknown option is refused")
    status, err = run()
    check(status == 2 and "usage:" in err, "no firmware file: the usage text and exit status 2")

    status, err = run("--duration", "0.01", os.path.join(work, "missing.hex"))
    check(status == 1 and "cannot read" in err, "a firmware file that does not exist: exit status 1")
    garbage = os.path.join(work, "garbage.hex")
    with open(garbage, "w") as f:
        f.write(":02000004FFFFFC\n:01FFFF000001\n:00000001FF\n")
    status, err = run("--duration", "0.01", garbage)
    check(status == 1 and "cannot read" in err,
          "a firmware file that places data outside the flash: exit status 1")

    # The largest values together. The latch time of a whole second means no
    # frame latches in this short run; the GIF must still be the right size
    # (64 pixels an LED) and one tick long, and match the frames dump.
    gif, dump = os.path.join(work, "edge.gif"), os.path.join(work, "edge.jsonl")
    status, err = run("--duration", "0.3", "--scale", "64", "--tick-cs", "6000", "--bpm", "600",
                      "--reset-us", "1000000", "--record", gif, "--frames", dump, hexfile)
    try:
        n, seconds = gif_check.compare(gif, dump)
        width, height, _ = gif_check.read_gif(gif)
        check(status == 0 and (width, height, seconds) == (62 * 64, 17 * 64, 60.0),
              "the largest scale, tick, beat rate and latch time together: a %d x %d GIF of "
              "%.0f s that matches its frames dump (status %d)" % (width, height, seconds, status))
    except (gif_check.GifError, OSError) as e:
        check(False, "the largest scale and tick together record a GIF that matches its dump: %s" % e)
    gif, dump = os.path.join(work, "small.gif"), os.path.join(work, "small.jsonl")
    status, err = run("--duration", "1.2", "--scale", "2", "--tick-cs", "1", "--band",
                      "--record", gif, "--frames", dump, hexfile)
    try:
        n, seconds = gif_check.compare(gif, dump)
        check(status == 0 and n > 20, "the smallest scale and tick, band layout: %d frames, "
                                       "%.2f s, matching the frames dump" % (n, seconds))
    except (gif_check.GifError, OSError) as e:
        check(False, "the smallest scale and tick record a GIF that matches its dump: %s" % e)

    # The dump says how its pictures would be drawn even when no picture is recorded.
    dump = os.path.join(work, "only-frames.jsonl")
    status, err = run("--duration", "0.2", "--scale", "10", "--tick-cs", "5", "--band",
                      "--link-latency-ms", "1234.5678", "--frames", dump, hexfile)
    meta = json.loads(open(dump, encoding="utf-8").readline())
    check(status == 0 and meta["render"] == {"scale": 10, "tick_cs": 5, "band": True, "gamma": 2.2}
          and meta["link_latency_ms"] == 1234.5678,
          "with only --frames, the first line still carries the scale, tick and layout asked "
          "for, and settings in full precision (%s, latency %r)"
          % (meta.get("render"), meta.get("link_latency_ms")))

    if failures:
        print("FAILED: %d command line check(s)" % len(failures))
        sys.exit(1)
    print("command line: all checks passed")


if __name__ == "__main__":
    main()
