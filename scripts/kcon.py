#!/usr/bin/env python3
"""Send console commands to a Keystone board over USB and print what it says.

Usage:
  scripts/kcon.py                       # interactive REPL on the brain
  scripts/kcon.py "relay" "chips"       # run commands, print the output
  scripts/kcon.py -p /dev/ttyACM2 -- "flash list"
  scripts/kcon.py --watch               # just follow the log
"""
import argparse
import re
import sys
import threading
import time

import serial

ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")


def strip_ansi(buf: bytes) -> str:
    return ANSI.sub(b"", buf).decode("utf-8", "replace")


def drain(s: serial.Serial, seconds: float) -> str:
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            out += chunk
            end = max(end, time.time() + 0.25)
    return strip_ansi(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="*")
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wait", type=float, default=2.0, help="seconds to listen after each command")
    ap.add_argument("--settle", type=float, default=0.6, help="pause before sending a command")
    ap.add_argument("--watch", action="store_true")
    ap.add_argument("--interactive", action="store_true")
    args = ap.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.2)
    # Do not toggle DTR/RTS: on the C6s that would reset the board.
    s.dtr = False
    s.rts = False

    if args.watch or args.interactive:
        stop = threading.Event()

        def follow() -> None:
            while not stop.is_set():
                chunk = s.read(4096)
                if chunk:
                    sys.stdout.write(strip_ansi(chunk))
                    sys.stdout.flush()

        t = threading.Thread(target=follow, daemon=True)
        t.start()
        if args.watch:
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                stop.set()
            return 0
        for line in sys.stdin:
            s.write(line.encode() + b"\r")
            s.flush()
            time.sleep(args.wait)
        stop.set()
        return 0

    # Batch mode: wake the console, then run each command.
    s.write(b"\r")
    s.flush()
    time.sleep(0.4)
    drain(s, 1.0)

    for cmd in args.commands:
        s.write(cmd.encode() + b"\r")
        s.flush()
        print(drain(s, args.wait).strip())
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())