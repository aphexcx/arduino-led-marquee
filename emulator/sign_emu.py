#!/usr/bin/env python3
"""The one way a tool or a test here starts sign-emu, connects to it and ends it.

A TOOL OR A TEST TALKS ONLY TO THE EMULATOR IT STARTED AND ENDS ONLY THE PROCESS IT STARTED.

  Emu(...)   starts the emulator and waits for ITS OWN line that it listens on the port (it writes
             "sign-emu: waiting for an app on tcp 127.0.0.1:<port>" once it has the port; the
             command must have --wait-client). When another program has the port, ours cannot
             listen and ends: Emu() raises, and nothing was connected to anybody.
  connect()  only after that, so whoever answers on the port is the process started here.
  end()      ends that process, by the number it was started with, and gives its status. Nothing
             looks for a process by its name or its command line.

From a shell script (test.sh), an emulator and the app that plays to it:

    sign_emu.py <port> <log file> <sign-emu> [its options and the image] -- <the app's command>

The emulator's messages go to the log file. The app is started when the emulator listens. The
status is the emulator's; APP_FAILED when the app's command failed (the emulator is then ended and
its log shown); 1 when the emulator did not come to listen.
Python 3 standard library only.
"""
import socket
import subprocess
import sys
import time

LISTENS = "sign-emu: waiting for an app on tcp 127.0.0.1:%d "
APP_FAILED = 90


class Emu:
    def __init__(self, command, port, log=None, said=None, wait_s=30, **how):
        """`command`: sign-emu, its options (--port <port> and --wait-client among them) and the
        image. Its messages go to the file `log`; or, when `how` (more arguments for Popen) sends
        them somewhere else, `said` is a function that gives all it has written so far, as bytes."""
        self.port, self.log = port, log
        if log is not None:
            with open(log, "wb") as messages:
                self.proc = subprocess.Popen(command, stderr=messages, **how)
            said = self.said
        else:
            self.proc = subprocess.Popen(command, **how)
        listens, deadline = (LISTENS % port).encode("ascii"), time.monotonic() + wait_s
        while listens not in said():
            if self.proc.poll() is not None or time.monotonic() > deadline:
                if listens in said():                # (it said so and ended at once)
                    break
                self.end()
                raise RuntimeError("sign-emu did not come to listen on port %d: %s"
                                   % (port, said().decode("utf-8", "replace").strip()[-300:] or "it said nothing"))
            time.sleep(0.02)

    def said(self):
        with open(self.log, "rb") as messages:
            return messages.read()

    def connect(self):
        try:
            sock = socket.create_connection(("127.0.0.1", self.port), timeout=2)
        except OSError as e:
            raise RuntimeError("cannot connect to sign-emu on port %d: %s" % (self.port, e))
        sock.settimeout(None)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock

    def poll(self):
        return self.proc.poll()

    def wait(self, timeout=None):
        return self.proc.wait(timeout)

    @property
    def returncode(self):
        return self.proc.returncode

    def end(self):
        """Ends the process started here if it still runs. Its status (negative: it was ended here)."""
        if self.proc.poll() is None:
            self.proc.kill()
        return self.proc.wait()


def main():
    if len(sys.argv) < 6 or "--" not in sys.argv[4:]:
        sys.exit(__doc__)
    port, log, rest = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
    try:
        emu = Emu(rest[:rest.index("--")], port, log=log)
    except RuntimeError as e:
        sys.exit("sign_emu: %s" % e)
    try:
        if subprocess.run(rest[rest.index("--") + 1:]).returncode != 0:
            emu.end()
            sys.stdout.write(emu.said().decode("utf-8", "replace"))
            sys.exit(APP_FAILED)
        status = emu.wait()
    finally:
        emu.end()
    sys.exit(status if status >= 0 else 1)


if __name__ == "__main__":
    main()
