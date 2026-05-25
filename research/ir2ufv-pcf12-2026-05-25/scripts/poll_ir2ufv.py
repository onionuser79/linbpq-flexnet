#!/usr/bin/env python3
"""Poll IR2UFV's BPQ console every POLL_INTERVAL seconds, run `L *`,
parse FlexNet link rows, and log:
  - one JSONL record per poll to argv[1]
  - one human-readable line per state transition to argv[2]

State transitions tracked per peer:
  - 'reachable -> unreachable'  (cost reaches 4095 / 'inf')
  - 'unreachable -> reachable'  (cost drops back below 4095)
  - any large cost jump (>= 1000 delta) at a finer granularity

Login: USER=Marco, PWD=sherwood (see /home/bpq-ufv/bpq32.cfg).
Talks to BPQ Telnet driver on localhost:2525.
"""
from __future__ import annotations

import json
import re
import socket
import sys
import time
from datetime import datetime, timezone

HOST = "127.0.0.1"
PORT = 2525
USER = "Marco"
PWD = "sherwood"
POLL_INTERVAL = 15.0          # seconds between polls
UNREACHABLE_COST = 4095
LARGE_JUMP = 1000

# Example row formats observed on `=>l *`:
#   IR2UFV  0-8  (2348/2)        49s P 5
#                    600 4095
# We parse two row types:
#   header: <CALL[-SSID]>  <ssidrange>  (<x>/<y>)  <age>  <flag> <port>
#   costs : <leading-spaces>  <cost1> <cost2>...
# But the format depends on linbpq-flexnet build. Below we capture
# anything that looks like a callsign and the trailing 1-2 cost numbers,
# whether they sit on the same physical line or the wrap line below.

CALL_RE = re.compile(r"\b([A-Z0-9]{3,6}(?:-\d{1,2})?)\b")
COSTS_RE = re.compile(r"(\d{2,5})\s+(\d{2,5})\s*$")


def now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def recv_until(sock: socket.socket, needles: list[bytes], timeout: float = 6.0) -> bytes:
    sock.settimeout(timeout)
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        for n in needles:
            if n in buf:
                return buf
    return buf


def fetch_links() -> tuple[str, dict[str, list[int]]]:
    """Connect, login, run `L *`, return (raw_text, {peer: [costs...]})."""
    s = socket.create_connection((HOST, PORT), timeout=10)
    try:
        recv_until(s, [b"login:", b"call:", b"user:"], timeout=4.0)
        s.sendall(USER.encode() + b"\r\n")
        recv_until(s, [b"password:", b"Password:"], timeout=4.0)
        s.sendall(PWD.encode() + b"\r\n")
        # Wait for any prompt. BPQ shows the apps menu or node prompt.
        recv_until(s, [b">", b"=>", b":"], timeout=4.0)
        # Force the FlexNet node command. If we landed on the apps menu,
        # 'BPQ' enters the node; if we're already at node, it's a harmless echo.
        s.sendall(b"BPQ\r\n")
        recv_until(s, [b">", b"=>"], timeout=2.0)
        s.sendall(b"L *\r\n")
        time.sleep(1.0)
        # Drain everything for ~1.5s after the command.
        s.settimeout(1.5)
        buf = b""
        deadline = time.monotonic() + 2.5
        while time.monotonic() < deadline:
            try:
                chunk = s.recv(8192)
                if not chunk:
                    break
                buf += chunk
            except socket.timeout:
                break
        s.sendall(b"B\r\n")  # bye
        raw = buf.decode("latin-1", errors="replace")
    finally:
        try:
            s.close()
        except Exception:
            pass

    peers = parse_links(raw)
    return raw, peers


def parse_links(raw: str) -> dict[str, list[int]]:
    """Extract peer -> [cost...] mapping from `L *` output.

    Logic: walk lines, remember the most recent callsign seen on a line that
    also looks like a link-table header. If the next non-empty line is a costs
    line, attach the costs to that callsign. Also handles single-line rows
    where costs sit on the header itself.
    """
    out: dict[str, list[int]] = {}
    last_call: str | None = None
    for line in raw.splitlines():
        m_costs = COSTS_RE.search(line)
        m_call = CALL_RE.search(line)
        if m_call and "(" in line:
            last_call = m_call.group(1)
            if m_costs:
                out[last_call] = [int(m_costs.group(1)), int(m_costs.group(2))]
                last_call = None
        elif m_costs and last_call is not None:
            out[last_call] = [int(m_costs.group(1)), int(m_costs.group(2))]
            last_call = None
    return out


def classify(prev: list[int] | None, cur: list[int]) -> str | None:
    cur_unreach = UNREACHABLE_COST in cur
    if prev is None:
        return "first-seen-unreachable" if cur_unreach else None
    prev_unreach = UNREACHABLE_COST in prev
    if cur_unreach and not prev_unreach:
        return "reachable->unreachable"
    if prev_unreach and not cur_unreach:
        return "unreachable->reachable"
    # Large cost jump (excluding 4095 transitions, already covered).
    if not cur_unreach and not prev_unreach:
        if abs(cur[0] - prev[0]) >= LARGE_JUMP or abs(cur[1] - prev[1]) >= LARGE_JUMP:
            return f"cost-jump {prev}->{cur}"
    return None


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: poll_ir2ufv.py <jsonl-out> <events-out>", file=sys.stderr)
        return 2
    jsonl_path, events_path = sys.argv[1], sys.argv[2]
    last: dict[str, list[int]] = {}
    poll_n = 0
    while True:
        poll_n += 1
        t_start = time.monotonic()
        try:
            raw, peers = fetch_links()
            rec = {
                "ts": now_iso(),
                "poll": poll_n,
                "peers": peers,
                "raw_len": len(raw),
                "ok": True,
            }
        except Exception as exc:
            rec = {
                "ts": now_iso(),
                "poll": poll_n,
                "peers": {},
                "ok": False,
                "error": repr(exc),
            }
            peers = {}

        with open(jsonl_path, "a", buffering=1) as f:
            f.write(json.dumps(rec) + "\n")

        for peer, cost in peers.items():
            kind = classify(last.get(peer), cost)
            if kind:
                line = f"{rec['ts']} poll={poll_n} peer={peer} event={kind} cost={cost}"
                with open(events_path, "a", buffering=1) as e:
                    e.write(line + "\n")
        last = peers

        elapsed = time.monotonic() - t_start
        time.sleep(max(0.0, POLL_INTERVAL - elapsed))


if __name__ == "__main__":
    sys.exit(main())
