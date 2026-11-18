#!/usr/bin/env python3
"""Tests of fake_app.py without a socket: the chooser checksum against the
contract's vectors, and what each contract version answers to ~ and ?.

usage: test_fake_app.py <work dir>
Python 3 standard library only.
"""
import argparse
import os
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
                              contract=contract, idle='{"type":"S","str":"","stars":20}')
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

    if failures:
        print("FAILED: %d fake app check(s)" % len(failures))
        sys.exit(1)
    print("fake app: all checks passed")


if __name__ == "__main__":
    main()
