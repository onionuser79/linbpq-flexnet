#!/usr/bin/env python3
"""
bpq_d_query.py — connect to a LinBPQ telnet port, issue one or more
                 `D <call>` queries, print the responses.

Used to verify FlexNet D-command output end-to-end after a deploy
(originator-role test of CE type-6/7 path discovery).

Usage:
  bpq_d_query.py --host YOUR_BPQ_HOST --port 2323 \
                 --user USERNAME --password PASSWORD \
                 IR5S IR3UGM N2MH-5

Each callsign argument is issued as `D <call>` against the BPQ
telnet listener, with the response printed line-by-line.

The `--reissue-after N` flag re-issues each query N seconds later;
useful when probing a destination that may not yet have a cached
path on the first try (the second query catches the late-arriving
PATH_REP from the round-robin probe).
"""
import socket, time, argparse, sys, re

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True,
                    help="LinBPQ telnet host (e.g. your.node or 192.168.x.x)")
    ap.add_argument("--port", type=int, default=2323,
                    help="LinBPQ telnet port (default 2323)")
    ap.add_argument("--user", required=True,
                    help="BPQ telnet username")
    ap.add_argument("--password", required=True,
                    help="BPQ telnet password")
    ap.add_argument("--gap", type=float, default=8.0,
                    help="seconds between queries")
    ap.add_argument("--reissue-after", type=float, default=6.0,
                    help="re-issue D <target> after this delay "
                         "(0 disables; useful to catch a path that "
                         "arrives shortly after the first query)")
    ap.add_argument("targets", nargs="+",
                    help="callsigns to query")
    args = ap.parse_args()

    print(f"[{time.strftime('%H:%M:%S')}] connecting to "
          f"{args.host}:{args.port}")
    s = socket.create_connection((args.host, args.port), timeout=10)
    s.settimeout(5.0)

    def send(line):
        s.sendall((line + "\r").encode("ascii", errors="replace"))

    def read_until_silence(silence=1.0, max_wait=8.0):
        end = time.time() + max_wait
        buf = b""
        last_data = time.time()
        s.settimeout(0.3)
        while time.time() < end:
            try:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
                last_data = time.time()
            except socket.timeout:
                if time.time() - last_data > silence and buf:
                    break
        return buf.decode("ascii", errors="replace")

    # Login flow — banner, then user, then password.
    banner = read_until_silence(silence=0.8, max_wait=4.0)
    send(args.user)
    read_until_silence(silence=0.8, max_wait=4.0)
    send(args.password)
    read_until_silence(silence=1.0, max_wait=5.0)

    for i, target in enumerate(args.targets):
        print(f"\n[{time.strftime('%H:%M:%S')}] >>> D {target}")
        send(f"D {target}")
        resp = read_until_silence(silence=1.5, max_wait=6.0)
        for ln in resp.splitlines():
            if ln.strip():
                print(f"    | {ln}")
        if args.reissue_after > 0:
            time.sleep(args.reissue_after)
            print(f"[{time.strftime('%H:%M:%S')}] "
                  f">>> D {target} (re-issue)")
            send(f"D {target}")
            resp = read_until_silence(silence=1.5, max_wait=6.0)
            for ln in resp.splitlines():
                if ln.strip():
                    print(f"    | {ln}")
        if i < len(args.targets) - 1:
            time.sleep(args.gap)

    print(f"\n[{time.strftime('%H:%M:%S')}] done — disconnecting")
    try:
        send("B")
        time.sleep(0.5)
    except Exception:
        pass
    s.close()


if __name__ == "__main__":
    main()
