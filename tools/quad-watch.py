#!/usr/bin/env python3
"""
quad-watch.py — periodic four-node FlexNet consistency watch.

Watches the four nodes that form the IR2UFV transit triangle and
cross-checks what each of them believes about the others:

    IR2UFV      linbpq-flexnet, the transit node under test (BPQ telnet)
    IW2OHX-14   (X)Net, our route source  (xnet telnet)
    IW2OHX-4    (X)Net, our route sink    (xnet telnet)
    IW2OHX-12   PC/Flexnet, reached by chaining `C IW2OHX-12` from -14

Why this tool and not pcf-watch.sh: that one samples ONE side (IR2UFV's
own counters). A distance-vector inconsistency is by definition a
DISAGREEMENT between two tables, so it is invisible from either end
alone. Every sample here reads all four tables within a few seconds of
each other and diffs them against each other and against the previous
sample.

Read-only: it issues only `FL` / `L` / `C` / `B`. It never writes config
or routing. The chained hop to -12 crosses the live mesh, so the default
interval is deliberately long.

Output (all under --out, default /tmp/quad-watch):
    samples.jsonl   one JSON object per sample: parsed tables + alerts
    watch.log       human-readable transcript of every sample
    alerts.log      ONLY the flagged inconsistencies — read this first

Credentials come from the environment so this file stays publishable:
    QW_PW_XNET / QW_SYS_XNET     login + SYS password for -14 / -4 / -12
    QW_USER_14 / QW_USER_4       xnet usernames
    QW_PW_UFV  / QW_USER_UFV     IR2UFV (BPQ) login

Usage (from iw2ohx-gw — only it reaches the xnet hosts):
    ./quad-watch.py --interval 900
    ./quad-watch.py --interval 600 --iterations 4 --skip-pcf
"""

import argparse
import json
import os
import re
import socket
import sys
import time
from datetime import datetime, timezone

RECV_TIMEOUT = 12.0
RECV_BUF = 8192

# Node access. Hosts are fixed by the station topology; credentials are not
# in this file (see module docstring).
XNET_14 = ("44.134.24.2", 23)
XNET_4 = ("44.134.24.3", 23)
UFV = ("127.0.0.1", 2525)

# A peer's dest-count for a route can legitimately lag ours by a sample or
# two: our advertisement is rate-limited by the token bucket and theirs is
# installed asynchronously. Only a persistent gap wider than this is a
# real inconsistency.
ADVERT_TOLERANCE = 3
SPLIT_HORIZON_MAX = 8
RTT0_BURST = 50


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")


def strip_iac(data):
    """Remove telnet IAC negotiation sequences from a raw byte string."""
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 0xFF and i + 2 < len(data):
            i += 3
            continue
        out.append(data[i])
        i += 1
    return bytes(out)


class Tel:
    """Minimal line-oriented telnet client good enough for node CLIs."""

    def __init__(self, host, port):
        self.host, self.port = host, port
        self.sock = None

    def open(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=20)
        self.sock.settimeout(RECV_TIMEOUT)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def send(self, line):
        self.sock.sendall((line + "\r").encode("latin-1", errors="replace"))

    def drain(self, seconds=2.0):
        """Collect everything that arrives within `seconds`."""
        deadline = time.time() + seconds
        buf = b""
        while time.time() < deadline:
            self.sock.settimeout(max(0.2, deadline - time.time()))
            try:
                chunk = self.sock.recv(RECV_BUF)
            except socket.timeout:
                break
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
        return strip_iac(buf).decode("latin-1", errors="replace").replace("\r", "")


# ---------------------------------------------------------------- parsers

# IR2UFV `FL` link row:
#   IW2OHX-14    2     CONNECTED  0.0s   6      00:16:51    107
FL_LINK = re.compile(
    r"^(?P<call>[A-Z0-9]+(?:-\d+)?)\s+(?P<port>\d+)\s+(?P<status>[A-Z]+)\s+"
    r"(?P<lt>[\d.]+s)\s+(?P<ka>\d+)\s+(?P<up>\d+:\d\d:\d\d)\s+(?P<routes>\d+)"
)
# IR2UFV `FL` peer-table row:
#   IW2OHX-14    xnet        109       1       3       0    2.00
FL_PEER = re.compile(
    r"^(?P<call>[A-Z0-9]+(?:-\d+)?)\s+(?P<family>xnet|PCF|BPQ)\s+(?P<learned>\d+)\s+"
    r"(?P<direct>\d+)\s+(?P<advert>\d+)\s+(?P<queued>\d+)\s+(?P<tokens>[\d.]+)"
)
FL_L2 = re.compile(
    r"L2 forwarding (?P<state>ON|OFF).*?extended=(?P<ext>\d+)\s+"
    r"contracted=(?P<con>\d+)\s+declined=(?P<dec>\d+)"
)
FL_TRANSIT = re.compile(r"FlexNetTransit \((?P<cfg>[^)]*)\).*?rtt0-skips=(?P<rtt0>\d+)",
                        re.IGNORECASE)

# (X)Net `L` row, FlexNet-flagged:
#   4:IR2UFV     109 F   3
# The flag column is unlabeled: Q=NETROM, I=INP3, F=FLEXNET.
XNET_L = re.compile(
    r"^\s*(?P<idx>\d+):(?P<call>[A-Z0-9]+(?:-\d+)?)\s+(?P<dests>\d+)\s+"
    r"(?P<flag>[A-Z])\s+(?P<cost>\d+)"
)


def parse_ufv(text):
    links, peers = {}, {}
    l2, transit = None, None
    for raw in text.splitlines():
        line = raw.strip()
        m = FL_LINK.match(line)
        if m and m.group("status") in ("CONNECTED", "PENDING", "DISCONNECTED"):
            links[m.group("call")] = {
                "port": int(m.group("port")),
                "status": m.group("status"),
                "lt": m.group("lt"),
                "ka": int(m.group("ka")),
                "uptime": m.group("up"),
                "routes": int(m.group("routes")),
            }
            continue
        m = FL_PEER.match(line)
        if m:
            peers[m.group("call")] = {
                "family": m.group("family"),
                "learned": int(m.group("learned")),
                "direct": int(m.group("direct")),
                "advert": int(m.group("advert")),
                "queued": int(m.group("queued")),
                "tokens": float(m.group("tokens")),
            }
            continue
        m = FL_L2.search(line)
        if m:
            l2 = {"state": m.group("state"), "extended": int(m.group("ext")),
                  "contracted": int(m.group("con")), "declined": int(m.group("dec"))}
            continue
        m = FL_TRANSIT.search(line)
        if m:
            transit = {"cfg": m.group("cfg"), "rtt0_skips": int(m.group("rtt0"))}
    return {"links": links, "peers": peers, "l2": l2, "transit": transit}


def parse_xnet_l(text):
    """Return {callsign: {dests, flag, cost}} for every link row."""
    rows = {}
    for raw in text.splitlines():
        m = XNET_L.match(raw.rstrip())
        if m:
            rows[m.group("call")] = {
                "dests": int(m.group("dests")),
                "flag": m.group("flag"),
                "cost": int(m.group("cost")),
            }
    return rows


def uptime_secs(s):
    try:
        h, m, sec = (int(x) for x in s.split(":"))
        return h * 3600 + m * 60 + sec
    except (ValueError, AttributeError):
        return None


# ---------------------------------------------------------------- samplers

def sample_ufv(user, pw):
    t = Tel(*UFV)
    t.open()
    try:
        t.drain(1.5)
        t.send(user)
        t.drain(1.5)
        t.send(pw)
        t.drain(1.5)
        t.send("FL")
        text = t.drain(5.0)
        t.send("B")
        t.drain(0.8)
    finally:
        t.close()
    return text


def xnet_login(t, user, pw, syspw):
    t.drain(2.0)
    t.send(user)
    t.drain(1.5)
    t.send(pw)
    banner = t.drain(2.5)
    # SYS elevation is NOT needed for `L`; skipped deliberately so this
    # watcher never holds a privileged session open for hours.
    return banner


def sample_xnet(host_port, user, pw, syspw, chain_to=None):
    t = Tel(*host_port)
    t.open()
    try:
        xnet_login(t, user, pw, syspw)
        if chain_to:
            t.send("C " + chain_to)
            # A chained AX.25/FlexNet connect needs time for link setup.
            t.drain(12.0)
        t.send("L")
        text = t.drain(6.0)
        if chain_to:
            t.send("B")
            t.drain(2.0)
        t.send("B")
        t.drain(0.8)
    finally:
        t.close()
    return text


# ---------------------------------------------------------------- checks

def check(sample, prev):
    """Cross-check the four tables. Returns a list of alert strings."""
    alerts = []
    ufv = sample.get("ufv") or {}
    peers = ufv.get("peers") or {}
    links = ufv.get("links") or {}
    tables = {"IW2OHX-14": sample.get("n14"), "IW2OHX-4": sample.get("n4"),
              "IW2OHX-12": sample.get("n12")}

    # A9 — a peer that is not CONNECTED invalidates everything else about it.
    for call, lk in links.items():
        if lk["status"] != "CONNECTED":
            alerts.append(f"A9 PEER_NOT_CONNECTED {call} status={lk['status']}")

    for call, p in peers.items():
        remote = tables.get(call)

        # A1 — what we advertise to a peer vs what that peer installed via us.
        if remote is not None:
            ours = None
            for rcall, row in remote.items():
                if rcall.upper().startswith("IR2UFV"):
                    ours = row
                    break
            if ours is None:
                if p["advert"] > ADVERT_TOLERANCE:
                    alerts.append(
                        f"A10 ORPHAN_ADVERT {call}: we advertise {p['advert']} "
                        f"but {call} has no IR2UFV row at all")
            else:
                diff = abs(ours["dests"] - p["advert"])
                if diff > ADVERT_TOLERANCE:
                    alerts.append(
                        f"A1 ADVERT_MISMATCH {call}: we advertise {p['advert']}, "
                        f"{call} installed {ours['dests']} via IR2UFV (diff {diff})")
                if ours["flag"] != "F":
                    alerts.append(
                        f"A11 NOT_FLEXNET {call}: IR2UFV row flag={ours['flag']} "
                        "(expected F)")

        # A3 — split horizon: never hand a peer back what it taught us.
        if p["learned"] > 10 and p["advert"] > SPLIT_HORIZON_MAX:
            alerts.append(
                f"A3 SPLIT_HORIZON {call}: learned {p['learned']} from it yet "
                f"advertise {p['advert']} back")

        # A7 — a queue that does not move is a drain that has stalled.
        if prev:
            pp = ((prev.get("ufv") or {}).get("peers") or {}).get(call)
            if pp and p["queued"] > 0 and p["queued"] == pp["queued"]:
                alerts.append(
                    f"A7 QUEUE_STUCK {call}: queued={p['queued']} unchanged "
                    "since previous sample")

    # A4 — uptime going backwards means the session restarted between samples.
    if prev:
        for call, lk in links.items():
            pl = (prev.get("ufv") or {}).get("links", {}).get(call)
            if pl:
                now_s, then_s = uptime_secs(lk["uptime"]), uptime_secs(pl["uptime"])
                if now_s is not None and then_s is not None and now_s < then_s:
                    alerts.append(
                        f"A4 SESSION_RESTART {call}: uptime {pl['uptime']} -> "
                        f"{lk['uptime']}")

        # A8 — a burst of zero-RTT skips means routes arriving unusable.
        pt, ct = (prev.get("ufv") or {}).get("transit"), ufv.get("transit")
        if pt and ct:
            d = ct["rtt0_skips"] - pt["rtt0_skips"]
            if d > RTT0_BURST:
                alerts.append(f"A8 RTT0_BURST +{d} skips since previous sample")

        # A6 — the append/contract asymmetry, tracked as a rate rather than a
        # total: a transit node should extend and contract in step.
        pl2, cl2 = (prev.get("ufv") or {}).get("l2"), ufv.get("l2")
        if pl2 and cl2:
            de = cl2["extended"] - pl2["extended"]
            dc = cl2["contracted"] - pl2["contracted"]
            dd = cl2["declined"] - pl2["declined"]
            if de or dc or dd:
                alerts.append(
                    f"I6 L2_DELTA extended+{de} contracted+{dc} declined+{dd}")
            if de == 0 and dc > 5:
                alerts.append(
                    f"A6 L2_CONTRACT_ONLY contracted+{dc} with extended+0 "
                    "(reverse frames for chains we never appended)")

    # A12 — cost drift on the peers' side, which is what PCF reacts to.
    for call, remote in tables.items():
        if not remote:
            continue
        for rcall, row in remote.items():
            if rcall.upper().startswith("IR2UFV") and prev:
                prow = (prev.get({"IW2OHX-14": "n14", "IW2OHX-4": "n4",
                                  "IW2OHX-12": "n12"}[call]) or {}).get(rcall)
                if prow and abs(row["cost"] - prow["cost"]) > 10:
                    alerts.append(
                        f"A12 COST_DRIFT {call}: cost to IR2UFV "
                        f"{prow['cost']} -> {row['cost']}")
    return alerts


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval", type=int, default=900)
    ap.add_argument("--iterations", type=int, default=0, help="0 = until killed")
    ap.add_argument("--out", default="/tmp/quad-watch")
    ap.add_argument("--skip-pcf", action="store_true",
                    help="skip the chained -12 query (it crosses the live mesh)")
    ap.add_argument("--pcf-every", type=int, default=4,
                    help="query -12 only every Nth sample (default 4)")
    args = ap.parse_args()

    pw_x = os.environ.get("QW_PW_XNET", "")
    sys_x = os.environ.get("QW_SYS_XNET", "")
    u14 = os.environ.get("QW_USER_14", "")
    u4 = os.environ.get("QW_USER_4", "")
    pw_u = os.environ.get("QW_PW_UFV", "")
    u_ufv = os.environ.get("QW_USER_UFV", "")
    if not (pw_x and u14 and u4 and pw_u and u_ufv):
        sys.exit("missing credentials in environment (see module docstring)")

    os.makedirs(args.out, exist_ok=True)
    f_jsonl = os.path.join(args.out, "samples.jsonl")
    f_log = os.path.join(args.out, "watch.log")
    f_alert = os.path.join(args.out, "alerts.log")

    def say(path, msg):
        with open(path, "a") as fh:
            fh.write(msg + "\n")

    say(f_log, f"=== quad-watch start {now_iso()} interval={args.interval}s ===")
    prev = None
    n = 0
    while True:
        n += 1
        sample = {"ts": now_iso(), "n": n}
        # Each node is sampled in its own try: one unreachable node must not
        # discard the other three, and a missing table must read as absent
        # rather than as zero (a zero would fire false inconsistencies).
        try:
            sample["ufv"] = parse_ufv(sample_ufv(u_ufv, pw_u))
        except Exception as exc:                        # noqa: BLE001
            sample["ufv_error"] = str(exc)
        try:
            sample["n14"] = parse_xnet_l(sample_xnet(XNET_14, u14, pw_x, sys_x))
        except Exception as exc:                        # noqa: BLE001
            sample["n14_error"] = str(exc)
        try:
            sample["n4"] = parse_xnet_l(sample_xnet(XNET_4, u4, pw_x, sys_x))
        except Exception as exc:                        # noqa: BLE001
            sample["n4_error"] = str(exc)
        if not args.skip_pcf and (n % args.pcf_every == 1):
            try:
                sample["n12"] = parse_xnet_l(
                    sample_xnet(XNET_14, u14, pw_x, sys_x, chain_to="IW2OHX-12"))
            except Exception as exc:                    # noqa: BLE001
                sample["n12_error"] = str(exc)

        sample["alerts"] = check(sample, prev)

        with open(f_jsonl, "a") as fh:
            fh.write(json.dumps(sample) + "\n")

        ufv = sample.get("ufv") or {}
        lines = [f"--- sample {n} {sample['ts']} ---"]
        for call, lk in sorted((ufv.get("links") or {}).items()):
            p = (ufv.get("peers") or {}).get(call, {})
            lines.append(
                f"  {call:<11} {lk['status']:<10} up={lk['uptime']} "
                f"routes={lk['routes']:<4} learned={p.get('learned', '?'):<4} "
                f"advert={p.get('advert', '?'):<4} queued={p.get('queued', '?')}")
        if ufv.get("l2"):
            q = ufv["l2"]
            lines.append(f"  L2 {q['state']} ext={q['extended']} "
                         f"con={q['contracted']} dec={q['declined']}")
        for key, label in (("n14", "IW2OHX-14"), ("n4", "IW2OHX-4"),
                           ("n12", "IW2OHX-12")):
            tbl = sample.get(key)
            if tbl:
                ufv_row = next((v for k, v in tbl.items()
                                if k.upper().startswith("IR2UFV")), None)
                lines.append(f"  {label} sees IR2UFV: {ufv_row} "
                             f"({len(tbl)} link rows)")
            elif sample.get(key + "_error"):
                lines.append(f"  {label} UNREACHABLE: {sample[key + '_error']}")
        for a in sample["alerts"]:
            lines.append("  ! " + a)
        say(f_log, "\n".join(lines))
        for a in sample["alerts"]:
            say(f_alert, f"{sample['ts']} sample={n} {a}")

        prev = sample
        if args.iterations and n >= args.iterations:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
