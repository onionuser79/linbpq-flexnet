#!/usr/bin/env python3
"""
parse_capture.py — Parse xnet_monitor.py raw captures into structured frame
records, then emit:

  1. A per-port frame inventory (counts by frame type / PID / src-dst pair).
  2. A timeline of CE-layer events (INIT / KA / LT / 3+ / 3- / compact / T6 / T7).
  3. A cross-port correlation (frames arriving on one port and being re-emitted
     on the other within a configurable window — the load-bearing signal for
     transit-role re-advertisement).

Input: one or more `*_raw.txt` files produced by xnet_monitor.py.
Each xnet monitor line looks like:

    <port>:fm <src> to <dst> [via <digi*>] ctl <CTL> [pid <PID>] [[N]] - DD.MM.YY HH:MM:SS

Optionally followed by an indented hex/text dump (when -h is passed; we
parse it if present, otherwise CE-frame classification works from the
[len] alone for KA/LT/T6/T7 — but with -iusk the CE payload is not
included, so we treat CE classification as "best effort by length".

Output: writes a *.parsed.json file next to each input.
"""

from __future__ import annotations
import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path


FRAME_RE = re.compile(
    r"^(?P<port>\d+):"
    r"fm\s+(?P<src>\S+)\s+to\s+(?P<dst>\S+)"
    r"(?:\s+via\s+(?P<digis>[^c]+?))?"
    r"\s+ctl\s+(?P<ctl>\S+)"
    r"(?:\s+pid\s+(?P<pid>\S+))?"
    r"(?:\s+\[(?P<plen>\d+)\])?"
    r"\s*-\s*(?P<date>\d\d\.\d\d\.\d\d)\s+(?P<time>\d\d:\d\d:\d\d)"
)


def classify_ce(plen: int | None) -> str:
    """Best-effort CE frame classification from payload length.

    With -iusk the monitor reports payload length but not contents, so we
    can only guess type from size. Refined classification needs `-h` (hex
    dump). This is good enough for inventory + cycle timing analysis.
    """
    if plen is None:
        return "ce?"
    if plen == 5:
        return "ce-init"          # type 0, fixed 5 bytes
    if plen == 241:
        return "ce-ka-xnet"       # xnet KA, '2' + 240 spaces
    if plen == 201:
        return "ce-ka-pcf"        # PC/Flexnet KA
    if 2 <= plen <= 6:
        return "ce-lt-or-stat"    # '1NNN\r' link-time, or '1n\r' stat family
    if plen == 3:
        return "ce-token"         # '3+\r' or '3-\r' tokens
    if 10 <= plen <= 250:
        return "ce-compact?"      # compact record batch (heuristic)
    return f"ce-unk-{plen}"


def parse_one(path: Path) -> dict:
    frames = []
    with path.open("r", encoding="utf-8", errors="replace") as fp:
        for ln in fp:
            ln = ln.rstrip("\r\n")
            m = FRAME_RE.match(ln)
            if not m:
                continue
            d = m.groupdict()
            port = int(d["port"])
            plen = int(d["plen"]) if d["plen"] else None
            ctl = d["ctl"]
            pid = (d["pid"] or "").upper()
            # frame-level classification
            if pid == "CE":
                kind = classify_ce(plen)
            elif pid == "CF":
                kind = "cf"
            elif pid == "F0":
                kind = "f0"
            else:
                # control-frame-only (RR / UA / SABM / DISC / FRMR / ...)
                cu = ctl.upper()
                if cu.startswith("RR"):
                    kind = "rr"
                elif cu.startswith("RNR"):
                    kind = "rnr"
                elif cu.startswith("REJ"):
                    kind = "rej"
                elif cu.startswith("SABM"):
                    kind = "sabm"
                elif cu.startswith("DISC"):
                    kind = "disc"
                elif cu.startswith("UA"):
                    kind = "ua"
                elif cu.startswith("DM"):
                    kind = "dm"
                elif cu.startswith("FRMR"):
                    kind = "frmr"
                elif cu.startswith("UI"):
                    kind = "ui"
                else:
                    kind = f"ctrl-{cu.lower()}"
            digis = (d["digis"] or "").strip()
            digi_list = digis.split() if digis else []
            # build epoch from DD.MM.YY HH:MM:SS (xnet UTC)
            dt = datetime.strptime(
                d["date"] + " " + d["time"], "%d.%m.%y %H:%M:%S"
            ).replace(tzinfo=timezone.utc)
            frames.append({
                "ts": dt.isoformat(),
                "epoch": int(dt.timestamp()),
                "port": port,
                "src": d["src"],
                "dst": d["dst"],
                "digis": digi_list,
                "ctl": ctl,
                "pid": pid,
                "plen": plen,
                "kind": kind,
            })
    return {"input": str(path), "frame_count": len(frames), "frames": frames}


def inventory(parsed: dict) -> dict:
    """Frame-type / src-dst / kind / payload-length histograms."""
    by_kind: dict[str, int] = {}
    by_pair: dict[str, int] = {}
    by_plen: dict[int, int] = {}
    for f in parsed["frames"]:
        by_kind[f["kind"]] = by_kind.get(f["kind"], 0) + 1
        pair = f"{f['src']}->{f['dst']}"
        by_pair[pair] = by_pair.get(pair, 0) + 1
        if f["plen"] is not None:
            by_plen[f["plen"]] = by_plen.get(f["plen"], 0) + 1
    return {
        "by_kind": dict(sorted(by_kind.items(), key=lambda x: -x[1])),
        "by_pair": dict(sorted(by_pair.items(), key=lambda x: -x[1])),
        "by_plen": dict(sorted(by_plen.items(), key=lambda x: -x[1])[:20]),
    }


def ce_timeline(parsed: dict, mark_kinds=("ce-init", "ce-ka-xnet",
                                          "ce-ka-pcf", "ce-token")) -> list:
    """Sparse timeline of CE control events for cycle-cadence analysis."""
    out = []
    for f in parsed["frames"]:
        if f["kind"] in mark_kinds or f["kind"].startswith("ce-compact"):
            out.append({
                "ts": f["ts"],
                "epoch": f["epoch"],
                "port": f["port"],
                "src": f["src"],
                "dst": f["dst"],
                "kind": f["kind"],
                "plen": f["plen"],
            })
    out.sort(key=lambda x: x["epoch"])
    return out


def correlate(p1_parsed: dict, p11_parsed: dict, window_s: int = 5) -> list:
    """For each CE payload frame on port 1, find a same-direction CE payload
    on port 11 within ±window_s seconds. Direction = "from xnet-14" vs
    "to xnet-14".

    This is the *bridge-style* re-emission test: if xnet were re-advertising
    transit destinations, we'd see, for every advertised record on one link,
    a matching shape (same callsign, same RTT shape) on the other link within
    a few seconds.
    """
    p1_ce = [f for f in p1_parsed["frames"]
             if f["pid"] == "CE" and f["plen"] and f["plen"] > 5]
    p11_ce = [f for f in p11_parsed["frames"]
              if f["pid"] == "CE" and f["plen"] and f["plen"] > 5]
    matches = []
    for a in p1_ce:
        # find candidates on the other port within window
        for b in p11_ce:
            if abs(a["epoch"] - b["epoch"]) > window_s:
                continue
            # require same direction relative to xnet-14
            same_dir = (("IW2OHX-14" in (a["src"], b["src"])) ==
                        ("IW2OHX-14" in (b["src"], a["src"])))
            if not same_dir:
                continue
            matches.append({
                "delta_s": b["epoch"] - a["epoch"],
                "a_port": a["port"], "a_kind": a["kind"],
                "a_pair": f"{a['src']}->{a['dst']}", "a_plen": a["plen"],
                "b_port": b["port"], "b_kind": b["kind"],
                "b_pair": f"{b['src']}->{b['dst']}", "b_plen": b["plen"],
            })
    return matches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help="*_raw.txt files from xnet_monitor.py")
    ap.add_argument("--correlate", action="store_true",
                    help="When two inputs are given, run cross-port correlation")
    ap.add_argument("--window", type=int, default=5, help="Correlation window (sec)")
    args = ap.parse_args()

    parsed = {}
    for p in args.inputs:
        path = Path(p)
        ps = parse_one(path)
        ps["inventory"] = inventory(ps)
        ps["ce_timeline_count"] = len(ce_timeline(ps))
        out = path.with_suffix(".parsed.json")
        with out.open("w") as fp:
            json.dump(ps, fp, default=str, indent=2)
        parsed[path.name] = ps
        print(f"[{path.name}] frames={ps['frame_count']} → {out.name}")
        print(f"   top kinds: {list(ps['inventory']['by_kind'].items())[:6]}")
        print(f"   top pairs: {list(ps['inventory']['by_pair'].items())[:4]}")

    if args.correlate and len(args.inputs) == 2:
        a, b = parsed.values()
        # which is which
        ports = {f["port"] for f in a["frames"]}, {f["port"] for f in b["frames"]}
        if 1 in ports[0] and 11 in ports[1]:
            p1, p11 = a, b
        else:
            p1, p11 = b, a
        m = correlate(p1, p11, window_s=args.window)
        print(f"\n[correlate] {len(m)} CE-payload pairs within ±{args.window}s")
        for x in m[:20]:
            print(f"  Δ={x['delta_s']:+3d}s  p{x['a_port']}/{x['a_kind']}({x['a_plen']})  "
                  f"{x['a_pair']:30s}  <->  "
                  f"p{x['b_port']}/{x['b_kind']}({x['b_plen']})  {x['b_pair']}")


if __name__ == "__main__":
    main()
