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

from fractions import Fraction

BIT_CYCLES = Fraction(16000000, 14400)          # CPU cycles in one bit time
REQUEST_CYCLES = -(-10 * BIT_CYCLES // 1)       # 10 bit times, rounded up to a whole cycle
failures = []
# Several cases here send a reply in pieces with pauses of real time in between, and expect the
# same dump as without the pauses. That holds while the pieces arrive within the emulator's
# wait for the reply (--link-wait-ms, 1 s unless told otherwise). So that a busy computer
# cannot stretch a 0.6 s pause past it, every run gets a wait of 20 s; the cases that are
# about the wait running out name their own (a later option wins).
LONG_WAIT = ("--link-wait-ms", "20000")


def nearest_cycle(bits):
    """The whole cycle nearest to `bits` bit times."""
    return int(bits * BIT_CYCLES + Fraction(1, 2))


def check(ok, text):
    print("  %s  %s" % ("ok  " if ok else "FAIL", text))
    if not ok:
        failures.append(text)


class Run:
    def __init__(self, lines, wall_s, log, status, tolerate_wire=False):
        self.lines = lines                     # the dump, without its meta line
        self.wall_s = wall_s
        self.log = log
        if status != 0:
            raise RuntimeError("sign-emu exited with status %s: %s" % (status, log.strip()[-300:]))
        events = [json.loads(l) for l in lines]
        self.tx = [e for e in events if e["type"] == "tx"]
        # Each rx line is a byte a listener decoded from the pin: "cycle" is when
        # its start bit was seen to fall, "due" is when the model had it due.
        self.rx = [e for e in events if e["type"] == "rx"]
        # Each break line is the app holding the line LOW: "cycle" is when it was
        # seen to fall, "due" when it was due to, "end" when it is due to rise.
        self.breaks = [e for e in events if e["type"] == "break"]
        late = [e["cycle"] - e["due"] for e in self.rx]
        if late and not (0 <= min(late) and max(late) <= 4) and not tolerate_wire:
            raise RuntimeError("a start bit was seen on the pin %d..%d cycles after it was due; "
                               "it must be there at the first instruction boundary (0 to 4 "
                               "cycles)" % (min(late), max(late)))
        if "WARNING" in log and not tolerate_wire:
            raise RuntimeError("sign-emu warned: %s" % log.strip()[-300:])
        self.frames = sum(1 for e in events if e["type"] == "frame")

    def rx_text(self):
        return bytes(e["byte"] for e in self.rx).decode("latin-1")

    def tx_text(self):
        return bytes(e["byte"] for e in self.tx).decode("latin-1")


def run_case(env, name, plan, requests, flags=(), tolerate_wire=False):
    """plan: for each request, a list of (bytes, pause before sending, in real seconds)."""
    dump = os.path.join(env["work"], name + ".jsonl")
    log = os.path.join(env["work"], name + ".log")
    with open(log, "w") as err:
        emu = subprocess.Popen(
            [env["emu"], "--headless", "--port", str(env["port"]), "--wait-client", "30",
             "--until-requests", str(requests), "--duration", "120", "--frames", dump,
             *LONG_WAIT, *flags, env["hex"]], stderr=err)
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
        try:
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
        except (ConnectionResetError, BrokenPipeError):
            # The emulator reached its last request and closed the link while
            # this peer still had something to send. That is how a run ends,
            # not a failure: the exit status and the dump are checked below.
            pass
        sock.close()
        emu.wait(timeout=120)
        wall = time.monotonic() - start
    finally:
        if emu.poll() is None:
            emu.kill()
    lines = open(dump, encoding="utf-8").read().splitlines()
    return Run(lines[1:], wall, open(log).read(), emu.returncode, tolerate_wire)


class Session:
    """One sign-emu run that a test script drives step by step, with as many clients as it likes."""

    def __init__(self, env, name, requests, flags=(), port=None):
        self.port = port or env["port"]
        self.dump = os.path.join(env["work"], name + ".jsonl")
        self.log = os.path.join(env["work"], name + ".log")
        with open(self.log, "w") as err:
            self.emu = subprocess.Popen(
                [env["emu"], "--headless", "--port", str(self.port), "--wait-client", "30",
                 "--until-requests", str(requests), "--duration", "120", "--frames", self.dump,
                 *LONG_WAIT, *flags, env["hex"]], stderr=err)
        self.start = time.monotonic()

    def connect(self):
        deadline = time.monotonic() + 30
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", self.port), timeout=2)
                break
            except OSError:
                if time.monotonic() > deadline or self.emu.poll() is not None:
                    raise
                time.sleep(0.05)
        sock.settimeout(60)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock

    def request(self, sock):
        """Waits for the sign's next request byte on this client. False if the link closed."""
        while True:
            data = sock.recv(1)
            if not data:
                return False
            if data in (b"~", b"?"):
                return True

    def finish(self, *socks):
        for sock in socks:
            try:
                while sock.recv(4096):
                    pass
            except OSError:
                pass
            sock.close()
        try:
            self.emu.wait(timeout=120)
        finally:
            if self.emu.poll() is None:
                self.emu.kill()
        lines = open(self.dump, encoding="utf-8").read().splitlines()
        return Run(lines[1:], time.monotonic() - self.start, open(self.log).read(),
                   self.emu.returncode)


def rejected(run):
    """The sign did not take the first reply for a message. Firmware up to PR 4 says so on the
    wire and flashes it ("InvalidInput", "IncompleteInput"). The contract 3 firmware takes
    bytes that are not a whole message for the rest of something it did not hear the start
    of, lets them go by, and asks again: its second request then comes within a second and a
    half of the last of those bytes (the longest it waits for one more byte is a second).
    (A reply it took for a message keeps it busy for longer than that before it asks again:
    showing HELLO takes 2.8 s. main() checks that this says no for such a run.)"""
    if "InvalidInput" in run.tx_text() or "IncompleteInput" in run.tx_text():
        return True
    asks = [e for e in run.tx if e.get("char") in ("~", "?")]
    if len(asks) < 2:
        return False
    reply = [e for e in run.rx if e["cycle"] < asks[1]["cycle"]]
    return bool(reply) and asks[1]["cycle"] - reply[-1]["cycle"] < 24000000


def split(reply, *cuts, pause=0.3):
    """reply cut at the given byte offsets, with a real-time pause before each later piece."""
    edges = [0, *cuts, len(reply)]
    return [(reply[a:b], pause if i else 0) for i, (a, b) in enumerate(zip(edges, edges[1:]))]


def first_difference(a, b):
    for i, (x, y) in enumerate(zip(a.lines, b.lines)):
        if x != y:
            return "line %d: %s  |  %s" % (i + 2, x[:90], y[:90])
    return "%d lines against %d" % (len(a.lines), len(b.lines))


def reconnection_tests(env, hello, stars, request_cycles):
    """What one client leaves unfinished must not reach the next client."""
    from concurrent.futures import ThreadPoolExecutor

    # Client A is there from the boot. At the sign's first request it does
    # `first` (sends something unfinished, or nothing) and is then gone: it
    # closes, or client B connects and takes its place. B answers the second
    # request with HELLO and the third with stars. These runs are paced to real
    # time, so that B is connected seconds before the second request.
    def two_clients(name, port, first, replace):
        run = Session(env, name, 4, ("--link-directives", "--speed", "1"), port)
        a = run.connect()
        run.request(a)
        if first:
            a.sendall(first)
        time.sleep(0.3)                       # let the emulator read it
        if not replace:
            a.close()
        b = run.connect()
        run.request(b) and b.sendall(hello)
        run.request(b) and b.sendall(stars)
        return run.finish(b) if not replace else run.finish(b, a)

    # The same, with one client from start to end.
    def one_client(name, port, first):
        run = Session(env, name, 4, ("--link-directives",), port)
        b = run.connect()
        run.request(b) and b.sendall(first)
        run.request(b) and b.sendall(hello)
        run.request(b) and b.sendall(stars)
        return run.finish(b)

    delayed = b"@500\n" + hello
    jobs = [
        ("only-b", one_client, (b"@none\n",)),
        ("only-b-delayed", one_client, (delayed,)),
        ("gate", two_clients, (b"@250", False)),
        ("mid-json", two_clients, (hello[:15], False)),
        ("pending-delay", two_clients, (b"@250\n", False)),
        ("silent", two_clients, (b"", False)),
        ("replaced-directive", two_clients, (b"@250", True)),
        ("replaced-mid-json", two_clients, (hello[:15], True)),
        ("delayed-then-gone", two_clients, (delayed, False)),
    ]
    with ThreadPoolExecutor(len(jobs)) as pool:
        futures = {name: pool.submit(fn, "re-" + name, env["port"] + 2 + i, *args)
                   for i, (name, fn, args) in enumerate(jobs)}
        runs = {}
        for name, future in futures.items():
            try:
                runs[name] = future.result()
            except Exception as e:                # a run that hangs or dies is a failed check
                runs[name] = None
                check(False, "run %s completed (%s: %s)" % (name, type(e).__name__, e))

    print("a client that goes away takes its unfinished business with it")
    only_b = runs["only-b"]
    if only_b:
        check(only_b.tx_text() == "~~~~" and only_b.rx_text() == (hello + stars).decode(),
              "reference: one client that gives the first request no reply, then HELLO, then "
              "stars (the sign sent %r)" % only_b.tx_text())

    def same_as_only_b(name, what):
        run = runs[name]
        if not run or not only_b:
            return
        same = run.lines == only_b.lines
        check(same, "%s: the second client is served exactly as if it had been the only one "
                    "(identical dump)%s" % (what, "" if same else
                    "; the sign sent %r, the wire carried %r; first difference at %s"
                    % (run.tx_text()[:30], run.rx_text()[:30], first_difference(only_b, run))))
        check("NOTE" not in run.log,
              "%s: the clock was released when the client went, not at the end of "
              "--link-wait-ms" % what)

    same_as_only_b("gate", 'the gate\'s sequence, client A sends "@250" with no newline and closes')
    same_as_only_b("mid-json", "A sends half a JSON object and closes")
    same_as_only_b("pending-delay", 'A sends a whole directive "@250\\n", no reply, and closes')
    same_as_only_b("silent", "A sends nothing and closes while the clock is held for it")
    same_as_only_b("replaced-directive", 'A sends "@250" and stays; B connects and replaces it')
    same_as_only_b("replaced-mid-json", "A sends half a JSON object and stays; B replaces it")

    print("a whole reply is still sent after its client has gone")
    gone, kept = runs["delayed-then-gone"], runs["only-b-delayed"]
    if gone and kept:
        got = gone.rx[0]["due"] - gone.tx[0]["cycle"] if gone.rx else None
        want = request_cycles + 500 * 16000
        check(got == want,
              "A sends a reply delayed by 500 ms and closes at once: the reply goes on the wire "
              "at its moment (%s cycles after the request, expected %d)" % (got, want))
        same = gone.lines == kept.lines
        check(same, "and the run is identical to one client sending the same three replies%s"
              % ("" if same else " (first difference at %s)" % first_difference(kept, gone)))


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    env = {"emu": sys.argv[1], "hex": sys.argv[2], "work": sys.argv[3],
           "port": int(sys.argv[4]) if len(sys.argv) > 4 else 14418}
    os.makedirs(env["work"], exist_ok=True)
    only = os.environ.get("LINK_TEST_ONLY", "")

    hello = b'{"type":"F","str":"HELLO"}'
    stars = b'{"type":"S","str":"","stars":5}'

    if only == "reconnect":
        reconnection_tests(env, hello, stars, REQUEST_CYCLES)
        sys.exit(1 if failures else 0)

    print("split replies behave exactly like single-write replies")
    whole = run_case(env, "whole", [[(hello, 0)], [(stars, 0)]], 3)
    check(whole.tx_text() == "~~~" and whole.rx_text() == (hello + stars).decode(),
          "single writes: the sign asked three times and parsed both replies "
          "(sent %r, %d frames, %.1f s real)" % (whole.tx_text(), whole.frames, whole.wall_s))
    check(not rejected(whole),
          "(and by this file's own measure the sign did not reject the first: it asked again "
          "%.2f s after HELLO's last byte, having shown it)"
          % ((whole.tx[1]["cycle"] - whole.rx[len(hello) - 1]["cycle"]) / 16e6))
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

    # The expected cycle numbers come from two facts only, the 16 MHz clock and
    # 14400 baud, in exact fractions: a request ends 10 bit times after its
    # start bit began (11111 1/9 cycles); a reply may not start before that, so
    # the first whole cycle it can use is the next one.
    request_cycles = REQUEST_CYCLES

    def offset(run, reply_index_in_rx, request_index):
        return run.rx[reply_index_in_rx]["due"] - run.tx[request_index]["cycle"]

    print("where a reply is placed in simulated time")
    want = request_cycles + 5 * 16000
    got = [offset(whole, 0, 0), offset(whole, len(hello), 1)]
    check(all(g == want for g in got),
          "default: a reply's start bit begins 5 ms after its request's stop bit ends "
          "(%s cycles after the request's start bit, expected %d)" % (got, want))
    run = run_case(env, "latency40", [[(hello, 0)], [(stars, 0)]], 3, ("--link-latency-ms", "40"))
    want = request_cycles + 40 * 16000
    got = [offset(run, 0, 0), offset(run, len(hello), 1)]
    check(all(g == want for g in got) and run.tx_text() == "~~~",
          "--link-latency-ms 40: 40 ms (%s cycles, expected %d)" % (got, want))
    starts = [e["due"] - whole.rx[0]["due"] for e in whole.rx[:len(hello)]]
    want_starts = [nearest_cycle(10 * k) for k in range(len(hello))]
    check(starts == want_starts,
          "a reply's bytes follow each other back to back: byte k starts at the cycle nearest "
          "to 10k bit times after the first, with no drift (last byte %d cycles in, expected %d)"
          % (starts[-1], want_starts[-1]))

    seen = [e["cycle"] - e["due"] for e in whole.rx]
    check(whole.rx_text() == (hello + stars).decode() and 0 <= min(seen) and max(seen) <= 4,
          "what a listener on the sign's receive pin decodes is byte for byte what the peer "
          "sent, each start bit seen %d to %d cycles after it was due (the first instruction "
          "boundary); every run in this file is held to that" % (min(seen), max(seen)))
    # Fractions of a millisecond are placed at the nearest cycle (16000 cycles in 1 ms).
    run = run_case(env, "latency2.5", [[(hello, 0)], [(stars, 0)]], 3, ("--link-latency-ms", "2.5"))
    want = request_cycles + 40000
    check(offset(run, 0, 0) == want, "--link-latency-ms 2.5: 40000 cycles (%d after the request's "
                                     "start bit, expected %d)" % (offset(run, 0, 0), want))

    print("directives (--link-directives)")
    d = ("--link-directives",)
    run = run_case(env, "delay1.001", [[(b"@1.001\n" + hello, 0)], [(stars, 0)]], 3, d)
    want = request_cycles + 16016
    check(offset(run, 0, 0) == want,
          '"@1.001\\n": 1.001 ms is 16016 cycles, to the nearest cycle (%d, expected %d)'
          % (offset(run, 0, 0), want))
    run = run_case(env, "crlf", [[(b"@250\r\n" + hello, 0)], [(b"@25x\r\n" + stars, 0)]], 3, d)
    check(offset(run, 0, 0) == request_cycles + 250 * 16000
          and run.rx_text() == (hello + b"@25x\r\n" + stars).decode(),
          'a directive line may end "\\r\\n"; a line that is not a directive reaches the sign '
          "with its \\r intact")
    run = run_case(env, "delay250", [[(b"@250\n" + hello, 0)], [(stars, 0)]], 3, d)
    want = request_cycles + 250 * 16000
    check(offset(run, 0, 0) == want and run.tx_text() == "~~~"
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
    check(raw.rx_text().startswith("@250\n{") and rejected(raw),
          "without --link-directives the same bytes go on the wire as they are "
          "(the sign answered %r)" % raw.tx_text()[:16])

    # Only "@<number>\n" and "@none\n" are directives. Anything else that
    # starts with @ goes to the sign as it is, and must not swallow what follows.
    for label, text in [("no newline", b"@250"), ("a letter in the number", b"@25x\n"),
                        ("two points", b"@1.2.3\n"), ("nothing after the @", b"@\n"),
                        ("far too long", b"@" + b"1" * 40 + b"\n")]:
        run = run_case(env, "malformed-" + label.replace(" ", "-"),
                       [[(text + hello, 0)], [(stars, 0)]], 3, d)
        check(run.rx_text() == (text + hello + stars).decode() and rejected(run),
              "a mistyped directive (%s) is not swallowed: its bytes go to the sign, which "
              "rejects that reply, and the next reply arrives as usual" % label)

    print("late and queued replies")
    # With no reply the firmware gives up after its one-second timeout, shows
    # stars, and asks again. Aim the late reply 20 ms after that second request.
    between = none.tx[1]["cycle"] - none.tx[0]["cycle"]
    late_ms = -(-between // 16000) + 20           # a whole number of milliseconds
    plan = [[(b"@%d\n" % late_ms + hello, 0)], [(stars, 0)]]
    late = run_case(env, "late", plan, 3, d)
    first_late = offset(late, 0, 0)
    want = request_cycles + late_ms * 16000
    check(first_late == want,
          "a reply delayed %d ms starts then, long after the firmware stopped waiting for it "
          "(%d cycles, expected %d)" % (late_ms, first_late, want))
    check(late.rx_text() == (hello + stars).decode(),
          "the reply to the second request is queued behind it: the wire carries the late "
          "reply, then the second")
    second = late.rx[len(hello)]["due"]
    late_end = late.rx[0]["due"] + nearest_cycle(10 * len(hello))
    due = late.tx[1]["cycle"] + request_cycles + 5 * 16000
    check(due < late_end and second == late_end,
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
    early = run_case(env, "early", [[(b"@%d\n" % (late_ms - 100) + hello, 0)], [(stars, 0)]], 3, d)
    gap = early.rx[len(hello)]["due"] - early.tx[1]["cycle"]
    check(early.rx[0]["due"] < early.tx[1]["cycle"]
          and gap == request_cycles + 5 * 16000,
          "a late reply that is over before the second request does not move the second "
          "reply: it starts 5 ms after its own request (%d cycles)" % gap)

    # What the firmware made of that reply is its own code, and it differs by
    # firmware. Master's readSerialData() returns from a timed-out read without
    # stopSerial(), so it is still listening while it draws the stars. The
    # firmware with chooser contract 3 stops listening there.
    if "InvalidInput" in early.tx_text():
        check(early.tx_text() == "~~InvalidInput~",
              "that reply arrived while the firmware showed stars after a timed-out read, "
              "still listening: it left damaged bytes in the receive buffer and the next "
              "read failed, once (the sign sent %r)" % early.tx_text())
    else:
        check(early.tx_text() == "~~~",
              "that reply arrived while the firmware showed stars after a timed-out read. This "
              "firmware had stopped listening: nothing of it was received and the next read "
              "was clean (the sign sent %r)" % early.tx_text())

    print("a reply cut short by a client that stays connected")
    # The first reply stops inside a string and the wait for it runs out. The
    # same client then answers the next requests properly. Those answers must
    # be seen as whole replies at once, not as more of the string.
    short_wait = ("--link-wait-ms", "400")
    run = run_case(env, "cut-short", [[(hello[:21], 0)], [(hello, 0)], [(stars, 0)]], 4, short_wait)
    check(run.rx_text() == (hello[:21] + hello + stars).decode() and rejected(run)
          and run.tx_text().count("IncompleteInput") <= 1 and run.tx_text().count("~") == 4,
          "the cut reply goes to the sign when the wait runs out; the sign does not take it for "
          "a message (%s) and the next two replies arrive whole"
          % ("it answers IncompleteInput once" if "IncompleteInput" in run.tx_text() else
             "it lets it go by and asks again %.2f s after its last byte"
             % ((run.tx[1]["cycle"] - run.rx[20]["cycle"]) / 16e6 if len(run.tx) > 1 and len(run.rx) > 20
                else -1)))
    check("NOTE: 1 request(s)" in run.log,
          "only that one request waited out --link-wait-ms; the later ones were answered at "
          "once (%s)" % ([l for l in run.log.splitlines() if "NOTE" in l] or ["no NOTE"])[0][10:60])
    run = run_case(env, "cut-nested", [[(b'{"a":{"b":1}', 0)], [(hello, 0)], [(stars, 0)]], 4, short_wait)
    check(run.rx_text().endswith((hello + stars).decode()) and "NOTE: 1 request(s)" in run.log,
          "the same when the cut reply stops inside a nested object")
    run = run_case(env, "cut-directive",
                   [[(b"@250\n", 0)], [(hello, 0)], [(stars, 0)]], 4, short_wait + d)
    check(run.rx_text() == (hello + stars).decode()
          and offset(run, 0, 1) == request_cycles + 5 * 16000,
          "a directive whose reply never came is dropped when the wait runs out: the next "
          "request's reply is placed by --link-latency-ms, not by the stale delay")
    run = run_case(env, "cut-open-directive",
                   [[(b"@250", 0)], [(hello, 0)], [(stars, 0)]], 4, short_wait + d)
    asked = [i for i, e in enumerate(run.tx) if e.get("char") == "~"]
    check(run.rx_text() == "@250" + (hello + stars).decode() and len(asked) == 4
          and offset(run, 4, asked[1]) == request_cycles + 5 * 16000,
          "a directive that never got its newline goes to the sign as the bytes it is when the "
          "wait runs out (the sign sent %r), and does not delay the next reply: that one "
          "starts 5 ms after its own request" % run.tx_text()[:16])

    print("bytes sent after a reply in the same write")
    # They are no part of the answer: they go on the wire when the emulator has read them,
    # behind the reply. One write is not one read, so "when" is not a cycle this test can
    # name (if the same read brought them it is the reply's last stop bit; if a later one
    # did, later). What holds however TCP cut the write: behind the reply, in order, and
    # long before the sign's next request, which is seconds away.
    run = run_case(env, "trailing", [[(hello + b"xyz", 0)], [(stars, 0)]], 3)
    reply_end = run.rx[0]["due"] + nearest_cycle(10 * len(hello))
    tail = run.rx[len(hello):len(hello) + 3]
    check(run.rx_text() == (hello + b"xyz" + stars).decode()
          and reply_end <= tail[0]["due"] and tail[2]["cycle"] < run.tx[1]["cycle"],
          "they go on the wire behind the reply (not before its last stop bit), not held "
          "back until the next one")

    print("a newline after each reply")
    nl_same = run_case(env, "nl-same", [[(hello + b"\n", 0)], [(stars + b"\n", 0)]], 3)
    nl_later = run_case(env, "nl-later", [[(hello, 0), (b"\n", 0.3)], [(stars, 0), (b"\n", 0.3)]], 3)
    check(nl_same.tx_text() == "~~~" and nl_same.rx_text() == (hello + b"\n" + stars).decode(),
          "the newline travels in front of the next reply and the sign parses both")
    check(nl_same.lines == nl_later.lines,
          "sent with the reply or 0.3 s after it: identical dump")

    print("the line held LOW (--link-directives)")
    # While the sign draws HELLO it is not listening, so nothing here depends on
    # what the firmware makes of a break: these checks are about the wire.
    # What comes behind a reply is placed from the moment the emulator read it.
    # One write is not one read: a read can end with the reply, the clock would
    # run on, and the push behind it would be placed by this computer's timing.
    # So every reply here that has something behind it is a batch ("@batch" ...
    # "@end"): the clock stays held for the request until "@end" has arrived,
    # and what is in between is placed with the clock where the request left
    # it: the middle of the request's stop bit (9.5 bit times after its start
    # bit began) and at most one instruction later.
    after_reply = (19 * 16000000 + 14400) // (2 * 14400)
    push = b"@300\n@break50\n@after20\n" + stars

    def batch(*parts):
        return b"@batch\n" + b"".join(parts) + b"@end\n"

    run = run_case(env, "break", [[(batch(hello, push), 0)], [(stars, 0)]], 3, d)
    b = run.breaks[0] if len(run.breaks) == 1 else {"cycle": 0, "due": 0, "end": 0, "ms": 0}
    placed = b["due"] - run.tx[0]["cycle"] - 300 * 16000
    check(len(run.breaks) == 1 and 0 <= b["cycle"] - b["due"] <= 4
          and b["end"] - b["due"] == 50 * 16000 and b["ms"] == 50,
          '"@break50\\n": the line falls at its due cycle and is due to rise exactly 50 ms later '
          "(%d breaks, %d cycles)" % (len(run.breaks), b["end"] - b["due"]))
    check(after_reply <= placed <= after_reply + 4,
          '"@300\\n" in front of it: the break is placed 300 ms after the bytes were read '
          "(%d cycles after that, expected %d to %d)" % (placed, after_reply, after_reply + 4))
    first = run.rx[len(hello)] if len(run.rx) > len(hello) + 1 else {"due": 0}
    check(run.rx_text() == (hello + stars + stars).decode() and first["due"] == b["end"] + 20 * 16000
          and run.rx[len(hello) + 1]["due"] == first["due"] + nearest_cycle(10),
          '"@after20\\n" before what follows: its first start bit is due exactly 20 ms after the '
          "break ended, and its other bytes follow back to back (%d cycles after the break)"
          % (first["due"] - b["end"]))
    again = run_case(env, "break-again", [[(batch(hello, push), 0)], [(stars, 0)]], 3, d)
    check(again.lines == run.lines, "the same run again: identical dump")
    cut = run_case(env, "break-cut",
                   [[(b"@batch\n" + hello, 0), (push[:9], 0.3), (push[9:] + b"@end\n", 0.3)],
                    [(stars, 0)]], 3, d)
    check(cut.lines == run.lines,
          "the same sent in three writes 0.3 s apart, the first ending with the reply: the "
          "clock stood still until @end, identical dump")
    run = run_case(env, "break-answer", [[(b"@break10\n@after1\n" + hello, 0)]], 2, d)
    cut = run_case(env, "break-answer-cut", [[(b"@break10\n", 0), (b"@after1\n" + hello, 0.3)]], 2, d)
    b = run.breaks[0] if len(run.breaks) == 1 else {"due": 0, "end": 0}
    check(len(run.breaks) == 1 and b["due"] == run.tx[0]["cycle"] + request_cycles + 5 * 16000
          and run.rx[0]["due"] == b["end"] + 16000 and run.rx_text() == hello.decode(),
          "a break sent as the answer to a request is placed like a reply (5 ms after the "
          "request), and the reply behind it starts 1 ms after the line rises")
    check(cut.lines == run.lines,
          "a break is not a reply: the clock stays held for the reply that comes 0.3 s later "
          "(identical dump)")
    for label, text in [("no length", b"@break\n"), ("a length of 0", b"@break0\n"),
                        ("a letter in the number", b"@after2x\n"), ("another word", b"@low50\n")]:
        run = run_case(env, "notbreak", [[(text + hello, 0)]], 2, d)
        check(not run.breaks and run.rx_text().startswith(text.decode() + "{"),
              "a mistyped directive (%s) is not swallowed: its bytes go to the sign" % label)

    print("the line has one level at a time")
    # A reply with no gap behind a break: the line does not rise between the
    # break and the reply's start bit, so that start bit is no falling edge and
    # nothing can find the byte. (The first falling edge is the first 0 bit
    # that follows a 1 bit inside the byte, at least a bit time later.)
    glued = run_case(env, "break-glued",
                     [[(batch(hello, b"@300\n@break10\n@after0\n" + stars), 0)], [(stars, 0)]], 3, d,
                     tolerate_wire=True)
    b = glued.breaks[0] if len(glued.breaks) == 1 else {"cycle": 0, "end": 0}
    edges = [e["cycle"] for e in glued.rx if e["cycle"] >= b["cycle"]]
    check(bool(edges) and min(edges) >= b["end"] + nearest_cycle(1),
          '"@after0\\n": no falling edge at the end of the break; the first one a listener '
          "finds is %d cycles later, inside the byte" % ((min(edges) - b["end"]) if edges else -1))
    check("no gap behind" in glued.log,
          "and the emulator says that those bytes were not seen on the wire as they were sent")
    gap = run_case(env, "break-gap",
                   [[(batch(hello, b"@300\n@break10\n@after0.5\n" + stars), 0)], [(stars, 0)]], 3, d)
    b = gap.breaks[0] if len(gap.breaks) == 1 else {"end": 0}
    check(gap.rx_text() == (hello + stars + stars).decode()
          and gap.rx[len(hello)]["due"] == b["end"] + 8000,
          '"@after0.5\\n": the line rises for half a millisecond and every byte is read')

    # The guard is the wire's, not the exchange's. A break, its "@after20", and then nothing
    # until the emulator has given up waiting for the reply (0.3 s of real time here, the
    # reply 0.32 s after the break was written, the run paced so that the break is still on
    # the wire when the reply comes): the reply still keeps the gap.
    late = run_case(env, "break-then-late-reply", [[(b"@break50\n@after20\n", 0), (hello, 0.32)]], 2,
                    d + ("--speed", "1", "--link-wait-ms", "300"))
    b = late.breaks[0] if len(late.breaks) == 1 else {"end": 0}
    check("NOTE: 1 request(s)" in late.log and late.rx_text() == hello.decode()
          and late.rx[0]["due"] >= b["end"] + 20 * 16000,
          "a reply that comes after the emulator stopped waiting for it, behind a break and its "
          '"@after20": its first start bit is still no sooner than 20 ms after the line rose '
          "(%d cycles after), and every byte is read"
          % (late.rx[0]["due"] - b["end"] if late.rx else -1))

    print("without a batch (--link-directives)")
    # The same reply and push in one write with no batch around them. Where the
    # push lands then depends on whether one read brought it all; what must hold
    # either way: nothing is lost, and the push is on the wire behind the reply.
    loose = run_case(env, "no-batch", [[(hello + push, 0)], [(stars, 0)]], 3, d)
    lb = loose.breaks[0] if len(loose.breaks) == 1 else {"due": 0}
    check(len(loose.breaks) == 1 and loose.rx_text() == (hello + stars + stars).decode()
          and lb["due"] - loose.tx[0]["cycle"] >= after_reply + 300 * 16000,
          "a reply and a push behind it in one write, no @batch: both are placed, the push no "
          "sooner than 300 ms after the request was read")

    reconnection_tests(env, hello, stars, request_cycles)

    if failures:
        print("FAILED: %d link check(s)" % len(failures))
        sys.exit(1)
    print("link: all checks passed")


if __name__ == "__main__":
    main()
