#!/usr/bin/env python3
"""Tests of fake_app.py without a socket: the chooser checksum against the
contract's vectors, what each contract version answers to ~ and ?, and what
version 3 pushes unasked and when.

usage: test_fake_app.py <work dir>
Python 3 standard library only.
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import fake_app  # noqa: E402

failures = []


def check(ok, text):
    print("  %s  %s" % ("ok  " if ok else "FAIL", text))
    if not ok:
        failures.append(text)


def app(work, lines, contract):
    path = os.path.join(work, "playlist.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    args = argparse.Namespace(playlist=path, once=False, quiet=True, chooser_polls=40,
                              contract=contract, idle='{"type":"S","str":"","stars":20}',
                              chooser_seconds=6, break_ms=50, guard_ms=20, interactive=False)
    return fake_app.App(args)


def answers(a, requests):
    return [a.answer(ord(r))[0].decode("ascii") for r in requests]


def main():
    work = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(work, exist_ok=True)

    print("chooser checksum (contract version 2)")
    check(fake_app.crc16_ccitt_false(b"123456789") == 0x29B1,
          "CRC-16/CCITT-FALSE of 123456789 is 0x29B1")
    vectors = [
        ("HELLO W", "3/12", 13345),
        ("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG AND KEEPS ON RUNNING", "7/12", 36894),
        ("<empty>", "1/0", 7805),
        ("", "1/1", 18175),
        ("A", "1/1", 26634),
        ("A", "11/1", 47554),
        ("I \x7f U", "12/345", 46267),
    ]
    got = [fake_app.chooser_checksum({"str": s, "flashy": f}) for s, f, _ in vectors]
    check(got == [n for _, _, n in vectors] and len(vectors[1][0]) == 64,
          "the contract's seven vectors (%s)" % ", ".join(str(n) for n in got))

    a_msg = '{"type":"D","str":"A"}'
    pick1 = '{"type":"H","str":"PICK","flashy":"1/2"}'
    pick2 = '{"type":"H","str":"OTHER","flashy":"2/2"}'
    b_msg = '{"type":"F","str":"B"}'
    playlist = [a_msg, pick1[:-1] + ',"_polls":2}', pick2[:-1] + ',"_polls":1}', b_msg]
    c1 = '{"c":%d}' % fake_app.chooser_checksum({"str": "PICK", "flashy": "1/2"})
    c2 = '{"c":%d}' % fake_app.chooser_checksum({"str": "OTHER", "flashy": "2/2"})

    print("contract version 1")
    got = answers(app(work, playlist, 1), "~~??~?~")
    check(got == [a_msg, pick1, "{}", "{}", pick2, "{}", b_msg],
          "~ sends the next message; ? gets {} while the chooser's polls last (%s)" % " ".join(got))

    print("contract version 2")
    got = answers(app(work, playlist, 2), "~~??")
    check(got == [a_msg, pick1, c1, c1],
          "? gets the checksum of the selected chooser message, every time (%s)" % " ".join(got))
    got = answers(app(work, playlist, 2), "~~?~?")
    check(got == [a_msg, pick1, c1, pick1, c2],
          "~ in chooser mode sends the selected message again; when the selection moves on, "
          "? reports the new checksum (%s)" % " ".join(got[2:]))
    got = answers(app(work, playlist, 2), "~~???~??~")
    check(got == [a_msg, pick1, c1, c1, c2, pick2, b_msg, a_msg, pick1],
          "after the last chooser message, ? is answered like ~ with the next message in full")
    check(all('"type"' not in x for x in (c1, c2)) and c1 != c2,
          "a checksum reply has no type and differs between the two messages (%s, %s)" % (c1, c2))

    print("contract version 3")
    now = [100.0]
    a = app(work, [a_msg, pick1[:-1] + ',"_seconds":2.5}', pick2, b_msg], 3)
    a.clock = lambda: now[0]
    check(a.push_due() == (None, None) and a.next_push_at() is None,
          "nothing is pushed while no chooser message is selected")
    got = answers(a, "~~~?~")
    check(got == [a_msg, pick1, pick1, c1, pick1],
          "requests are answered as in version 2, however many come (%s)" % " ".join(got[2:]))
    now[0] += 2.4
    check(a.push_due() == (None, None) and a.next_push_at() == 102.5,
          "the selection lasts its _seconds, counted from the answer that selected it")
    now[0] += 0.1
    push, _ = a.push_due()
    check(push == b"@break50\n@after20\n" + pick2.encode("ascii"),
          "then the next message is pushed: the line LOW for 50 ms, a wait of 20 ms, the "
          "message (%r)" % push)
    got = answers(a, "~?")
    check(got == [pick2, c2] and a.push_due() == (None, None),
          "and requests are answered about the new selection, which is not pushed twice")
    now[0] += 5.9
    check(a.push_due() == (None, None), "a message without _seconds stays for --chooser-seconds")
    now[0] += 0.2
    push, _ = a.push_due()
    check(push == b"@break50\n" and a.next_push_at() is None and a.push_due() == (None, None),
          "leaving chooser mode is the line LOW and no message (%r), and then nothing more "
          "is pushed" % push)
    got = answers(a, "~~~")
    check(got == [b_msg, a_msg, pick1],
          "the message it left chooser mode for goes to the sign's next request, once")
    a.args.break_ms, a.args.guard_ms = 12.5, 0.25
    now[0] += 10
    push, _ = a.push_due()
    check(push.startswith(b"@break12.5\n@after0.25\n{"),
          "--break-ms and --guard-ms are written as plain decimal numbers (%r)" % push[:25])
    refused = []
    for guard in ("0", "0.0004", "-1"):
        p = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(fake_app.__file__)),
                                                         "fake_app.py"),
                            "--contract", "3", "--guard-ms", guard, os.path.join(work, "playlist.jsonl")],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        if p.returncode == 2 and b"--guard-ms" in p.stderr:
            refused.append(guard)
    check(refused == ["0", "0.0004", "-1"],
          "a guard of nothing (0, or too small to be written as more than 0) is refused on the "
          "command line: the message would be glued to the break (%s)" % ", ".join(refused))

    print("contract version 3: a change that is overdue when a request is read")
    now = [500.0]
    a = app(work, [a_msg, pick1[:-1] + ',"_seconds":2.5}', pick2, b_msg], 3)
    a.clock = lambda: now[0]
    answers(a, "~~")                     # the first message, then pick1 is selected
    now[0] += 2.6                        # its time is up, and a request is read in the same turn
    got = a.on_request(ord("~"))
    check(len(got) == 1 and got[0][0] == b"@break50\n@after20\n" + pick2.encode("ascii"),
          "the new message is pushed before the request is answered, and the push is the "
          "answer: one frame, never the old message (%r)" % [g[0] for g in got])
    got = a.on_request(ord("~"))
    check([g[0] for g in got] == [pick2.encode("ascii")],
          "the next request is answered with the new selection")
    now[0] += 6.1                        # the app leaves chooser mode, a request is waiting
    got = a.on_request(ord("~"))
    check([g[0] for g in got] == [b"@break50\n", b"@after20\n" + b_msg.encode("ascii")],
          "leaving chooser mode with a request waiting: the line LOW, then, no sooner than the "
          "guard after it, that request is answered with the message it left for (%r)"
          % [g[0] for g in got])
    got = a.on_request(ord("~"))
    check([g[0] for g in got] == [a_msg.encode("ascii")],
          "and nothing is sent twice; a later answer carries no guard")

    print("contract version 3: nothing leaves behind a break before the guard")
    now = [900.0]
    a = app(work, [a_msg, pick1[:-1] + ',"_seconds":2.5}', b_msg, pick1, pick2], 3)
    a.clock = lambda: now[0]
    answers(a, "~~")
    now[0] += 2.6                        # nothing to read: the run loop finds the change due
    push, _ = a.push_due()
    got = a.on_request(ord("~"))         # and the sign's request comes in right behind the break
    check(push == b"@break50\n" and [g[0] for g in got] == [b"@after20\n" + b_msg.encode("ascii")],
          "the app leaves chooser mode with no request waiting (the line LOW, nothing behind "
          "it); the next request, whenever it comes, is answered behind the guard (%r)"
          % [g[0] for g in got])
    got = a.on_request(ord("~"))
    check([g[0] for g in got] == [pick1.encode("ascii")], "the answer after that carries no guard")
    now[0] += 6.1
    push, _ = a.push_due()
    got = a.on_request(ord("~"))
    check(push == b"@break50\n@after20\n" + pick2.encode("ascii")
          and [g[0] for g in got] == [pick2.encode("ascii")],
          "a pushed message has its guard between its break and itself, and owes none to the "
          "answer behind it")
    got = []
    for version in (1, 2):
        a = app(work, [a_msg, pick1[:-1] + ',"_seconds":2.5,"_polls":1}', b_msg], version)
        got += [a.on_request(ord(r)) for r in "~~?~?~"]
    check(len(got) == 12 and all(len(g) == 1 and b"@" not in g[0][0] for g in got),
          "contract versions 1 and 2 send no break and no directive at all (%d answers)" % len(got))

    if failures:
        print("FAILED: %d fake app check(s)" % len(failures))
        sys.exit(1)
    print("fake app: all checks passed")


if __name__ == "__main__":
    main()
