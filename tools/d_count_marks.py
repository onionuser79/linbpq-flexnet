#!/usr/bin/env python3
"""
d_count_marks.py — connect to LinBPQ telnet, run `D *`, count how many
                   FlexNet destinations have a resolved path (! marker).

Used to track how the in-memory path cache fills up after a restart.
With the default 60 s round-robin probe interval, expect roughly
1 mark per minute up to a coverage plateau equal to the count of
reachable FlexNet destinations.

Usage:
  d_count_marks.py --host YOUR_BPQ_HOST --port 2323 \
                   --user USERNAME --password PASSWORD
"""
import socket, time, re, argparse

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True,
                    help="LinBPQ telnet host")
    ap.add_argument("--port", type=int, default=2323,
                    help="LinBPQ telnet port (default 2323)")
    ap.add_argument("--user", required=True,
                    help="BPQ telnet username")
    ap.add_argument("--password", required=True,
                    help="BPQ telnet password")
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port), timeout=10)
    s.settimeout(3.0)

    def send(x):
        s.sendall((x + "\r").encode())

    def rd(silence=1.2, max_wait=8.0):
        end = time.time() + max_wait
        buf = b""
        last = time.time()
        s.settimeout(0.3)
        while time.time() < end:
            try:
                c = s.recv(4096)
                if not c:
                    break
                buf += c
                last = time.time()
            except socket.timeout:
                if time.time() - last > silence and buf:
                    break
        return buf.decode("ascii", errors="replace")

    rd()
    send(args.user); rd()
    send(args.password); rd()

    send("D *")
    out = rd(silence=2.0, max_wait=15.0)

    # Each destination row: "<call>  <ssid-range>  <rtt>  [!]"
    rows = []
    for ln in out.splitlines():
        m = re.match(
            r"^([A-Z0-9]+(?:-[0-9]+)?)\s+(\d+-\d+)\s+(\d+)\s*(!?)\s*$",
            ln.strip(),
        )
        if m:
            rows.append((m.group(1), m.group(2),
                         int(m.group(3)), m.group(4) == "!"))

    marked   = sum(1 for r in rows if r[3])
    unmarked = sum(1 for r in rows if not r[3])
    total    = len(rows)
    pct      = (100.0 * marked / total) if total else 0.0
    print(f"{time.strftime('%H:%M:%S')}  total={total}  marked={marked}  "
          f"unmarked={unmarked}  ({pct:.1f}% covered)")

    unm = [r for r in rows if not r[3]]
    if unm:
        sample = ", ".join(f"{r[0]} ({r[1]})" for r in unm[:8])
        print(f"  next unmarked (first 8): {sample}")

    try:
        send("B")
        time.sleep(0.3)
    except Exception:
        pass
    s.close()


if __name__ == "__main__":
    main()
