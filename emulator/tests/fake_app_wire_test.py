#!/usr/bin/env python3
"""What fake_app.py --contract 3 writes when it leaves chooser mode, as it is
on the simulated wire.

Contract 3.1, rule 4: nothing leaves behind a break before the guard. A byte
glued to a break has no start edge (the line does not rise in between), so no
receiver finds it; the first byte of a message is then lost after the app has
given the message away. The fake app must keep the rule the real app keeps.

The app here is fake_app.App itself, with this script's clock, talking to
sign-emu over the TCP link exactly as fake_app.py does (every write it
returns is sent as it is). What is checked is the frames dump: the break, and
the bytes a listener on the sign's receive pin DECODED from the line.

  1. A request is waiting at the moment chooser mode ends: the app holds the
     line LOW and answers that request with the message it left for.
  2. The same moment by the fake app's other path: its loop finds chooser mode
     over with nothing to read (the line LOW, nothing behind it) and reads
     the request next. The request was on its way either way, so the wire is
     the same as in 1; what differs is the code that owes the guard.
  3. Chooser mode ends unasked, with no request anywhere (the break goes out
     by itself), with --guard-ms 200. The sign's next request comes when the
     sign sends it; firmware that reads a pushed line asks 150 ms after the
     line rose, which is inside that guard.

In 1 and 2 the answer's first start bit must be due exactly --guard-ms after
the line rises; in 3 no sooner (and exactly then for firmware that asks inside
the guard). In all three every byte of the answer must be found on the pin.
That is a matter of the wire alone, so it holds for any firmware. With
--contract3 (this firmware reads a pushed line) the sign must also take that
answer without asking twice and without an error.

usage: fake_app_wire_test.py <sign-emu> <firmware.hex> <work dir> [port] [--contract3]
Python 3 standard library only.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import fake_app  # noqa: E402
import link_test  # noqa: E402
from link_test import REQUEST_CYCLES, Session, check  # noqa: E402

CYCLES_PER_MS = 16000
PICK = '{"type":"H","str":"PICK","flashy":"1/2","_seconds":5}'
BYE = '{"type":"F","str":"BYE"}'
STARS = '{"type":"S","str":"","stars":5}'


def make_app(work, break_ms, guard_ms):
    path = os.path.join(work, "leave.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join([PICK, BYE, STARS]) + "\n")
    args = argparse.Namespace(playlist=path, once=False, quiet=True, chooser_polls=40, contract=3,
                              idle=STARS, chooser_seconds=6, break_ms=break_ms, guard_ms=guard_ms,
                              interactive=False)
    return fake_app.App(args)


def leave(env, name, how, break_ms=50, guard_ms=20):
    """Request 1 is answered with a chooser message, whose time is then up.
    how = "waiting": the app finds that out as it reads request 2 (on_request).
    how = "loop": its run loop found it first, with nothing to read (push_due), and request 2
          is read next.
    how = "unasked": its run loop finds it right after request 1 was answered: the break goes
          out with no request anywhere."""
    app = make_app(env["work"], break_ms, guard_ms)
    now = [1000.0]
    app.clock = lambda: now[0]
    # The run is cut off by simulated time, long after the exchange, whatever the firmware
    # asks in between: every later request is answered from the playlist as fake_app.py would.
    run = Session(env, name, 1000000, ("--link-directives", "--duration", "40"))
    sock = run.connect()
    requests = 0
    sent = []
    try:
        while True:
            data = sock.recv(1)
            if not data:
                break
            if data not in (b"~", b"?"):
                continue
            requests += 1
            writes = []
            if requests == 2 and how != "unasked":
                now[0] += 10.0                       # the selection's 5 seconds are over
                if how == "loop":
                    writes.append(app.push_due())    # what fake_app.py's loop does when idle
            writes += app.on_request(data[0]) or []
            if requests == 1 and how == "unasked":
                now[0] += 10.0
                writes.append(app.push_due())        # a write of its own, behind the answer
            for chunk, _ in writes:
                sock.sendall(chunk)                  # one write each, as fake_app.py sends them
                if requests == 2 or (requests == 1 and chunk.startswith(b"@break")):
                    sent.append(chunk)
    except (ConnectionResetError, BrokenPipeError):
        pass                                         # the emulator reached its end and hung up
    return run.finish(sock), sent


def check_leave(env, name, what, how, contract3, guard_ms=20):
    try:
        run, sent = leave(env, name, how, guard_ms=guard_ms)
    except RuntimeError as e:
        # Run() refuses a dump in which a start bit was not seen where it was due, and a
        # run in which sign-emu warned that bytes were not seen on the wire as they were sent.
        check(False, "%s: the wire carried what the app sent (%s)" % (what, e))
        return
    bye = BYE.encode("ascii")
    pick = fake_app.encode(fake_app.json.loads(PICK))
    print("        the app wrote for the break and behind it: %r" % sent)
    check(len(run.breaks) == 1 and len(run.tx) >= 2,
          "%s: one break on the wire (%d), the sign asked at least twice (%d)"
          % (what, len(run.breaks), len(run.tx)))
    if len(run.breaks) != 1 or len(run.tx) < 2:
        return
    b = run.breaks[0]
    behind = [e for e in run.rx if e["due"] >= b["cycle"]]
    decoded = bytes(e["byte"] for e in behind[:len(bye)])
    guard = guard_ms * CYCLES_PER_MS
    if how == "unasked":
        asked = [e for e in run.tx if e["cycle"] > b["cycle"]]
        check(b["ms"] == 50 and run.tx[1]["cycle"] > b["cycle"],
              "the line falls with no request of the sign's waiting, for 50 ms; the sign's "
              "next request comes %.1f ms after it rose"
              % ((asked[0]["cycle"] - b["end"]) / CYCLES_PER_MS if asked else -1))
    else:
        check(b["due"] == run.tx[1]["cycle"] + REQUEST_CYCLES + 5 * CYCLES_PER_MS and b["ms"] == 50,
              "the line falls 5 ms after the sign's second request, as a reply would begin, and "
              "stays LOW for 50 ms")
    check(decoded == bye,
          "the bytes a listener on the receive pin decoded behind the break are the message "
          "the app left chooser mode for, from its opening brace (%r)" % decoded)
    exactly = how != "unasked" or contract3
    check(bool(behind) and (behind[0]["due"] == b["end"] + guard if exactly
                            else behind[0]["due"] >= b["end"] + guard),
          "and its first start bit is due %s %g ms after the line rose (%s cycles after, "
          "expected %d%s)" % ("exactly" if exactly else "no sooner than", guard_ms,
                             behind[0]["due"] - b["end"] if behind else None, guard,
                             "" if exactly else " or more"))
    before = bytes(e["byte"] for e in run.rx if e["due"] < b["cycle"])
    check(before.startswith(pick), "(before the break the wire carried the chooser message)")
    if contract3:
        text = run.tx_text()
        end_of_bye = behind[len(bye) - 1]["cycle"] if len(behind) >= len(bye) else 0
        asks = [e for e in run.tx if b["cycle"] < e["cycle"] <= end_of_bye]
        check(set(text) == {"~"} and len(asks) == (1 if how == "unasked" else 0),
              "this firmware %s: it asks %s until the message is in, and never sends an error "
              "text (it sent %r)"
              % ("saw the line LOW, heard nothing behind it, and asked" if how == "unasked" else
                 "reads on past the break and takes that message as the answer",
                 "once" if how == "unasked" else "nothing more", text[:24]))


def main():
    args = [a for a in sys.argv[1:] if a != "--contract3"]
    contract3 = "--contract3" in sys.argv[1:]
    if len(args) < 3:
        sys.exit(__doc__)
    env = {"emu": args[0], "hex": args[1], "work": args[2],
           "port": int(args[3]) if len(args) > 3 else 14418}
    os.makedirs(env["work"], exist_ok=True)

    print("the fake app leaves chooser mode: what is on the wire behind its break")
    check_leave(env, "leave-waiting", "a request is waiting as chooser mode ends", "waiting", contract3)
    check_leave(env, "leave-loop",
                "the app's loop finds chooser mode over, then reads the request that was on "
                "its way", "loop", contract3)
    check_leave(env, "leave-unasked",
                "chooser mode ends with no request anywhere, the guard set to 200 ms", "unasked",
                contract3, guard_ms=200)

    if link_test.failures:
        print("FAILED: %d fake app wire check(s)" % len(link_test.failures))
        sys.exit(1)
    print("fake app on the wire: all checks passed")


if __name__ == "__main__":
    main()
