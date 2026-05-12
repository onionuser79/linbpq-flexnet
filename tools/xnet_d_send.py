#!/usr/bin/env python3
"""
xnet_d_send.py — connect to an xnet node, elevate to SYS, issue one or more
                 D <target> queries, capture the responses, then quit.

Used in tandem with xnet_agent.py monitor sessions to drive D queries
while traffic capture is running on neighbouring ports.

Usage:
  xnet_d_send.py --host your.node.example.org \
                 --user USERNAME --password LOGINPASS \
                 --syspass SYSPASSWORD \
                 --target TARGET_CALL [--target ANOTHER ...]
"""
import argparse, socket, re, time, sys, os

PROMPT       = b"=>"
RECV_TIMEOUT = 8.0
SETTLE       = 0.5

RE_SYS_PRIMARY = re.compile(
    r"[Pp]assword\s+characters?\s*[:\-]?\s*([\d\s,]+)")
RE_SYS_ALT     = re.compile(
    r"[Cc]har(?:acter)?s?\s+([\d](?:\s*[,\s]\s*[\d])*)")


def now():
    return time.strftime("%H:%M:%S")


def recv_until(sock, marker, timeout=RECV_TIMEOUT):
    buf = b""
    end = time.time() + timeout
    sock.settimeout(0.3)
    while time.time() < end:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            if marker in buf:
                return buf
        except socket.timeout:
            continue
    return buf


def solve_challenge(text, syspass):
    m = RE_SYS_PRIMARY.search(text) or RE_SYS_ALT.search(text)
    if m:
        positions = [int(x) for x in re.findall(r"\d+", m.group(1))]
    else:
        positions = [int(d) for d in re.findall(r"\b(\d+)\b", text)
                     if 1 <= int(d) <= len(syspass)]
    if not positions:
        return None
    return "".join(syspass[p - 1] if 1 <= p <= len(syspass) else "?"
                   for p in positions)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=23)
    ap.add_argument("--user", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--syspass", required=True)
    ap.add_argument("--target", action="append", required=True,
                    help="callsign to query with D (repeatable)")
    ap.add_argument("--per-query-wait", type=float, default=4.0,
                    help="seconds to wait after each D query (default 4)")
    args = ap.parse_args()

    print(f"[{now()}] connecting to {args.host}:{args.port}")
    s = socket.create_connection((args.host, args.port), timeout=15)
    s.settimeout(0.3)

    banner = recv_until(s, b"ogin:", timeout=15)
    print(f"[{now()}] banner OK ({len(banner)}b), sending user={args.user}")
    s.sendall((args.user + "\r").encode())

    resp = recv_until(s, b":", timeout=8)
    print(f"[{now()}] after-user; sending password")
    s.sendall((args.password + "\r").encode())

    resp = recv_until(s, PROMPT, timeout=20)
    if PROMPT not in resp:
        print(f"[{now()}] WARNING: no prompt after login. tail={resp[-200:]!r}")
    else:
        print(f"[{now()}] at node prompt; sending SYS")

    s.sendall(b"SYS\r")
    chal = recv_until(s, b":", timeout=10)
    chal_s = chal.decode("latin-1", errors="replace")
    print(f"[{now()}] SYS challenge: {chal_s.strip()[-200:]!r}")

    answer = solve_challenge(chal_s, args.syspass)
    if not answer:
        print(f"[{now()}] ERROR: could not parse SYS challenge")
        sys.exit(2)
    print(f"[{now()}] sending SYS response ({len(answer)} chars)")
    s.sendall((answer + "\r").encode())

    result = recv_until(s, PROMPT, timeout=10)
    print(f"[{now()}] post-SYS prompt: {result[-100:]!r}")

    for tgt in args.target:
        print(f"\n[{now()}] >>> D {tgt}")
        s.sendall((f"D {tgt}\r").encode())
        time.sleep(args.per_query_wait)
        # Read everything pending
        buf = b""
        s.settimeout(0.3)
        deadline = time.time() + 1.5
        while time.time() < deadline:
            try:
                c = s.recv(4096)
                if not c:
                    break
                buf += c
            except socket.timeout:
                continue
        for ln in buf.decode("latin-1", errors="replace").splitlines():
            if ln.strip():
                print(f"   | {ln}")

    print(f"\n[{now()}] done — quitting")
    try:
        s.sendall(b"q\r")
        time.sleep(0.3)
        s.sendall(b"q\r")
    except Exception:
        pass
    s.close()


if __name__ == "__main__":
    main()
