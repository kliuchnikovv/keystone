#!/usr/bin/env python3
"""Send console commands to a Keystone board over USB and print what it says.

Usage:
  scripts/kcon.py                       # interactive REPL on the brain
  scripts/kcon.py "relay" "chips"       # run commands, print the output
  scripts/kcon.py -p /dev/ttyACM2 -- "flash list"
  scripts/kcon.py --watch               # just follow the log
"""
import argparse
import errno
import re
import sys
import threading
import time

import serial

ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")


def strip_ansi(buf: bytes) -> str:
    return ANSI.sub(b"", buf).decode("utf-8", "replace")


class _ConsolePort(serial.Serial):
    """A serial port that never touches the RTS/DTR lines.

    The C6's console is the USB-Serial-JTAG peripheral, and on some kernels
    its driver does not implement the TIOCMBIC/TIOCMBIS ioctls: pyserial then
    fails to *open* the port with EPROTO ("Protocol error"), even though
    reading and writing work fine. The lines are useless to us anyway — a
    pulse on RTS resets the chip — so skip them and open the port anyway.
    """

    def _update_rts_state(self) -> None:
        pass

    def _update_dtr_state(self) -> None:
        pass


def open_console(port: str, baud: int) -> serial.Serial:
    """Open the console, falling back to a port that leaves RTS/DTR alone."""
    try:
        return serial.Serial(port, baud, timeout=0.2)
    except OSError as err:
        if err.errno not in (errno.EPROTO, errno.EINVAL, errno.ENOTTY):
            raise
        print(f"note: {port} does not support the RTS/DTR ioctls ({err}); "
              f"opening it without touching them", file=sys.stderr)
        return _ConsolePort(port, baud, timeout=0.2)


def drain(s: serial.Serial, seconds: float) -> str:
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.read(4096)
        except (serial.SerialException, OSError):
            # The board rebooted (or the cable moved): keep what we got.
            break
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
    ap.add_argument("--reset", action="store_true",
                    help="reset the board first (DTR low resets the S3) and exit")
    ap.add_argument("--follow-seconds", type=float, default=0.0,
                    help="in interactive mode: keep following the log this long after the last command")
    args = ap.parse_args()

    s = open_console(args.port, args.baud)
    # Do not touch DTR/RTS. Deasserting DTR resets the brain (the S3 wires it
    # to EN), and a pulse on RTS resets the C6s - either way the command would
    # be swallowed by the boot log. pyserial's default (both asserted) is what
    # the boards see on a plain terminal, so leave it alone.

    if args.reset:
        # The one exception, asked for explicitly: the S3 wires DTR to EN, so
        # deasserting it resets the brain. The USB device goes with it, so the
        # caller has to look the port up again (by MAC) and follow the boot log.
        try:
            s.dtr = False
            s.flush()
            time.sleep(0.2)
            s.close()
        except Exception:
            pass
        print(f"reset {args.port}")
        return 0

    if args.watch or args.interactive:
        stop = threading.Event()
        port = args.port
        baud = args.baud

        def follow() -> None:
            """Read the console, and survive the chip rebooting.

            A reboot takes the USB device with it: the read fails with
            "device disconnected", the port re-appears a moment later, and the
            interesting part — the boot log — is in the first seconds after
            that. So reopen the port and carry on instead of dying.

            The old handle does not always raise: after the re-enumeration it
            can read "nothing" forever. So reopen on silence as well.
            """
            nonlocal s
            last_data = time.time()
            while not stop.is_set():
                try:
                    chunk = s.read(4096)
                except (serial.SerialException, OSError):
                    if stop.is_set():
                        return
                    time.sleep(0.2)
                    try:
                        s = open_console(port, baud)
                    except (serial.SerialException, OSError):
                        time.sleep(0.5)
                        continue
                    last_data = time.time()
                    continue
                if chunk:
                    last_data = time.time()
                    sys.stdout.write(strip_ansi(chunk))
                    sys.stdout.flush()
                elif time.time() - last_data > 3.0:
                    # The device behind the port changed (a reboot) and the
                    # old handle is a dead end: reopen it.
                    try:
                        s.close()
                    except Exception:
                        pass
                    time.sleep(0.2)
                    try:
                        s = open_console(port, baud)
                    except (serial.SerialException, OSError):
                        time.sleep(0.5)
                    last_data = time.time()

        t = threading.Thread(target=follow, daemon=True)
        t.start()
        if args.watch:
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                stop.set()
            return 0
        deadline = time.time() + args.follow_seconds
        for line in sys.stdin:
            try:
                s.write(line.encode() + b"\r")
                s.flush()
            except (serial.SerialException, OSError):
                pass
            time.sleep(args.wait)
            deadline = time.time() + args.follow_seconds
        # The input is done, but the chip may still be booting after the last
        # command (or a reboot): keep following for a while.
        while time.time() < deadline:
            time.sleep(0.2)
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

    if args.follow_seconds > 0:
        # Keep listening: a command like "reboot" makes the chip disappear
        # from USB and come back, and the boot log is in the first seconds
        # after that. Reopen the port when it goes away.
        deadline = time.time() + args.follow_seconds
        while time.time() < deadline:
            try:
                chunk = s.read(4096)
            except (serial.SerialException, OSError):
                time.sleep(0.2)
                try:
                    s = open_console(args.port, args.baud)
                except (serial.SerialException, OSError):
                    time.sleep(0.5)
                continue
            if chunk:
                sys.stdout.write(strip_ansi(chunk))
                sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())