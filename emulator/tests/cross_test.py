#!/usr/bin/env python3
"""Bytes of the app's that begin while the sign's request goes out, and the answer behind
them, on the real firmware.

The sign cannot receive while it sends (SoftwareSerial sends a byte with interrupts off). A
byte the app wrote behind its last message (a newline, say) can arrive just then, and the
answer to the request can follow it at once: the app's UART sends them back to back. That
answer is the app's next message, taken from its list. It must be read.

  cross_test.py <sign-emu> <firmware.hex> <Fonts.h> <work dir> [port] [--full] [--expect-lost]

An idle sign asks, gets no answer, shows its stars and asks again; when, is the same in every
run (one run without an app tells). So everything here is placed from request 1, in simulated
time, and falls on request 2 to the cycle:

  glued   some bytes (a trailer) and the answer in one piece, back to back, the trailer's
          first start bit moved through request 2 from before its start bit to behind its stop
          bit. Trailers of 1 to 14 bytes, of white space and of bytes with their last falling
          edge as late as a byte can have it (0x7F).
  gapped  a byte with nothing but a start bit (0xFF: the line LOW for one bit time, HIGH for
          nine), one or five of them back to back, moved through request 2 the same way, and
          the answer 0.02 to 20 ms behind the end of the last of them.

  moment  two bytes back to back in fine steps (a fiftieth of a bit time) through the moment
          just in front of request 2's start bit: the first of them is then received between
          the sign's last look at its receive buffer and its start bit, and the second would
          fall under the request. The sign must hold its request. Request 2, whenever it
          comes, is answered 5 ms later. (Steps in which the first byte itself falls under the
          request do not count: see the end of this text.)
  before  two bytes that are no answer ("{}") just in front of request 2, where the sign is
          about to listen or has just begun to: the moment in which a byte's start bit is
          seen neither by the sign's look at the line nor by its receiver. The sign may wait
          with its request; whenever it sends it, it is answered then (5 ms later), and that
          answer must be read.

A row counts only if its answer begins after the last data bit of the request the sign really
sent (nobody answers a request that has not been sent; and a sign that hears the trailer
coming waits with its request, which is right, and makes the row no test of anything). In
every row that counts the sign must show the answer, and must not ask a second time before it
does. Rows whose first byte does not fall under the request are in the sweep too.

Not in the table, because the sign does not try (see ChooserDisplay.h, "what stands where the
answer is waited for"): a frame nobody asked for, of characters of text, under the request or
behind it, and the answer close behind that. And because no UART does it: a LOW that is no byte (noise) under the request,
and the answer less than a byte time behind it. The sign takes the LOW for the start of a
byte and listens again when that byte would be over.

--full is the whole table (about 950 runs, for the record); without it one row per bit of the
request and per kind (about 150 runs). --expect-lost is for firmware from before the fix: the
run then says how many rows it loses, and fails if it loses none.

Python 3 standard library only.
"""
import json
import os
import socket
import subprocess
import sys
import time

from check_frames import Frame, find_on_both_sides, load_font_std, text_columns

CYCLES_PER_MS = 16000
BIT = 16000000 / 14400.0                       # cycles in one bit time
REQUEST_END = -(-10 * 16000000 // 14400)       # where a reply's delay is counted from: 10 bit times, rounded up
REPLY = b'{"type":"F","str":"HI"}'
# (a plain one; and one whose text ends with a brace, so that it reads `{"` where no message opens)
STRAYS = [b'{"type":"F","str":"NO"}', b'{"type":"F","str":"NO{"}']


def run(emu, hexfile, work, port, name, first_answer, duration, later_answer=b"@none\n"):
    """One run: request 1 gets first_answer, every later request later_answer. Returns the dump's events."""
    dump = os.path.join(work, name + ".jsonl")
    with open(os.path.join(work, name + ".log"), "w") as err:
        proc = subprocess.Popen([emu, "--headless", "--port", str(port), "--wait-client", "30",
                                 "--link-directives", "--duration", "%.3f" % duration, "--frames", dump,
                                 hexfile], stderr=err)
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
                    if byte == ord("~"):
                        seen += 1
                        sock.sendall(first_answer if seen == 1 else later_answer)
        except OSError:
            pass
        sock.close()
        proc.wait(timeout=300)
    finally:
        if proc.poll() is None:
            proc.kill()
    if proc.returncode != 0:
        sys.exit("FAILED: sign-emu exited with status %s in run %s" % (proc.returncode, name))
    return [json.loads(line) for line in open(dump, encoding="utf-8").read().splitlines()[1:]]


def directive_ms(cycles):
    return b"%.6f" % (cycles / float(CYCLES_PER_MS))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    full = "--full" in sys.argv
    expect_lost = "--expect-lost" in sys.argv
    if len(args) < 4:
        sys.exit(__doc__)
    emu, hexfile, font_path, work = args[:4]
    port = int(args[4]) if len(args) > 4 else 14437
    os.makedirs(work, exist_ok=True)
    font = load_font_std(font_path)
    cols = text_columns(font, "HI")
    stray_cols = text_columns(font, "NO")

    alone = [e for e in run(emu, hexfile, work, port, "alone", b"@none\n", 10) if e["type"] == "tx"]
    period = alone[1]["cycle"] - alone[0]["cycle"]
    alone_t2 = alone[1]["cycle"]
    duration = alone[1]["cycle"] / 16e6 + 1.6          # request 2, and time to show the answer
    print("an idle sign asks again %.3f ms after it asked" % (period / float(CYCLES_PER_MS)))

    def place(bits):
        """The directive that puts an item's first bit `bits` bit times after request 2's start bit."""
        return b"@" + directive_ms(period - REQUEST_END + round(bits * BIT)) + b"\n"

    # where the first bit falls, in bit times from the request's start bit: before it, in each of
    # its nine bits (start and data), in its stop bit, and behind it
    if full:
        offsets = [-14.0, -10.0, -6.0, -4.0, -3.0] + [q / 4.0 for q in range(-8, 49)]
    else:
        offsets = [-6.5, -1.5, -0.5] + [b + 0.5 for b in range(10)] + [12.5]
    glued = [(b"\r\n", "\\r\\n"), (b"\x7f", "one 0x7F"), (b" " * 14, "14 spaces"),
             (b"\x7f\r\n" * 3, "9 bytes, 0x7F \\r \\n three times")]
    if full:
        glued += [(b"\n", "\\n"), (b"\r\n" * 7, "14 bytes of \\r\\n"), (b"\x7f" * 14, "14 times 0x7F")]
    gaps_ms = [0.02, 0.4, 0.8, 2.0, 20.0] if not full else [0.02, 0.1, 0.25, 0.4, 0.6, 0.8, 1.2, 2.0, 5.0, 9.0, 20.0]
    pulses = [1, 5] if full else [1]
    gap_offsets = [-0.5, 0.25] + [b + 0.5 for b in range(9)] + [8.75, 9.25, 12.5] if full else [0.5, 3.5, 6.5, 8.5]

    rows = []           # (kind, bits, crossed, read, text)
    skipped = [0]
    def one(kind, name, bits, item, said, later_answer=b"@none\n"):
        more = {"before": 0.4, "moment": 0.4, "long": 1.6}.get(kind, 0)
        events = run(emu, hexfile, work, port, name, item, duration + more, later_answer)
        asks = [e for e in events if e["type"] == "tx" and e.get("char") == "~"]
        if len(asks) < 2:
            sys.exit("FAILED: the sign did not ask twice in run %s" % name)
        t2 = asks[1]["cycle"]
        first = [e for e in events if e["type"] in ("rx", "break") and e.get("due", 0) > t2 - 20 * BIT]
        shown = [e["cycle"] for e in events if e["type"] == "frame" and e["cycle"] > t2
                 and find_on_both_sides(Frame(e), cols) is not None]
        # (HI in a frame that began before any answer could be read is not HI: the two bytes of
        # a "before" row are no text)
        again = [e for e in asks[2:] if not shown or e["cycle"] < shown[0]]
        others = [e for e in events if e["type"] == "tx" and e["cycle"] > t2 and e.get("char") != "~"]
        wrong = [e for e in events if e["type"] == "frame" and e["cycle"] > t2
                 and find_on_both_sides(Frame(e), stray_cols) is not None]
        read = bool(shown) and not again and not others and not wrong
        began = (first[0]["due"] - t2) / BIT if first else None
        crossed = began is not None and 0 <= began < 9
        # where the answer begins: the brace of the reply (in a "before" and a "frame" row what
        # stands in front has braces of its own, and the reply's is the last one)
        if kind == "moment" and abs(t2 - alone_t2) < CYCLES_PER_MS:
            # The sign sent its request when it would have with a silent line: the first of the
            # two bytes did not come in front of it but fell under it. That is text under the
            # request, which the sign lets by for a second before it asks again (by decision:
            # ChooserDisplay.h, "what stands where the answer is waited for"), not the moment
            # this kind of row is about.
            skipped[0] += 1
            return
        brace = [e for e in events if e["type"] == "rx" and e["byte"] == ord("{")]
        brace = brace[:1] if kind in ("glued", "gapped") else brace[-1:]
        if not brace or brace[0]["due"] < t2 + 9 * BIT:
            skipped[0] += 1         # (an "answer" that begins before the request is out is none)
            return
        rows.append((kind, bits, crossed, read))
        if not read or full:
            print("  %s  %s, first bit %5.2f bit times into the request%s: %s"
                  % ("ok  " if read else "LOST", said, bits,
                     "" if began is None else " (on the wire: %.2f)" % began,
                     "read, HI shown" if read else
                     "NOT read" + (", the sign asked again" if again else "")
                     + (", the stray frame was shown" if wrong else "")
                     + (", it sent %r" % "".join(e.get("char", "?") for e in others) if others else "")))

    for trailer, said in glued:
        for bits in offsets:
            one("glued", "glued-%d-%s-%05.2f" % (len(trailer), trailer[:1].hex(), bits), bits,
                place(bits) + trailer + REPLY, "%s and the answer back to back" % said)
    for count in pulses:
        for gap in gaps_ms:
            for bits in gap_offsets:
                item = place(bits) + b"@break%s\n" % directive_ms(round(BIT))
                for _ in range(count - 1):
                    item += b"@after%s\n@break%s\n" % (directive_ms(round(9 * BIT)), directive_ms(round(BIT)))
                # (the byte ends nine bit times after the line rose)
                item += b"@after%s\n" % directive_ms(round(9 * BIT) + round(gap * CYCLES_PER_MS)) + REPLY
                one("gapped", "gap-%d-%s-%05.2f" % (count, gap, bits), bits, item,
                    "%d byte(s) 0xFF, the answer %g ms behind" % (count, gap))

    # the moment between the sign's last look at its receive buffer and its start bit
    for fiftieths in (range(-20, 2) if not full else range(-40, 6)):
        bits = fiftieths / 50.0
        one("moment", "moment-%06.2f" % bits, bits, place(bits) + b"{}",
            "\"{}\" in front of the request, the request answered when it comes", REPLY)

    before_offsets = [q / 4.0 for q in range(-80, 0)] if full else [-17.5, -3.0, -2.0, -1.5, -1.0, -0.5, -0.25]
    for bits in before_offsets:
        one("before", "before-%06.2f" % bits, bits, place(bits) + b"{}",
            "\"{}\" in front of the request, the request answered when it comes", REPLY)

    crossed = [r for r in rows if r[2]]
    lost = [r for r in rows if not r[3]]
    really_lost = len(lost)
    lost_crossed = [r for r in crossed if not r[3]]
    print("%d rows (and %d that do not count: the answer would have begun before the request was out); in "
          "%d the first byte began under the request (its start bit or a data bit)"
          % (len(rows), skipped[0], len(crossed)))
    print("the answer was read in %d of %d rows; of the rows that crossed, in %d of %d"
          % (len(rows) - len(lost), len(rows), len(crossed) - len(lost_crossed), len(crossed)))
    if expect_lost:
        if not lost_crossed:
            sys.exit("FAILED: --expect-lost, and this firmware lost no row")
        print("cross test: this firmware loses the answer in %d row(s), as expected of it" % really_lost)
        return
    before = [r for r in rows if r[0] == "before"]
    print("rows with two bytes just in front of the request: %d, the answer read in %d"
          % (len(before), len([r for r in before if r[3]])))
    for kind, least in (("moment", 15),):
        of_kind = [r for r in rows if r[0] == kind]
        print("rows of the kind '%s': %d, the answer read in %d" % (kind, len(of_kind), len([r for r in of_kind if r[3]])))
        if len(of_kind) < least:
            lost.append(("too few rows of the kind", kind))
    if lost or len(crossed) < 20 or len(rows) < 60 or len(before) < 5:
        sys.exit("FAILED: %d row(s) lost the answer (%d rows crossed)" % (len(lost), len(crossed)))
    print("cross test: all checks passed")


if __name__ == "__main__":
    main()
