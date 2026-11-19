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

    # Which file is which is decided by the opened file (its device and inode), never by a
    # path: no output may be the firmware, and no two outputs one file, under whatever names.
    # Every output is opened without being emptied, and nothing is emptied before all are known.
    import shutil
    ident = os.path.join(work, "ident")
    shutil.rmtree(ident, ignore_errors=True)
    os.makedirs(ident)
    fw = os.path.join(ident, "fw.hex")
    shutil.copy(hexfile, fw)
    fw_bytes = open(hexfile, "rb").read()

    def at(*names):
        return os.path.join(ident, *names)

    def firmware_whole():
        return open(fw, "rb").read() == fw_bytes

    def refused(status, err):
        return status == 2 and "same file" in err and "nothing was written" in err

    status, err = run("--duration", "0.01", "--frames", fw, fw)
    check(refused(status, err) and firmware_whole(),
          "--frames given the firmware file: refused with exit status 2, the firmware untouched")
    os.symlink(os.path.abspath(fw), at("link-to-fw.hex"))
    status, err = run("--duration", "0.01", "--record", at("link-to-fw.hex"), fw)
    check(refused(status, err) and firmware_whole(),
          "--record given a link to the firmware file: refused, the firmware untouched")
    os.link(fw, at("hard-link-to-fw.hex"))
    status, err = run("--duration", "0.01", "--frames", at("hard-link-to-fw.hex"), fw)
    check(refused(status, err) and firmware_whole(),
          "--frames given a second name (a hard link) of the firmware file: refused, untouched")
    status, err = run("--duration", "0.01", "--record", at("one.out"), "--frames",
                      at(".", "one.out"), fw)
    check(refused(status, err) and not os.path.exists(at("one.out")),
          "--record and --frames on one path (spelled two ways): refused, nothing written")

    # The gate's four.
    status, err = run("--duration", "0.01", "--png-dir", at("out"), "--frames",
                      at("out", "index.tsv"), fw)
    check(refused(status, err) and not os.path.exists(at("out")),
          "--png-dir out with --frames out/index.tsv (the file the picture recorder makes in "
          "its directory): refused, and neither the file nor the directory is left behind "
          "(status %d: %s)" % (status, err.strip()[:120]))
    os.makedirs(at("out2"))
    os.symlink(os.path.abspath(fw), at("out2", "index.tsv"))
    with open(at("old.gif"), "wb") as f:
        f.write(b"OLD")
    status, err = run("--duration", "0.01", "--record", at("old.gif"), "--png-dir", at("out2"), fw)
    check(refused(status, err) and firmware_whole() and os.path.islink(at("out2", "index.tsv")),
          "--png-dir whose index.tsv is a link to the firmware: refused, the firmware untouched "
          "(status %d: %s)" % (status, err.strip()[:120]))
    check(open(at("old.gif"), "rb").read() == b"OLD",
          "and the --record file of that command, which was there before and is no part of the "
          "collision, was not emptied: nothing is emptied before every output is known")
    with open(at("Probe"), "w"):
        pass
    if os.path.exists(at("probe")):
        status, err = run("--duration", "0.01", "--record", at("Run.out"), "--frames",
                          at("run.out"), fw)
        check(refused(status, err) and not os.path.exists(at("Run.out")),
              "--record Run.out --frames run.out on this volume, which ignores case: one file, "
              "refused, nothing written (status %d: %s)" % (status, err.strip()[:120]))
    else:
        status, err = run("--duration", "0.01", "--record", at("Run.out"), "--frames",
                          at("run.out"), fw)
        check(status == 0 and os.path.exists(at("Run.out")) and os.path.exists(at("run.out")),
              "--record Run.out --frames run.out on this volume, which keeps case apart: two "
              "files, both written")
    os.symlink("target.gif", at("dangling"))             # a link to a file that is not there
    status, err = run("--duration", "0.01", "--record", at("target.gif"), "--frames",
                      at("dangling"), fw)
    check(refused(status, err) and not os.path.exists(at("target.gif"))
          and os.path.islink(at("dangling")),
          "--frames given a dangling link to the --record file: refused, nothing written "
          "(status %d: %s)" % (status, err.strip()[:120]))
    os.symlink("target2.jsonl", at("dangling2"))
    status, err = run("--duration", "0.01", "--record", at("dangling2"), "--frames",
                      at("target2.jsonl"), fw)
    check(refused(status, err) and not os.path.exists(at("target2.jsonl"))
          and os.path.islink(at("dangling2")),
          "the same with the link first (the file is created through it): refused, and the "
          "file it made is removed again (status %d: %s)" % (status, err.strip()[:120]))

    # The pictures the PNG recorder makes are its own files in its own directory. Each is
    # checked as it is made: never written through a link, never onto a file that has a second
    # name, never onto a file the emulator holds.
    os.makedirs(at("out3"))
    os.symlink(os.path.abspath(fw), at("out3", "t00000.000.png"))
    status, err = run("--duration", "0.05", "--png-dir", at("out3"), fw)
    check(status == 4 and "is a link" in err and firmware_whole(),
          "--png-dir where the first picture's name is a link to the firmware: the run stops "
          "with exit status 4 and the firmware is untouched (status %d: %s)"
          % (status, [l for l in err.splitlines() if "RECORDING" in l][:1]))
    os.makedirs(at("out3h"))
    os.link(fw, at("out3h", "t00000.000.png"))
    status, err = run("--duration", "0.05", "--png-dir", at("out3h"), fw)
    check(status == 4 and "same file" in err and firmware_whole(),
          "the same with a second name (a hard link) of the firmware: exit status 4, the "
          "firmware untouched (status %d: %s)"
          % (status, [l for l in err.splitlines() if "RECORDING" in l][:1]))
    os.makedirs(at("out4"))
    with open(at("four.jsonl"), "w"):
        pass
    os.link(at("four.jsonl"), at("out4", "t00000.000.png"))
    status, err = run("--duration", "0.05", "--png-dir", at("out4"), "--frames", at("four.jsonl"), fw)
    try:
        first = open(at("four.jsonl"), encoding="utf-8").readline()
    except (OSError, UnicodeDecodeError):
        first = ""
    check(status == 4 and "same file" in err and first.startswith('{"type":"meta"'),
          "--png-dir where the first picture's name is a second name of the --frames file: the "
          "run stops with exit status 4 and the frames dump is still a frames dump")
    os.makedirs(at("out5"))
    os.symlink("../elsewhere.bin", at("out5", "t00000.000.png"))
    status, err = run("--duration", "0.05", "--png-dir", at("out5"), fw)
    check(status == 4 and "is a link" in err and not os.path.exists(at("elsewhere.bin"))
          and os.path.islink(at("out5", "t00000.000.png")),
          "--png-dir where the first picture's name is a link to a file that is not there: no "
          "picture is made outside the directory; exit status 4 (status %d)" % status)

    # The run's report goes to standard error. An output that is that file is refused too.
    with open(at("report.jsonl"), "wb") as report:
        p = subprocess.run([emu, "--headless", "--port", "0", "--duration", "0.01", "--frames",
                            at("report.jsonl"), fw], stdout=subprocess.PIPE, stderr=report, timeout=60)
    said = open(at("report.jsonl"), encoding="utf-8", errors="replace").read()
    check(p.returncode == 2 and "same file" in said and '"type":"meta"' not in said,
          "--frames given the file that standard error goes to: refused with exit status 2, "
          "no dump written into the report (status %d)" % p.returncode)
    status, err = run("--duration", "0.01", "--record", "/dev/null", "--frames", "/dev/null", fw)
    check(status == 0, "two outputs on /dev/null are no collision: nothing there to mix up "
                       "(status %d)" % status)

    # Nothing is emptied before the run begins: an output that was there keeps its contents
    # when the run cannot start for another reason (here: the TCP port is taken).
    import socket
    taken = socket.socket()
    taken.bind(("127.0.0.1", 0))
    taken.listen(1)
    with open(at("kept.jsonl"), "wb") as f:
        f.write(b"KEPT")
    p = subprocess.run([emu, "--headless", "--port", str(taken.getsockname()[1]), "--duration", "0.01",
                        "--frames", at("kept.jsonl"), "--record", at("never.gif"), fw],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    taken.close()
    check(p.returncode == 1 and b"cannot listen" in p.stderr
          and open(at("kept.jsonl"), "rb").read() == b"KEPT" and not os.path.exists(at("never.gif")),
          "the TCP port is taken: exit status 1, the --frames file that was there keeps its "
          "contents, and the --record file that was not there is not left behind (status %d)"
          % p.returncode)

    # And what is no collision works as before.
    with open(at("two.jsonl"), "wb") as f:
        f.write(b"x" * 300000)                        # longer than the dump that replaces it
    status, err = run("--duration", "0.01", "--record", at("two.gif"), "--frames",
                      at("two.jsonl"), "--png-dir", at("two"), fw)
    lines = open(at("two.jsonl"), "rb").read().splitlines()
    check(status == 0 and firmware_whole() and lines[0].startswith(b'{"type":"meta"')
          and lines[-1].startswith(b'{"type":"end"') and os.path.exists(at("two", "index.tsv")),
          "three different outputs are written (status %d), and a file that was there before "
          "is replaced whole: nothing of its old contents is left behind the new" % status)

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
