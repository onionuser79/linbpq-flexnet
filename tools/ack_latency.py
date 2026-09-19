#!/usr/bin/env python3
"""Ack latency: how long after an inbound I-frame do we put anything on
the wire? An I-frame always needs an answer, so this is the node's
response time with no interpretation. Correlates each slow one with the
/proc sampler, which says whether the node was computing or blocked.

usage: ack_latency.py CAPTURE LOCAL_IP PEER_IP [MIN_MS] [SAMPLER_CSV]
"""
import importlib.util, os, sys
from datetime import datetime, timezone

_here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "td", os.path.join(_here, "axudp_teardown.py"))
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)

path, local, peer = sys.argv[1], sys.argv[2], sys.argv[3]
min_ms = float(sys.argv[4]) if len(sys.argv) > 4 else 400.0
csv = sys.argv[5] if len(sys.argv) > 5 else None

samples = []
if csv and os.path.exists(csv):
    with open(csv) as fh:
        next(fh, None)
        for line in fh:
            p = line.strip().split(",")
            if len(p) >= 4:
                try:
                    # epoch,utime,stime[,wchan,logsize,cache_mtime]
                    wchan = p[3] if len(p) >= 6 else "-"
                    logsz = int(p[4]) if len(p) >= 6 else int(p[3])
                    samples.append((float(p[0]), float(p[1]) + float(p[2]),
                                    logsz, wchan))
                except ValueError:
                    pass

frames = []
for ts, linktype, data in td.read_pcap(path):
    ip = td.strip_link_header(linktype, data)
    if ip is None:
        continue
    u = td.parse_udp(ip)
    if u is None or peer not in (u[0], u[1]):
        continue
    f = td.decode_ax25(u[4])
    if not f or f["digis"]:
        continue
    off = 15
    f["pid"] = u[4][off] if (f["ctl"] & 0x01) == 0 and len(u[4]) > off else None
    f["info"] = u[4][off + 1:-2] if f["pid"] is not None else b""
    f["ts"] = ts
    f["out"] = (u[0] == local)
    frames.append(f)


def at(t):
    """CPU seconds, log bytes and wait channels around t."""
    if not samples:
        return None
    before = [s for s in samples if s[0] <= t]
    after = [s for s in samples if s[0] >= t]
    if not before or not after:
        return None
    a, b = before[-1], after[0]
    if b[0] - a[0] <= 0:
        return None
    # every wait channel seen inside the quiet stretch
    inside = [s[3] for s in samples if a[0] <= s[0] <= b[0]]
    return b[1] - a[1], b[2] - a[2], b[0] - a[0], ",".join(sorted(set(inside)))


def iso(ts):
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%H:%M:%S.%f")[:-3]


lat = []
for i, f in enumerate(frames):
    if f["out"] or (f["ctl"] & 0x01) != 0:
        continue
    nxt = next((g for g in frames[i + 1:] if g["out"]), None)
    if nxt is None:
        continue
    lat.append((nxt["ts"] - f["ts"], f))

vals = sorted(x[0] for x in lat)
if vals:
    n = len(vals)
    print(f"inbound I-frames: {n}   ack latency median {vals[n//2]*1000:.1f} ms  "
          f"p90 {vals[min(n-1,int(n*0.9))]*1000:.1f} ms  max {vals[-1]*1000:.1f} ms")
    print(f"slower than {min_ms:.0f} ms: "
          f"{sum(1 for v in vals if v*1000 > min_ms)} "
          f"({100.0*sum(1 for v in vals if v*1000 > min_ms)/n:.1f} %)\n")

for d, f in sorted(lat, key=lambda x: -x[0]):
    if d * 1000 < min_ms:
        break
    pid = f"PID={f['pid']:02X}" if f["pid"] is not None else ""
    txt = "".join(chr(b) if 32 <= b < 127 else "." for b in f["info"][:48])
    s = at(f["ts"] + d / 2)
    extra = ""
    if s:
        extra = (f"  [cpu +{s[0]*1000:.0f} ms, log +{s[1]} B over "
                 f"{s[2]*1000:.0f} ms, wchan {s[3]}]")
    print(f"{iso(f['ts'])}  ack in {d*1000:7.1f} ms  {pid} len={len(f['info'])}{extra}")
    if f["info"]:
        print(f"      {txt}")
