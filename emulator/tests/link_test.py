#!/usr/bin/env python3
"""Tests of the serial link's timing model, against the real firmware.

A scripted peer plays the app. Each case runs sign-emu headless with a frames
dump and checks the dump: what the firmware saw must depend only on what the
peer sent, not on how the peer's writes were cut up or how long this computer
took between them.

usage: link_test.py <sign-emu> <firmware.hex> <work dir> [port]
Python 3 standard library only.
"""
import json
import os
import socket
import subprocess
import sys
import time

BIT_S = 1.0 / 14400
CYCLE_S = 1.0 / 16000000
failures = []


def check(ok, text):
    print("  %s  %s" % ("ok  " if ok else "FAIL", text))
    if not ok:
        failures.append(text)


class Run:
    def __init__(self, lines, wall_s, log):
        self.lines = lines                     # the dump, without its meta line
        self.wall_s = wall_s
        self.log = log
        events = [json.loads(l) for l in lines]
        self.tx = [e for e in events if e["type"] == "tx"]
        self.rx = [e for e in events if e["type"] == "rx"]
        self.frames = sum(1 for e in events if e["type"] == "frame")

    def rx_text(self):
        return bytes(e["byte"] for e in self.rx).decode("latin-1")

    def tx_text(self):
        return bytes(e["byte"] for e in self.tx).decode("latin-1")


def run_case(env, name, plan, requests, flags=()):
    """plan: for each request, a list of (bytes, pause before sending, in real seconds)."""
    dump = os.path.join(env["work"], name + ".jsonl")
    log = os.path.join(env["work"], name + ".log")
    with open(log, "w") as err:
        emu = subprocess.Popen(
            [env["emu"], "--headless", "--port", str(env["port"]), "--wait-client", "30",
             "--until-requests", str(requests), "--duration", "120", "--frames", dump,
             *flags, env["hex"]], stderr=err)
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", env["port"]), timeout=2)
                break
            except OSError:
                if time.monotonic() > deadline or emu.poll() is not None:
                    raise
                time.sleep(0.05)
        start = time.monotonic()
        sock.settimeout(60)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        seen = 0
        while True:
            data = sock.recv(4096)
            if not data:
                break
            for byte in data:
                if byte not in b"~?":
                    continue
                if seen < len(plan):
                    for chunk, pause in plan[seen]:
                        if pause:
                            time.sleep(pause)
                        sock.sendall(chunk)
                seen += 1
        sock.close()
        emu.wait(timeout=120)
        wall = time.monotonic() - start
    finally:
        if emu.poll() is None:
            emu.kill()
    lines = open(dump, encoding="utf-8").read().splitlines()
    return Run(lines[1:], wall, open(log).read())


def split(reply, *cuts, pause=0.3):
    """reply cut at the given byte offsets, with a real-time pause before each later piece."""
    edges = [0, *cuts, len(reply)]
    return [(reply[a:b], pause if i else 0) for i, (a, b) in enumerate(zip(edges, edges[1:]))]


def first_difference(a, b):
    for i, (x, y) in enumerate(zip(a.lines, b.lines)):
        if x != y:
            return "line %d: %s  |  %s" % (i + 2, x[:90], y[:90])
    return "%d lines against %d" % (len(a.lines), len(b.lines))


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    env = {"emu": sys.argv[1], "hex": sys.argv[2], "work": sys.argv[3],
           "port": int(sys.argv[4]) if len(sys.argv) > 4 else 14418}
    os.makedirs(env["work"], exist_ok=True)
    only = os.environ.get("LINK_TEST_ONLY", "")

    hello = b'{"type":"F","str":"HELLO"}'
    stars = b'{"type":"S","str":"","stars":5}'

    print("split replies behave exactly like single-write replies")
    whole = run_case(env, "whole", [[(hello, 0)], [(stars, 0)]], 3)
    check(whole.tx_text() == "~~~" and whole.rx_text() == (hello + stars).decode(),
          "single writes: the sign asked three times and parsed both replies "
          "(sent %r, %d frames, %.1f s real)" % (whole.tx_text(), whole.frames, whole.wall_s))
    cases = [
        ("after the first byte", [split(hello, 1), split(stars, 1)], ()),
        ("in the middle", [split(hello, 13), split(stars, 16)], ()),
        ("in three pieces", [split(hello, 1, 20, pause=0.2), split(stars, 5, 6, pause=0.2)], ()),
        ("whole, but 0.6 s late in real time", [[(hello, 0.6)], [(stars, 0.6)]], ()),
        # The gate's sequence: --speed 1 --link-wait-ms 2000, "{" at once, the rest 1.5 s later.
        ("the gate's sequence (--speed 1 --link-wait-ms 2000, the rest 1.5 s later)",
         [split(hello, 1, pause=1.5), [(stars, 0)]], ("--speed", "1", "--link-wait-ms", "2000")),
    ]
    for i, (what, plan, flags) in enumerate(cases):
        if only and only not in what:
            continue
        run = run_case(env, "split%d" % i, plan, 3, flags)
        same = run.lines == whole.lines
        check(same, "reply cut %s: the dump is identical to the single-write run%s"
              % (what, "" if same else " (sign sent %r; first difference at %s)"
                 % (run.tx_text()[:40], first_difference(whole, run))))

    if only:
        sys.exit(1 if failures else 0)

    # sign-emu's arithmetic: a request ends 10 bit times after its start bit
    # began; the reply's start bit begins a whole number of cycles after that.
    request_cycles = int(10 * 16000000.0 / 14400)

    def offset(run, reply_index_in_rx, request_index):
        return run.rx[reply_index_in_rx]["cycle"] - run.tx[request_index]["cycle"]

    print("where a reply is placed in simulated time")
    want = request_cycles + 5 * 16000
    got = [offset(whole, 0, 0), offset(whole, len(hello), 1)]
    check(all(abs(g - want) <= 1 for g in got),
          "default: a reply's start bit begins 5 ms after its request's stop bit ends "
          "(%s cycles after the request's start bit, expected %d)" % (got, want))
    run = run_case(env, "latency40", [[(hello, 0)], [(stars, 0)]], 3, ("--link-latency-ms", "40"))
    want = request_cycles + 40 * 16000
    got = [offset(run, 0, 0), offset(run, len(hello), 1)]
    check(all(abs(g - want) <= 1 for g in got) and run.tx_text() == "~~~",
          "--link-latency-ms 40: 40 ms (%s cycles, expected %d)" % (got, want))
    gaps = {b["cycle"] - a["cycle"] for a, b in zip(whole.rx, whole.rx[1:len(hello)])}
    check(gaps <= {request_cycles, request_cycles + 1},
          "a reply's bytes follow each other back to back, 10 bit times apart (%s cycles)"
          % sorted(gaps))

    print("directives (--link-directives)")
    d = ("--link-directives",)
    run = run_case(env, "delay250", [[(b"@250\n" + hello, 0)], [(stars, 0)]], 3, d)
    want = request_cycles + 250 * 16000
    check(abs(offset(run, 0, 0) - want) <= 1 and run.tx_text() == "~~~"
          and run.rx_text() == (hello + stars).decode(),
          '"@250\\n" before a reply: it starts 250 ms after its request and the directive is '
          "not put on the wire (%d cycles, expected %d)" % (offset(run, 0, 0), want))
    cut = run_case(env, "delay250cut",
                   [[(b"@2", 0), (b"50\n{", 0.2), (hello[1:], 0.2)], [(stars, 0)]], 3, d)
    check(cut.lines == run.lines, "the same with directive and reply cut into three writes: "
                                  "identical dump")
    none = run_case(env, "none", [[(b"@none\n", 0)]], 2, d)
    check(not none.rx and none.tx_text() == "~~" and "NOTE" not in none.log,
          '"@none\\n": the request gets no reply and the clock is not held for one '
          "(next request %.3f s later)" % (none.tx[1]["t"] - none.tx[0]["t"]))
    raw = run_case(env, "nodirectives", [[(b"@250\n" + hello, 0)]], 2)
    check(raw.rx_text().startswith("@250\n{") and "InvalidInput" in raw.tx_text(),
          "without --link-directives the same bytes go on the wire as they are "
          "(the sign answered %r)" % raw.tx_text()[:16])

    print("late and queued replies")
    # With no reply the firmware gives up after its one-second timeout, shows
    # stars, and asks again. Aim the late reply 20 ms after that second request.
    between = none.tx[1]["cycle"] - none.tx[0]["cycle"]
    late_ms = between / 16000.0 + 20
    plan = [[(b"@%.4f\n" % late_ms + hello, 0)], [(stars, 0)]]
    late = run_case(env, "late", plan, 3, d)
    first_late = offset(late, 0, 0)
    want = request_cycles + int(late_ms * 16000)
    check(abs(first_late - want) <= 1,
          "a reply delayed %.1f ms starts then, long after the firmware stopped waiting for it "
          "(%d cycles, expected %d)" % (late_ms, first_late, want))
    check(late.rx_text() == (hello + stars).decode(),
          "the reply to the second request is queued behind it: the wire carries the late "
          "reply, then the second")
    second = late.rx[len(hello)]["cycle"]
    late_end = late.rx[len(hello) - 1]["cycle"] + request_cycles
    due = late.tx[1]["cycle"] + request_cycles + 5 * 16000
    check(due < late_end and abs(second - late_end) <= 1,
          "the second reply was due %.2f ms before the late one finished, and starts as the "
          "late one's last stop bit ends (%d cycles apart)"
          % ((late_end - due) / 16000.0, second - late_end))
    # What the firmware made of it: the late HELLO was read as the answer to
    # the second request, so the third request comes one HELLO later, as in
    # the single-write run (where HELLO answered the first), 15 ms further on.
    shown = (late.tx[2]["cycle"] - late.tx[1]["cycle"]) - (whole.tx[1]["cycle"] - whole.tx[0]["cycle"])
    check(late.tx_text() == "~~~" and abs(shown - 15 * 16000) <= 16000,
          "the firmware reads the late reply as the answer to the second request and shows it "
          "(third request %.2f ms later than a HELLO takes in the single-write run; the reply "
          "came 15 ms later into its window)" % (shown / 16000.0))
    again = run_case(env, "late2", plan, 3, d)
    check(again.lines == late.lines, "the same run again gives an identical dump")
    early = run_case(env, "early", [[(b"@%.4f\n" % (late_ms - 100) + hello, 0)], [(stars, 0)]], 3, d)
    gap = early.rx[len(hello)]["cycle"] - early.tx[1]["cycle"]
    check(early.rx[0]["cycle"] < early.tx[1]["cycle"]
          and abs(gap - (request_cycles + 5 * 16000)) <= 1,
          "a late reply that is over before the second request does not move the second "
          "reply: it starts 5 ms after its own request (%d cycles)" % gap)

    check("InvalidInput" in early.tx_text(),
          "that reply arrived while the firmware showed stars after a timed-out read, still "
          "listening: it left damaged bytes in the receive buffer and the next read failed "
          "(the sign sent %r)" % early.tx_text())

    print("a newline after each reply")
    nl_same = run_case(env, "nl-same", [[(hello + b"\n", 0)], [(stars + b"\n", 0)]], 3)
    nl_later = run_case(env, "nl-later", [[(hello, 0), (b"\n", 0.3)], [(stars, 0), (b"\n", 0.3)]], 3)
    check(nl_same.tx_text() == "~~~" and nl_same.rx_text() == (hello + b"\n" + stars).decode(),
          "the newline travels in front of the next reply and the sign parses both")
    check(nl_same.lines == nl_later.lines,
          "sent with the reply or 0.3 s after it: identical dump")

    if failures:
        print("FAILED: %d link check(s)" % len(failures))
        sys.exit(1)
    print("link: all checks passed")


if __name__ == "__main__":
    main()
