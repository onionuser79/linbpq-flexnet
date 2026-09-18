#!/usr/bin/env python3
"""Show what happens immediately before each link teardown.

"Who sent DISC" narrows the fault to one end; it does not say why. The
frames in the seconds before it do: an N2 retry exhaustion, a peer
answering DM because it lost the session, and an idle timeout all look
identical in a session-uptime table and completely different here.
"""
import importlib.util
import os
import sys
from collections import Counter

# Shares the pcap/AX.25 decoder with axudp_teardown.py, which sits next
# to this file wherever the pair has been copied to.
_here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "td", os.path.join(_here, "axudp_teardown.py"))
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)

if len(sys.argv) < 4:
    print("usage: disc_context.py CAPTURE.pcap LOCAL_IP PEER_IP "
          "[CONTEXT_FRAMES] [Out|In]", file=sys.stderr)
    raise SystemExit(2)

path = sys.argv[1]
local = sys.argv[2]
peer = sys.argv[3]
context = int(sys.argv[4]) if len(sys.argv) > 4 else 12
WANT_DIR = (sys.argv[5],) if len(sys.argv) > 5 else ("Out",)

rows = []
for ts, linktype, data in td.read_pcap(path):
    ip_pkt = td.strip_link_header(linktype, data)
    if ip_pkt is None:
        continue
    udp = td.parse_udp(ip_pkt)
    if udp is None:
        continue
    src, dst, _sp, _dp, payload = udp
    ax = td.decode_ax25(payload)
    if ax is None or ax["digis"]:
        continue
    if src == local and dst == peer:
        direction = "Out"
    elif src == peer and dst == local:
        direction = "In"
    else:
        continue
    rows.append((ts, direction, ax, len(payload)))

rows.sort(key=lambda r: r[0])
print(f"{len(rows)} non-digipeated frames on {local} <-> {peer}\n")

triggers = Counter()
for i, (ts, direction, ax, _ln) in enumerate(rows):
    if not (direction in WANT_DIR and ax["kind"] in ("DISC", "DM")):
        continue
    start = max(0, i - context)
    print(f"===== {direction} {ax['kind']} at {td.iso(ts)} "
          f"({ax['src']}>{ax['dest']}) =====")
    prev_ts = None
    for ts2, d2, ax2, ln2 in rows[start:i + 1]:
        gap = "" if prev_ts is None else f"+{ts2 - prev_ts:6.2f}s"
        prev_ts = ts2
        ns = f" N(S)={(ax2['ctl'] >> 1) & 0x07}" if ax2["kind"] == "I" else ""
        nr = (f" N(R)={(ax2['ctl'] >> 5) & 0x07}"
              if ax2["kind"] in ("RR", "RNR", "REJ") or ax2["kind"] == "I"
              else "")
        print(f"   {td.iso(ts2)} {gap:>9} {d2:<3} {ax2['kind']:<5}"
              f"{'F' if ax2['pf'] else ' '}{ns}{nr} len={ln2}")
    # Classify: what did the 6 frames before the teardown look like?
    window = rows[max(0, i - 6):i]
    kinds = [f"{d}{a['kind']}" for _t, d, a, _l in window]
    if kinds.count("OutI") >= 4 and "InRR" in kinds:
        triggers["our I-frames retried, peer only RRs (N2 exhaustion)"] += 1
    elif any(k == "InDM" for k in kinds):
        triggers["peer answered DM (it lost the session)"] += 1
    elif any(k in ("InSABM", "InDISC") for k in kinds):
        triggers["peer re-established or disconnected first"] += 1
    else:
        triggers["other / idle"] += 1
    print()

print("=== teardown trigger classification ===")
for k, n in triggers.most_common():
    print(f"  {n:>3}  {k}")
