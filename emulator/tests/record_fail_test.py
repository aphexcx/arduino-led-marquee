#!/usr/bin/env python3
"""Recording failures must stop the run, be reported once, and exit nonzero.

Each case makes one output impossible to write, in a different way and at a
different moment (creating it, writing to it, closing it), and checks that
sign-emu says which path failed and why, exits with status 4, and leaves no
picture listed in index.tsv that was not written whole.

Failures during writing and closing are provoked with a file size limit
(RLIMIT_FSIZE) on the sign-emu process: macOS has no /dev/full. With the limit
at 0 and an output small enough to sit in the C library's buffer, the first
real write is the one fclose() makes, so that case fails at the close.

usage: record_fail_test.py <sign-emu> <firmware.hex> <work dir>
Python 3 standard library only.
"""
import os
import resource
import shutil
import struct
import subprocess
import sys
import zlib

import gif_check

failures = []


def check(ok, text):
    print("  %s  %s" % ("ok  " if ok else "FAIL", text))
    if not ok:
        failures.append(text)


def run(emu, hexfile, args, limit=None):
    def set_limit():
        resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))
    p = subprocess.run([emu, "--headless", "--port", "0", *args, hexfile],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
                       preexec_fn=set_limit if limit is not None else None)
    return p.returncode, p.stderr.decode("utf-8", "replace")


def reports(stderr):
    return [l for l in stderr.splitlines() if "recording failed" in l.lower()]


def png_is_whole(path):
    """A PNG with good chunk checksums whose picture data is all there."""
    try:
        data = open(path, "rb").read()
    except OSError:
        return False
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return False
    pos, size, packed, ended = 8, None, b"", False
    while pos + 12 <= len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if len(body) != n or pos + 12 + n > len(data):
            return False
        if struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0] != zlib.crc32(kind + body):
            return False
        if kind == b"IHDR":
            size = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            packed += body
        elif kind == b"IEND":
            ended = True
        pos += 12 + n
    if not ended or size is None or pos != len(data):
        return False
    try:
        return len(zlib.decompress(packed)) == (size[0] * 3 + 1) * size[1]
    except zlib.error:
        return False


def index_entries(directory):
    """The pictures index.tsv lists, or None if there is no readable index with its header."""
    try:
        lines = open(os.path.join(directory, "index.tsv")).read().splitlines()
    except OSError:
        return None
    if not lines or not lines[0].startswith("file\t"):
        return None
    return [l.split("\t")[0] for l in lines[1:] if l]


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    emu, hexfile, work = sys.argv[1], sys.argv[2], sys.argv[3]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    def expect_failure(what, status, stderr, path, reason=None):
        said = reports(stderr)
        ok = (status == 4 and len(said) == 1 and path in said[0]
              and (reason is None or reason in said[0]))
        check(ok, "%s: exit status 4 and one report naming the path%s (status %d; %s)"
              % (what, " and the reason" if reason else "", status,
                 said[0].strip() if len(said) == 1 else "%d reports" % len(said)))

    print("an output that cannot be created")
    # The gate's case: the first picture's name is taken by a directory.
    d = os.path.join(work, "png-blocked")
    os.makedirs(os.path.join(d, "t00000.000.png"))
    status, err = run(emu, hexfile, ["--duration", "0.05", "--png-dir", d])
    expect_failure("--png-dir where t00000.000.png is a directory", status, err,
                   os.path.join(d, "t00000.000.png"))
    listed = index_entries(d)
    check(listed == [], "index.tsv is there, with its header, and lists no picture (%s)"
          % ("no index" if listed is None else listed or "none listed"))

    missing = os.path.join(work, "no-such-directory")
    status, err = run(emu, hexfile, ["--duration", "0.05", "--record", os.path.join(missing, "a.gif")])
    expect_failure("--record into a directory that does not exist", status, err,
                   os.path.join(missing, "a.gif"), "No such file or directory")
    status, err = run(emu, hexfile, ["--duration", "0.05", "--record", work])
    expect_failure("--record onto a directory", status, err, work, "Is a directory")
    status, err = run(emu, hexfile, ["--duration", "0.05", "--frames", os.path.join(missing, "a.jsonl")])
    expect_failure("--frames into a directory that does not exist", status, err,
                   os.path.join(missing, "a.jsonl"), "No such file or directory")
    plain = os.path.join(work, "plain-file")
    open(plain, "w").close()
    status, err = run(emu, hexfile, ["--duration", "0.05", "--png-dir", plain])
    expect_failure("--png-dir onto a plain file", status, err, plain)

    print("an output that fails while it is being written (file size limit)")
    path = os.path.join(work, "limited.jsonl")
    status, err = run(emu, hexfile, ["--duration", "3", "--frames", path], limit=100000)
    expect_failure("--frames with a 100000-byte limit", status, err, path, "File too large")
    check("simulated 3.000 s" not in err, "the run stopped there instead of carrying on to its end")
    path = os.path.join(work, "limited.gif")
    status, err = run(emu, hexfile, ["--duration", "3", "--record", path], limit=20000)
    expect_failure("--record with a 20000-byte limit", status, err, path, "File too large")
    # The startup's first pictures (dark, then "I") are under 600 bytes as PNGs;
    # "LIVE" is the first one over.
    d = os.path.join(work, "png-limited")
    status, err = run(emu, hexfile, ["--duration", "3", "--tick-cs", "10", "--png-dir", d], limit=600)
    expect_failure("--png-dir with a 600-byte limit on each file", status, err, d + "/t")
    listed = index_entries(d) or []
    present = sorted(f for f in os.listdir(d) if f.endswith(".png"))
    check(len(listed) >= 5 and all(png_is_whole(os.path.join(d, f)) for f in listed)
          and present == sorted(listed),
          "the pictures written before the failure are whole and listed in index.tsv; the one "
          "that failed is neither listed nor left behind (%d listed, %d files)"
          % (len(listed), len(present)))

    print("an output that fails when it is closed (limit 0, output still in the buffer)")
    path = os.path.join(work, "close.jsonl")
    status, err = run(emu, hexfile, ["--duration", "0.0001", "--frames", path], limit=0)
    expect_failure("--frames", status, err, path, "close failed")
    path = os.path.join(work, "close.gif")
    status, err = run(emu, hexfile, ["--duration", "0.0001", "--record", path], limit=0)
    expect_failure("--record", status, err, path, "close failed")

    print("and when nothing fails")
    d = os.path.join(work, "png-good")
    gif, dump = os.path.join(work, "good.gif"), os.path.join(work, "good.jsonl")
    status, err = run(emu, hexfile, ["--duration", "1.5", "--png-dir", d, "--record", gif,
                                     "--frames", dump])
    listed = index_entries(d) or []
    check(status == 0 and not reports(err) and len(listed) > 5
          and all(png_is_whole(os.path.join(d, f)) for f in listed),
          "exit status 0, no report, %d whole pictures listed" % len(listed))
    try:
        n = gif_check.compare_pngs(d, dump)
        check(True, "the %d PNG pictures and their times in index.tsv are what the frames "
                    "dump gives, pixel for pixel" % n)
        n, seconds = gif_check.compare(gif, dump)
        check(True, "so is the GIF of the same run (%d frames, %.2f s)" % (n, seconds))
    except gif_check.GifError as e:
        check(False, "the pictures of the same run match its frames dump: %s" % e)

    if failures:
        print("FAILED: %d recording failure check(s)" % len(failures))
        sys.exit(1)
    print("recording failures: all checks passed")


if __name__ == "__main__":
    main()
