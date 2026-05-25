#!/usr/bin/env python3
"""Chain IW2OHX-14 -> C IW2OHX-12 -> SYS -> reset IR2UFV link entry -> BYE.

Sequence (per Marco, 2026-05-25):
  L - IR2UFV     -> delete the stale IR2UFV link entry
  L 5 IR2UFV     -> re-add at port 5 (matches current 'P 5' in the table)

After SYS, the link goes back to user prompt; we run both commands, then BYE BYE.
"""
from __future__ import annotations

import re
import socket
import sys
import time


HOST = "44.134.24.2"      # IW2OHX-14
PORT = 23
USER = "iw7eas-1"
PWD  = "sherwood"
SYS_PWD = "SHERWOOD12"    # IW2OHX-12 SYS password (per memory)

SYS_CHALLENGE = re.compile(rb"\S+>\s+(\d+(?:\s+\d+){2,})", re.MULTILINE)


def drain_until(s, needles, timeout=8.0):
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
        else:
            time.sleep(0.1)
    return buf


def drain_idle(s, idle_ms=600, max_wait=8.0):
    """Read until idle_ms passes WITH AT LEAST ONE BYTE in buf, or max_wait."""
    s.settimeout(idle_ms / 1000.0)
    end = time.monotonic() + max_wait
    buf = b""
    while time.monotonic() < end:
        try:
            chunk = s.recv(8192)
        except socket.timeout:
            chunk = b""
        if chunk:
            buf += chunk
            continue
        if buf:
            # Got at least one chunk and now idle for idle_ms — done.
            break
        # No data yet; keep waiting up to max_wait.
    return buf


def resolve_challenge(challenge_buf: bytes, pwd: str) -> str:
    m = SYS_CHALLENGE.search(challenge_buf)
    if not m:
        raise RuntimeError(f"no SYS challenge in: {challenge_buf!r}")
    positions = [int(x) for x in m.group(1).split()]
    out = []
    for p in positions:
        if p < 1 or p > len(pwd):
            raise RuntimeError(
                f"SYS position {p} out of range for password length {len(pwd)}"
            )
        out.append(pwd[p - 1])
    return "".join(out)


def main() -> int:
    s = socket.create_connection((HOST, PORT), timeout=10)
    try:
        # Outer login.
        drain_until(s, [b"Login:", b"login:"], timeout=8)
        s.sendall(USER.encode() + b"\r\n")
        drain_until(s, [b"Password:", b"password:"], timeout=6)
        s.sendall(PWD.encode() + b"\r\n")
        drain_idle(s, idle_ms=800, max_wait=6)

        # Chain to -12.
        s.sendall(b"C IW2OHX-12\r\n")
        buf = drain_until(s, [b"onnected to", b"failure", b"busy", b"unknown"],
                          timeout=20)
        if b"onnected to" not in buf.lower():
            sys.stdout.write(f"!! chain failed: {buf.decode('latin-1','replace')!r}\n")
            return 2
        post_chain = drain_idle(s, idle_ms=800, max_wait=8)
        sys.stdout.write(f"=== inner prompt ===\n{post_chain.decode('latin-1','replace')}\n")

        # SYS elevation.
        s.sendall(b"SYS\r\n")
        challenge = drain_idle(s, idle_ms=1500, max_wait=15)
        sys.stdout.write(f"=== SYS challenge ===\n{challenge.decode('latin-1','replace')}\n")
        reply = resolve_challenge(challenge, SYS_PWD)
        sys.stdout.write(f"=== reply (positions resolved) ===\n{reply}\n")
        s.sendall(reply.encode() + b"\r\n")
        post = drain_idle(s, idle_ms=800, max_wait=6)
        sys.stdout.write(f"=== after SYS reply ===\n{post.decode('latin-1','replace')}\n")

        # Run the two reset commands.
        for cmd in (b"L - IR2UFV\r\n", b"L 5 IR2UFV\r\n", b"L *\r\n"):
            s.sendall(cmd)
            out = drain_idle(s, idle_ms=700, max_wait=8)
            sys.stdout.write(f"=== {cmd!r} ===\n{out.decode('latin-1','replace')}\n")

        # Tear down.
        s.sendall(b"BYE\r\n")
        drain_idle(s, idle_ms=400, max_wait=3)
        s.sendall(b"BYE\r\n")
        drain_idle(s, idle_ms=400, max_wait=3)
    finally:
        try:
            s.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
