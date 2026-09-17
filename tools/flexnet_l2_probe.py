#!/usr/bin/env python3
"""
flexnet_l2_probe.py — observe FlexNet L2/L3 forwarding on BOTH sides of a
transit node, time-correlated, around a triggered connect.

FlexNet's forwarding behaviour is undocumented (flexnetd/PROTOCOL_SPEC.md
§5.1 records only the two *legal* patterns and why chain-extension is
illegal, not what the real routers actually do). The only ground truth is
a native transit: PC/Flexnet IW2OHX-12 already forwards for IW2OHX-4 <->
IW2OHX-14, so watching -4's link to -12 and -14's link from -12 at the
same instant shows a real transit node's input and output.

Why a new tool rather than xnet_agent.py: that one is a single-node,
hour-long soak. What the question needs is SEVERAL nodes monitored
concurrently against one clock, with a connect fired at a known moment
inside the window, so cause and effect can be lined up. Every line is
stamped by THIS host as it arrives, so all streams share a clock even
though the nodes' own clocks do not.

Run it from iw2ohx-gw (only it reaches the HAMNET xnet hosts).

    ./flexnet_l2_probe.py --scenario native-transit --target IGATE
    ./flexnet_l2_probe.py --watch IW2OHX-14:1 --watch IW2OHX-4:3 \
        --connect-from IW2OHX-4 --target VE3TOK --seconds 60

Output: <out>.jsonl  one stamped record per line, all streams merged
        <out>.txt    human-readable interleaved transcript
"""

import argparse
import json
import os
import re
import socket
import sys
import threading
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xnet_agent import XnetSession, authenticate            # noqa: E402

# Node access. Passwords come from the environment or --pw so this file
# stays publishable; FLEXNET_PROBE_PW is the shared login/sys password
# used across Marco's xnet nodes.
NODES = {
    "IW2OHX-14": {"host": "44.134.24.2",  "port": 23, "user": "iw7eas-1"},
    "IW2OHX-4":  {"host": "44.134.24.3",  "port": 23, "user": "iw7eas-2"},
    "IR2UFV":    {"host": "127.0.0.1",    "port": 2525, "user": "Pino",
                  "bpq": True},
    "IR2UFX":    {"host": "127.0.0.1",    "port": 2727, "user": "Marco",
                  "bpq": True},
}

STOP = threading.Event()
LOCK = threading.Lock()
RECORDS = []


def now_iso():
    return datetime.now(timezone.utc).strftime("%H:%M:%S.%f")[:-3]


def emit(stream, kind, text):
    with LOCK:
        RECORDS.append({"t": time.time(), "ts": now_iso(),
                        "stream": stream, "kind": kind, "text": text})


def bpq_login(sock_file_pair, user, pw):
    """LinBPQ telnet: callsign/password prompts, no SYS challenge."""
    sess, _ = sock_file_pair
    time.sleep(1.5)
    sess.send(user)
    time.sleep(1.5)
    sess.send(pw)
    time.sleep(1.5)


def open_session(name, pw, debug=False):
    cfg = NODES[name]
    sess = XnetSession(cfg["host"], cfg["port"])
    sess.connect()
    if cfg.get("bpq"):
        time.sleep(1.5); sess.send(cfg["user"])
        time.sleep(1.5); sess.send(pw)
        time.sleep(1.5)
    else:
        authenticate(sess, cfg["user"], pw, pw.upper(), debug=debug)
    return sess


def monitor_thread(name, spec, pw, mon_flags, debug):
    """Hold a monitor session open and stamp every line that arrives."""
    try:
        sess = open_session(name, pw, debug)
    except Exception as exc:                       # noqa: BLE001
        emit(name, "error", f"session failed: {exc}")
        return

    cmd = f"MONITOR {mon_flags} {spec}".strip() if mon_flags else \
          f"MONITOR {spec}"
    emit(name, "cmd", cmd)
    sess.send(cmd)

    pending = ""
    while not STOP.is_set():
        try:
            chunk = sess.read_monitor_chunk(timeout=0.2)
        except Exception as exc:                   # noqa: BLE001
            emit(name, "error", f"read failed: {exc}")
            break
        if not chunk:
            continue
        pending += chunk
        while "\n" in pending:
            line, pending = pending.split("\n", 1)
            line = line.replace("\r", "").rstrip()
            if line:
                emit(name, "mon", line)

    try:
        sess.send("")           # any key leaves monitor mode
        time.sleep(0.4)
        sess.send("B")
        sess.close()
    except Exception:           # noqa: BLE001
        pass


def connect_thread(name, target, pw, delay, hold, digi, debug):
    """Fire the connect inside the monitor window, then tear it down."""
    time.sleep(delay)
    try:
        sess = open_session(name, pw, debug)
    except Exception as exc:                       # noqa: BLE001
        emit(f"{name}/actor", "error", f"session failed: {exc}")
        return

    cmd = f"C {target} {digi}".strip() if digi else f"C {target}"
    emit(f"{name}/actor", "cmd", cmd)
    sess.send(cmd)

    deadline = time.time() + hold
    pending = ""
    while time.time() < deadline and not STOP.is_set():
        try:
            chunk = sess.read_monitor_chunk(timeout=0.2)
        except Exception:                          # noqa: BLE001
            break
        if not chunk:
            continue
        pending += chunk
        while "\n" in pending:
            line, pending = pending.split("\n", 1)
            line = line.replace("\r", "").rstrip()
            if line:
                emit(f"{name}/actor", "conn", line)

    for teardown in ("\x1a", "B", "B"):            # ^Z then bye, twice
        try:
            sess.send(teardown)
            time.sleep(1.0)
        except Exception:                          # noqa: BLE001
            break
    try:
        sess.close()
    except Exception:                              # noqa: BLE001
        pass


SCENARIOS = {
    # A real PC/Flexnet transit: -4 reaches everything beyond -14 THROUGH
    # -12, so -4's link to -12 is the transit node's input and -14's link
    # from -12 is its output. Neither side is ours, so whatever appears is
    # how FlexNet actually forwards.
    "native-transit": {
        "watch": ["IW2OHX-4:3", "IW2OHX-14:1"],
        "connect_from": "IW2OHX-4",
        "note": "PC/Flexnet IW2OHX-12 as the transit node (ground truth)",
    },
    # Us as the transit node, same observation points plus our own trace.
    "our-transit": {
        "watch": ["IW2OHX-4:4", "IW2OHX-14:14"],
        "connect_from": "IW2OHX-4",
        "note": "IR2UFV as the transit node (compare against native)",
    },
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", choices=sorted(SCENARIOS))
    ap.add_argument("--watch", action="append", default=[],
                    metavar="NODE:PORT",
                    help="monitor NODE's port PORT (repeatable)")
    ap.add_argument("--connect-from", metavar="NODE")
    ap.add_argument("--target", required=True, help="destination callsign")
    ap.add_argument("--digi", default="", help="explicit digi for the connect")
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--connect-at", type=int, default=8,
                    help="seconds into the window to fire the connect")
    ap.add_argument("--hold", type=int, default=20)
    ap.add_argument("--mon-flags", default="",
                    help="flags before the port, e.g. '-x' (see notes)")
    ap.add_argument("--pw", default=os.environ.get("FLEXNET_PROBE_PW", ""))
    ap.add_argument("--out", default="")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

    if not args.pw:
        sys.exit("need --pw or FLEXNET_PROBE_PW")

    watch = list(args.watch)
    cfrom = args.connect_from
    if args.scenario:
        sc = SCENARIOS[args.scenario]
        watch = watch or sc["watch"]
        cfrom = cfrom or sc["connect_from"]
        print(f">> scenario {args.scenario}: {sc['note']}")

    if not watch:
        sys.exit("nothing to watch — pass --watch or --scenario")

    out = args.out or f"/tmp/l2probe-{args.target.replace('-', '')}-" \
                      f"{datetime.now().strftime('%H%M%S')}"

    print(f">> watching: {', '.join(watch)}")
    print(f">> connect:  {cfrom} -> C {args.target} {args.digi}".rstrip())
    print(f">> window:   {args.seconds}s, connect at +{args.connect_at}s")
    print(f">> out:      {out}.jsonl / {out}.txt")

    threads = []
    for w in watch:
        node, _, spec = w.partition(":")
        if node not in NODES:
            sys.exit(f"unknown node {node}")
        t = threading.Thread(target=monitor_thread,
                             args=(node, spec or "*", args.pw,
                                   args.mon_flags, args.debug),
                             daemon=True)
        t.start(); threads.append(t)

    if cfrom:
        t = threading.Thread(target=connect_thread,
                             args=(cfrom, args.target, args.pw,
                                   args.connect_at, args.hold, args.digi,
                                   args.debug),
                             daemon=True)
        t.start(); threads.append(t)

    try:
        time.sleep(args.seconds)
    except KeyboardInterrupt:
        pass
    STOP.set()
    for t in threads:
        t.join(timeout=8)

    with LOCK:
        recs = sorted(RECORDS, key=lambda r: r["t"])

    with open(f"{out}.jsonl", "w") as fh:
        for r in recs:
            fh.write(json.dumps(r) + "\n")

    width = max((len(r["stream"]) for r in recs), default=10)
    with open(f"{out}.txt", "w") as fh:
        for r in recs:
            fh.write(f"{r['ts']}  {r['stream']:<{width}}  "
                     f"{r['kind']:<5} {r['text']}\n")

    print(f"\n>> {len(recs)} records")
    per = {}
    for r in recs:
        per[r["stream"]] = per.get(r["stream"], 0) + 1
    for k in sorted(per):
        print(f"   {k:<{width}} {per[k]}")
    print(f"\n>> transcript: {out}.txt")


if __name__ == "__main__":
    main()
