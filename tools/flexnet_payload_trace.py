#!/usr/bin/env python3
"""Timestamped payload trace for one AXUDP peer: frame type + FlexNet body.

frames.py gives the L2 view; the teardown is clean at L2, so what matters
is what the I-frames actually carried.
usage: payload_trace.py CAPTURE LOCAL PEER [SINCE_ISO] [UNTIL_ISO] [--full]
"""
import importlib.util, os, sys
from datetime import datetime, timezone

_here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("td", os.path.join(_here, "axudp_teardown.py"))
td = importlib.util.module_from_spec(spec); spec.loader.exec_module(td)

args = [a for a in sys.argv[1:] if not a.startswith("--")]
FULL = "--full" in sys.argv
path, local, peer = args[0], args[1], args[2]
def to_ts(s): return datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()
t0 = to_ts(args[3]) if len(args) > 3 else None
t1 = to_ts(args[4]) if len(args) > 4 else None

prev = None
for ts, lt, data in td.read_pcap(path):
    if t0 and ts < t0: continue
    if t1 and ts >= t1: break
    ip = td.strip_link_header(lt, data)
    if ip is None: continue
    u = td.parse_udp(ip)
    if u is None: continue
    src, dst, _sp, _dp, pl = u
    if peer not in (src, dst): continue
    f = td.decode_ax25(pl)
    if f is None or f["digis"]: continue
    d = "Out" if src == local else "In "
    kind = f["kind"]
    body = pl[16:] if kind == "I" else b""
    pid = pl[15] if kind == "I" and len(pl) > 15 else None
    txt = body.decode("latin-1").replace("\r", "|")
    if not FULL and len(txt) > 150: txt = txt[:150] + f"...<{len(body)}B>"
    gap = "" if prev is None else f"+{ts-prev:5.1f}s"
    prev = ts
    pidtxt = f"PID={pid:02X} " if pid is not None else ""
    print(f"{td.iso(ts)[11:23]} {gap:>8} {d} {kind:5} {pidtxt}{txt}")
