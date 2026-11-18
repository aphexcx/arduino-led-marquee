#!/usr/bin/env python3
"""Plays the Android app's side of the sign's serial link.

The sign PULLS: it sends one request byte and reads one JSON object back.
  ~   "send the next message"
  ?   "chooser poll": while a chooser message is selected, answered with
      {} (--contract 1, the default) or with {"c":N}, the checksum of the
      selected chooser message (--contract 2 and 3); otherwise like ~

With --contract 3 the app also PUSHES (contract version 3.1). A chooser
message stays selected for a time, not for a number of polls. When that time
is up the playlist moves on and the app says so unasked:
  - to another chooser message: it holds the line LOW (a break, --break-ms),
    waits (--guard-ms) and sends the new message
  - to a message of any other type (it has left chooser mode): it holds the
    line LOW and sends nothing. The sign asks, and that request gets the message.
The break is a link directive, so the emulator must run with
--link-directives; and the time is this computer's, so run the emulator with
--speed 1.

This script connects to the emulator's TCP port (the UART), and answers each
request with the next message from a playlist file, looping. With --interactive
it also sends what you type.

Playlist file: one JSON object per line. Blank lines and lines starting with
# are skipped. Keys starting with "_" are for this script and are not sent:
  "_polls": n   (chooser messages) how long the message stays selected before
                the playlist moves on. Default: --chooser-polls.
                --contract 1: the next n "?" polls are answered with {}.
                --contract 2: the next n requests of either kind are answered
                about this message: "?" with its checksum, "~" with the message
                again (in chooser mode the app's next message is the selected
                one, and asking for it does not use it up).
  "_seconds": s (chooser messages, --contract 3) how long the message stays
                selected before the playlist moves on and the next message is
                pushed. Default: --chooser-seconds. Requests in that time are
                answered about this message, as with --contract 2.

Python 3 standard library only.
"""
import argparse
import json
import select
import socket
import sys
import threading
import time

REQUEST_NEXT = ord("~")
REQUEST_POLL = ord("?")


def crc16_ccitt_false(data):
    """CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final xor."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def chooser_checksum(message):
    """Contract version 2: the bytes of str, one zero byte, the bytes of flashy."""
    text = str(message.get("str", "")).encode("utf-8")
    count = str(message.get("flashy", "")).encode("utf-8")
    return crc16_ccitt_false(text + b"\0" + count)


def load_playlist(path):
    messages = []
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as e:
                sys.exit("%s:%d: not JSON: %s" % (path, number, e))
            if not isinstance(obj, dict) or "type" not in obj:
                sys.exit('%s:%d: every message is a JSON object with a "type"' % (path, number))
            polls = obj.get("_polls", 0)
            if not isinstance(polls, int) or isinstance(polls, bool) or polls < 0:
                sys.exit('%s:%d: "_polls" is a whole number, 0 or more' % (path, number))
            seconds = obj.get("_seconds", 1)
            if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or not 0 < seconds <= 86400:
                sys.exit('%s:%d: "_seconds" is a number of seconds, more than 0' % (path, number))
            messages.append(obj)
    if not messages:
        sys.exit("%s: no messages" % path)
    return messages


def encode(message):
    """The bytes that go on the wire: compact JSON, no newline, like the app's gson."""
    wire = {k: v for k, v in message.items() if not k.startswith("_")}
    return json.dumps(wire, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def directive_number(value):
    """A number as a link directive takes it: digits with at most one point."""
    return ("%.3f" % value).rstrip("0").rstrip(".")


def push_bytes(message, break_ms, guard_ms):
    """Contract version 3, app to sign on a change: the line LOW for break_ms, a wait of
    guard_ms, then the message. The first two are lines for the emulator (--link-directives),
    which puts the break and the wait on the wire; only the message is bytes on it."""
    return ("@break%s\n@after%s\n" % (directive_number(break_ms), directive_number(guard_ms))
            ).encode("ascii") + encode(message)


def parse_typed_line(line):
    """Interactive input: a JSON object, or ':X text' for type X, or plain text."""
    line = line.rstrip("\n")
    if not line.strip():
        return None
    if line.lstrip().startswith("{"):
        return json.loads(line)
    if len(line) >= 2 and line[0] == ":":
        return {"type": line[1].upper(), "str": line[2:].strip()}
    return {"type": "D", "str": line}


class App:
    def __init__(self, args):
        self.args = args
        self.playlist = load_playlist(args.playlist) if args.playlist else []
        self.position = 0
        self.loops = 0
        self.typed = []                  # messages typed in interactive mode
        self.lock = threading.Lock()
        self.chooser_polls_left = 0      # {} answers still owed for the chooser on the sign
        self.chooser = None              # contract 2 and 3: the selected chooser message
        self.chooser_left = 0            # contract 2: requests it stays selected for
        self.chooser_until = 0.0         # contract 3: when (self.clock) the playlist moves on
        self.held = None                 # contract 3: the message it left chooser mode for, not sent yet
        self.clock = time.monotonic      # a test puts its own clock here
        self.last_sent = None
        self.start = time.monotonic()
        self.finished = False

    def log(self, text):
        if not self.args.quiet:
            print("[%8.3f] %s" % (time.monotonic() - self.start, text), flush=True)

    def next_message(self):
        with self.lock:
            if self.typed:
                return self.typed.pop(0)
        if self.playlist:
            if self.position >= len(self.playlist):
                self.position = 0
                self.loops += 1
                if self.args.once:
                    self.finished = True
                    return None
            message = self.playlist[self.position]
            self.position += 1
            return message
        return json.loads(self.args.idle)

    def answer_v2(self, request):
        """Contract version 2: a poll is answered with a report of the app's state."""
        with self.lock:
            typed = bool(self.typed)
        if self.chooser is not None and self.chooser_left > 0 and not typed:
            self.chooser_left -= 1
            if request == REQUEST_POLL:
                data = b'{"c":%d}' % chooser_checksum(self.chooser)
                return data, "%s (checksum of the selected chooser message, %d more)" % (
                    data.decode("ascii"), self.chooser_left)
            data = encode(self.chooser)
            return data, "%s (still selected, %d more)" % (data.decode("ascii"), self.chooser_left)
        was_choosing = self.chooser is not None
        message = self.next_message()
        if message is None:
            return None, None
        if message.get("type") != "H":
            self.chooser = None
            data = encode(message)
            return data, data.decode("ascii")
        self.chooser = message
        self.chooser_left = int(message.get("_polls", self.args.chooser_polls))
        if request == REQUEST_POLL and was_choosing:
            # Still in chooser mode, with a new selection: the poll gets the
            # new checksum, and the sign asks for the message itself with ~.
            data = b'{"c":%d}' % chooser_checksum(message)
            return data, "%s (checksum of the newly selected chooser message)" % data.decode("ascii")
        data = encode(message)
        return data, data.decode("ascii")

    def select_v3(self, message):
        """The playlist has moved on to this message: is the app (still) in chooser mode?"""
        if message.get("type") == "H":
            self.chooser = message
            self.chooser_until = self.clock() + float(
                message.get("_seconds", self.args.chooser_seconds))
        else:
            self.chooser = None

    def answer_v3(self, request):
        """Contract version 3: requests are answered as in version 2, but what is selected
        changes with time (see push_due), not with the number of requests."""
        if self.held is not None:
            message, self.held = self.held, None
            data = encode(message)
            return data, "%s (what it left chooser mode for)" % data.decode("ascii")
        if self.chooser is not None:
            if request == REQUEST_POLL:
                data = b'{"c":%d}' % chooser_checksum(self.chooser)
                return data, "%s (checksum of the selected chooser message)" % data.decode("ascii")
            data = encode(self.chooser)
            return data, "%s (still selected)" % data.decode("ascii")
        message = self.next_message()
        if message is None:
            return None, None
        self.select_v3(message)
        data = encode(message)
        return data, data.decode("ascii")

    def next_push_at(self):
        """Contract version 3: when the next push is due on self.clock, None if none is."""
        if self.args.contract != 3 or self.chooser is None:
            return None
        with self.lock:
            if self.typed:
                return self.clock()      # something was typed: the chooser changes now
        return self.chooser_until

    def push_due(self):
        """Contract version 3: if the selected chooser message's time is up (or something was
        typed), moves on and returns what the app sends unasked, as (bytes, text for the log).
        Otherwise (None, None). Another chooser message is pushed: break, wait, message. A
        message of another type means the app has left chooser mode: it gives the break alone
        and keeps the message for the sign's next request (a push never takes anything from
        the list; the request does)."""
        due = self.next_push_at()
        if due is None or self.clock() < due:
            return None, None
        message = self.next_message()
        if message is None:
            return None, None
        self.select_v3(message)
        if self.chooser is None:
            self.held = message
            data = ("@break%s\n" % directive_number(self.args.break_ms)).encode("ascii")
            return data, ("line LOW %s ms and nothing behind it: chooser mode is over, the sign "
                          "will ask" % directive_number(self.args.break_ms))
        data = push_bytes(message, self.args.break_ms, self.args.guard_ms)
        return data, "line LOW %s ms, wait %s ms, then %s" % (
            directive_number(self.args.break_ms), directive_number(self.args.guard_ms),
            encode(message).decode("ascii"))

    def on_request(self, request):
        """Everything the app writes when it reads a request byte, in order, as a list of
        (bytes, text for the log). None when the playlist is finished.
        Contract version 3: a change that is overdue is handled BEFORE the request is answered
        (the app never answers with a message it no longer holds, and never pushes behind a
        stale answer). If that change pushed a chooser message, the push is the answer: a
        request gets at most one frame. If the app left chooser mode instead (the line LOW,
        no frame), the request is answered with the message it left for."""
        out = []
        if self.args.contract == 3:
            push, shown = self.push_due()
            if push is not None:
                out.append((push, "the selection's time was up: pushed first  ->  %s" % shown))
                if self.held is None:
                    return out
            elif self.finished:
                return None
        reply, shown = self.answer(request)
        if reply is None:
            return out or None
        out.append((reply, "sign asked %s  ->  %s" % (chr(request), shown)))
        return out

    def answer(self, request):
        if self.args.contract == 3:
            return self.answer_v3(request)
        if self.args.contract == 2:
            return self.answer_v2(request)
        if request == REQUEST_POLL and self.chooser_polls_left > 0 and not self.typed:
            self.chooser_polls_left -= 1
            return b"{}", "{} (chooser unchanged, %d more)" % self.chooser_polls_left
        message = self.next_message()
        if message is None:
            return None, None
        self.chooser_polls_left = 0
        if message.get("type") == "H":
            self.chooser_polls_left = int(message.get("_polls", self.args.chooser_polls))
        data = encode(message)
        self.last_sent = data
        return data, data.decode("ascii")

    def read_typed(self):
        for line in sys.stdin:
            try:
                message = parse_typed_line(line)
            except json.JSONDecodeError as e:
                print("not JSON: %s" % e, flush=True)
                continue
            if message is not None:
                with self.lock:
                    self.typed.append(message)
                print("queued: %s" % encode(message).decode("ascii"), flush=True)

    def connect(self):
        deadline = time.monotonic() + self.args.connect_timeout
        while True:
            try:
                return socket.create_connection((self.args.host, self.args.port), timeout=2)
            except OSError as e:
                if time.monotonic() >= deadline:
                    sys.exit("cannot connect to the emulator on %s:%d: %s"
                             % (self.args.host, self.args.port, e))
                time.sleep(0.1)

    def run(self):
        if self.args.interactive:
            print("Type a message and press return. Plain text scrolls as a marquee;\n"
                  "':F HELLO' sends type F with that text; a line starting with { is sent as is.",
                  flush=True)
            threading.Thread(target=self.read_typed, daemon=True).start()
        sock = self.connect()
        sock.settimeout(None)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.log("connected to %s:%d" % (self.args.host, self.args.port))
        other = bytearray()              # bytes from the sign that are not requests
        while not self.finished:
            # Contract version 3: wait for the sign's next byte only until a push is due.
            due = self.next_push_at()
            wait = None if due is None else max(0.0, due - self.clock())
            if due is not None and (self.args.interactive or wait > 0.05):
                wait = min(wait, 0.05)   # look again soon: something may be typed meanwhile
            if not select.select([sock], [], [], wait)[0]:
                push, shown = self.push_due()
                if push is not None:
                    sock.sendall(push)   # one write: break, wait and message arrive together
                    self.log("pushed unasked  ->  %s" % shown)
                elif self.finished:
                    self.log("the chooser's time is up; playlist finished")
                continue
            data = sock.recv(4096)
            if not data:
                self.log("the emulator closed the link")
                break
            for byte in data:
                if byte not in (REQUEST_NEXT, REQUEST_POLL):
                    other.append(byte)
                    continue
                if other:
                    self.log("sign said: %r" % bytes(other).decode("ascii", "replace"))
                    other.clear()
                writes = self.on_request(byte)
                if writes is None:
                    self.log("sign asked %s; playlist finished" % chr(byte))
                    self.finished = True
                    break
                for reply, shown in writes:
                    sock.sendall(reply)
                    self.log(shown)
        if other:
            self.log("sign said: %r" % bytes(other).decode("ascii", "replace"))
        sock.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("playlist", nargs="?", help="playlist file (JSON lines); loops")
    p.add_argument("-i", "--interactive", action="store_true",
                   help="also send what is typed on stdin (typed messages go first)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=14400)
    p.add_argument("--connect-timeout", type=float, default=15,
                   help="seconds to keep trying to reach the emulator")
    p.add_argument("--once", action="store_true", help="play the playlist once, then hang up")
    p.add_argument("--contract", type=int, choices=(1, 2, 3), default=1,
                   help='chooser contract version: 1 answers a "?" poll with {}, '
                        '2 with {"c":N}, the CRC-16/CCITT-FALSE of the selected chooser message; '
                        '3 answers like 2 and pushes every change of the chooser unasked: the '
                        'line LOW, a wait, the message; on leaving chooser mode the line LOW '
                        'and nothing (needs sign-emu --link-directives)')
    p.add_argument("--chooser-polls", type=int, default=40,
                   help="how long a chooser message stays selected, in polls (default 40)")
    p.add_argument("--chooser-seconds", type=float, default=6,
                   help="--contract 3: how long a chooser message stays selected, in seconds of "
                        "this computer's time (default 6)")
    p.add_argument("--break-ms", type=float, default=50,
                   help="--contract 3: how long a push holds the line LOW (default 50)")
    p.add_argument("--guard-ms", type=float, default=20,
                   help="--contract 3: the wait between the end of the break and the message "
                        "(default 20)")
    p.add_argument("--idle", default='{"type":"S","str":"","stars":20}',
                   help="what to answer when there is no playlist and nothing typed")
    p.add_argument("-q", "--quiet", action="store_true")
    args = p.parse_args()
    if not args.playlist and not args.interactive:
        p.error("give a playlist file, or --interactive, or both")
    if not 0 < args.chooser_seconds <= 86400:
        p.error("--chooser-seconds is more than 0")
    if not 0.001 <= args.break_ms <= 60000 or not 0 <= args.guard_ms <= 60000:
        p.error("--break-ms is 0.001 to 60000 and --guard-ms 0 to 60000")
    try:
        json.loads(args.idle)
    except json.JSONDecodeError as e:
        p.error("--idle is not JSON: %s" % e)
    try:
        App(args).run()
    except KeyboardInterrupt:
        pass
    except OSError as e:
        # The link broke under a read or a write: say so, do not pretend it ended well.
        sys.exit("fake_app: the link to the emulator failed: %s" % e)


if __name__ == "__main__":
    main()
