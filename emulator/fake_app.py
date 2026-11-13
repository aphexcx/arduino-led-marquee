#!/usr/bin/env python3
"""Plays the Android app's side of the sign's serial link.

The sign PULLS: it sends one request byte and reads one JSON object back.
  ~   "send the next message"
  ?   "chooser poll": while a chooser message is selected, answered with
      {} (--contract 1, the default) or with {"c":N}, the checksum of the
      selected chooser message (--contract 2); otherwise like ~

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

Python 3 standard library only.
"""
import argparse
import json
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
            messages.append(obj)
    if not messages:
        sys.exit("%s: no messages" % path)
    return messages


def encode(message):
    """The bytes that go on the wire: compact JSON, no newline, like the app's gson."""
    wire = {k: v for k, v in message.items() if not k.startswith("_")}
    return json.dumps(wire, separators=(",", ":"), ensure_ascii=True).encode("ascii")


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
        self.chooser = None              # contract 2: the selected chooser message
        self.chooser_left = 0            # contract 2: requests it stays selected for
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

    def answer(self, request):
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
                reply, shown = self.answer(byte)
                if reply is None:
                    self.log("sign asked %s; playlist finished" % chr(byte))
                    break
                sock.sendall(reply)
                self.log("sign asked %s  ->  %s" % (chr(byte), shown))
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
    p.add_argument("--contract", type=int, choices=(1, 2), default=1,
                   help='chooser contract version: 1 answers a "?" poll with {}, '
                        '2 with {"c":N}, the CRC-16/CCITT-FALSE of the selected chooser message')
    p.add_argument("--chooser-polls", type=int, default=40,
                   help="how long a chooser message stays selected, in polls (default 40)")
    p.add_argument("--idle", default='{"type":"S","str":"","stars":20}',
                   help="what to answer when there is no playlist and nothing typed")
    p.add_argument("-q", "--quiet", action="store_true")
    args = p.parse_args()
    if not args.playlist and not args.interactive:
        p.error("give a playlist file, or --interactive, or both")
    try:
        json.loads(args.idle)
    except json.JSONDecodeError as e:
        p.error("--idle is not JSON: %s" % e)
    try:
        App(args).run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
