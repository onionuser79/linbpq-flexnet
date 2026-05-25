#!/usr/bin/env python3
"""Chain via IW2OHX-14 → C IW2OHX-12 → L * → BYE BYE.

Runs on iw2ohx-gw. Prints the inner-node transcript to stdout.
"""
from __future__ import annotations

import socket
import sys
import time


HOST = "44.134.24.2"   # IW2OHX-14
PORT = 23
USER = "iw7eas-1"
PWD  = "sherwood"


def drain_until(s, needles, timeout=8.0):
    """Read bytes until any needle (bytes) is seen or timeout."""
    s.settimeout(0.5)
    end = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < end:
        try:
            chunk = s.recv(8192)
        except socket.timeout:
            chunk = b""
        if chunk:
            buf += chunk
            for n in needles:
                if n in buf:
                    return buf
        elif chunk == b"":
            # No data; small sleep then continue.
            time.sleep(0.1)
    return buf


def drain_idle(s, idle_ms=600, max_wait=8.0):
    """Read until idle_ms passes with no data, or max_wait."""
    s.settimeout(idle_ms / 1000.0)
    end = time.monotonic() + max_wait
    buf = b""
    while time.monotonic() < end:
        try:
            chunk = s.recv(8192)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    return buf


def main() -> int:
    s = socket.create_connection((HOST, PORT), timeout=10)
    try:
        sys.stdout.write("=== outer banner ===\n")
        b = drain_until(s, [b"Login:", b"login:", b"call:"], timeout=8)
        sys.stdout.write(b.decode("latin-1", "replace"))

        s.sendall(USER.encode() + b"\r\n")
        b = drain_until(s, [b"Password:", b"password:"], timeout=6)
        sys.stdout.write("=== after user ===\n")
        sys.stdout.write(b.decode("latin-1", "replace"))

        s.sendall(PWD.encode() + b"\r\n")
        # outer prints MOTD + user prompt
        b = drain_idle(s, idle_ms=800, max_wait=6)
        sys.stdout.write("=== outer prompt ===\n")
        sys.stdout.write(b.decode("latin-1", "replace"))

        s.sendall(b"C IW2OHX-12\r\n")
        # Wait for "*** CONNECTED" or failure.
        b = drain_until(
            s,
            [b"CONNECTED to", b"Connected to", b"failure", b"Failure",
             b"busy", b"Busy", b"retried out", b"no route", b"unknown"],
            timeout=20,
        )
        sys.stdout.write("=== connect attempt ===\n")
        sys.stdout.write(b.decode("latin-1", "replace"))
        if b"onnected to" not in b.lower():
            sys.stdout.write("\n!! chain failed before reaching -12\n")
            return 2

        # Drain any inner MOTD.
        b = drain_idle(s, idle_ms=600, max_wait=5)
        sys.stdout.write(b.decode("latin-1", "replace"))

        # Run L * (Marco: use 'L *', not 'L', on PC/Flexnet).
        s.sendall(b"L *\r\n")
        b = drain_idle(s, idle_ms=800, max_wait=10)
        sys.stdout.write("=== L * output ===\n")
        sys.stdout.write(b.decode("latin-1", "replace"))

        # Tear down: inner BYE first, then outer BYE.
        s.sendall(b"BYE\r\n")
        b = drain_idle(s, idle_ms=500, max_wait=4)
        sys.stdout.write("=== after inner BYE ===\n")
        sys.stdout.write(b.decode("latin-1", "replace"))

        s.sendall(b"BYE\r\n")
        b = drain_idle(s, idle_ms=400, max_wait=3)
        sys.stdout.write("=== after outer BYE ===\n")
        sys.stdout.write(b.decode("latin-1", "replace"))
    finally:
        try:
            s.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
