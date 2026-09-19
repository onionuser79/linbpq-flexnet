#!/usr/bin/env python3
"""How fast do we answer a peer's poll, and do slow answers cost the link?

A poll is any inbound COMMAND frame with P=1: the peer is asking for an
immediate response and is running its T1/N2 against our reply. The
answer is our next outbound RESPONSE with F=1. Everything here is in
wall-clock seconds off the pcap, so it measures the node's real
scheduling latency, not a modelled one.

usage: poll_latency.py CAPTURE LOCAL_IP PEER_IP
"""
import importlib.util, os, sys
from datetime import datetime, timezone

_here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "td", os.path.join(_here, "axudp_teardown.py"))
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)

path, local, peer = sys.argv[1], sys.argv[2], sys.argv[3]

frames = []
for ts, linktype, data in td.read_pcap(path):
    ip = td.strip_link_header(linktype, data)
    if ip is None:
        continue
    u = td.parse_udp(ip)
    if u is None:
        continue
    src_ip, dst_ip, payload = u[0], u[1], u[4]
    if peer not in (src_ip, dst_ip):
        continue
    f = td.decode_ax25(payload)
    if not f or f["digis"]:
        continue
    dest_c = (payload[6] & 0x80) != 0
    src_c = (payload[13] & 0x80) != 0
    f["cr"] = "CMD" if (dest_c and not src_c) else ("RSP" if src_c else "v1")
    f["out"] = (src_ip == local)
    f["ts"] = ts
    frames.append(f)


def iso(ts):
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%H:%M:%S.%f")[:-3]


lat = []
unanswered = 0
for i, f in enumerate(frames):
    if f["out"] or f["cr"] != "CMD" or not (f["ctl"] & 0x10):
        continue
    for g in frames[i + 1:]:
        if g["ts"] - f["ts"] > 20:
            unanswered += 1
            break
        if g["out"] and (g["ctl"] & 0x10) and g["cr"] in ("RSP", "v1"):
            lat.append((g["ts"] - f["ts"], f["ts"]))
            break
    else:
        unanswered += 1

lat.sort()
print(f"inbound polls answered: {len(lat)}   unanswered/timed out: {unanswered}")
if lat:
    vals = [x[0] for x in lat]
    n = len(vals)
    def pct(p):
        return vals[min(n - 1, int(n * p / 100))]
    print(f"  reply latency  min {vals[0]*1000:7.1f} ms   median {pct(50)*1000:7.1f} ms")
    print(f"                 p90 {pct(90)*1000:7.1f} ms   p99    {pct(99)*1000:7.1f} ms"
          f"   max {vals[-1]*1000:8.1f} ms")
    slow = [x for x in lat if x[0] > 0.3]
    print(f"  replies slower than 300 ms: {len(slow)} "
          f"({100.0*len(slow)/n:.1f} %)")
    for d, ts in sorted(slow, key=lambda x: -x[0])[:10]:
        print(f"     {iso(ts)}  {d*1000:8.1f} ms")

# Peer poll bursts: >=3 inbound polls inside 2 s.
bursts, cur = [], []
for f in frames:
    if f["out"] or f["cr"] != "CMD" or not (f["ctl"] & 0x10):
        continue
    if cur and f["ts"] - cur[-1] > 2.0:
        if len(cur) >= 3:
            bursts.append(cur)
        cur = []
    cur.append(f["ts"])
if len(cur) >= 3:
    bursts.append(cur)

print(f"\ninbound poll bursts (>=3 polls within 2 s): {len(bursts)}")
for b in bursts[:25]:
    gaps = [b[i + 1] - b[i] for i in range(len(b) - 1)]
    med = sorted(gaps)[len(gaps) // 2]
    # did the link die within 60 s?
    died = any((not g["out"]) and (g["ctl"] & 0xEF) in (0x0F, 0x43)
               and 0 <= g["ts"] - b[-1] <= 60 for g in frames)
    print(f"   {iso(b[0])}  polls={len(b):2d}  median gap {med*1000:6.1f} ms"
          f"  -> {'DM/DISC within 60 s' if died else 'survived'}")
