#!/usr/bin/env python3
"""Checks that every line of a frames dump is JSON and that its first line
names the firmware file it was given, whatever characters the name holds.

usage: check_meta.py <frames.jsonl> <firmware path as passed to sign-emu>
"""
import json
import sys

lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
try:
    events = [json.loads(line) for line in lines]
except ValueError as e:
    print("  FAIL  every line of the dump is JSON: %s" % e)
    sys.exit(1)
meta = events[0]
ok = meta.get("type") == "meta" and meta.get("firmware") == sys.argv[2] and "render" in meta
print("  %s  the dump's %d lines are JSON and its first names the firmware as %r"
      % ("ok  " if ok else "FAIL", len(lines), meta.get("firmware")))
ok2 = events[-1].get("type") == "end"
print("  %s  the dump ends with an end line (t=%s)" % ("ok  " if ok2 else "FAIL", events[-1].get("t")))
sys.exit(0 if ok and ok2 else 1)
