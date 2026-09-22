#!/usr/bin/env python3
"""Per '3+' transaction: the closing '3-', then every outbound record
frame that follows it, and where the teardown lands among them.

This is the measurement that identified the 2026-09-22 root cause of the
IR2UFV <-> IW2OHX-12 teardown. "Who hung up" and "what did we send" both
stayed ambiguous for four investigations because the population being
compared was wrong: our record frames are overwhelmingly safe, so any
average over them hides the effect. Slicing by TRANSACTION makes it a
clean law -- after the '3-' that closes a '3+' answer, PC/Flexnet accepts
at most two more record frames and then tears the session down (10 died
on the 1st, 20 on the 2nd, none on the 0th, none reached a 3rd).

A histogram with weight outside {1,2} means the quiesce is not holding.

usage: flexnet_transaction_audit.py CAPTURE.pcap LOCAL_IP PEER_IP
"""
import importlib.util, os, re, sys
_here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("td", os.path.join(_here, "axudp_teardown.py"))
td = importlib.util.module_from_spec(spec); spec.loader.exec_module(td)
REC = re.compile(rb'([A-Z0-9 ]{6})([0-9:;<=>?]{2})(\d+)')
path, local, peer = sys.argv[1], sys.argv[2], sys.argv[3]
ev = []
for ts, lt, data in td.read_pcap(path):
    ip = td.strip_link_header(lt, data)
    if ip is None: continue
    u = td.parse_udp(ip)
    if u is None: continue
    src, dst, _sp, _dp, pl = u
    if peer not in (src, dst): continue
    f = td.decode_ax25(pl)
    if f is None or f["digis"]: continue
    ev.append((ts, "Out" if src == local else "In", f["kind"],
               pl[16:] if f["kind"] == "I" else b""))

plus = [e[0] for e in ev if e[1] == "In" and e[3][:2] == b'3+']
eob  = [e[0] for e in ev if e[1] == "Out" and e[3][:2] == b'3-']
rec  = [e[0] for e in ev if e[1] == "Out" and e[3][:1] == b'3' and e[3][1:2] not in (b'-', b'+')]
tear = [e[0] for e in ev if e[1] == "In" and e[2] in ("DISC", "DM", "FRMR")]
indep, seen = [], []
for t in tear:
    if not any(0 < t - x <= 90 for x in seen): indep.append(t)
    seen.append(t)

print(f"{'3+':>9} {'close 3-':>10} {'answer':>7}  {'record frames after the close -> teardown':<44}")
hist = {}
for p in plus:
    nxt_p = next((x for x in plus if x > p), 1e18)
    close = next((x for x in eob if x >= p), None)
    if close is None or close >= nxt_p:
        print(f"{td.iso(p)[11:19]:>9} {'(none)':>10}"); continue
    t = next((x for x in indep if x >= close), None)
    after = [r for r in rec if close < r <= (t if t else nxt_p)]
    idx = len(after)
    hist[idx] = hist.get(idx, 0) + 1
    marks = "".join("." for _ in after[:-1]) + ("X" if t and after else "")
    print(f"{td.iso(p)[11:19]:>9} {td.iso(close)[11:19]:>10} {close-p:>6.0f}s  "
          f"frames after close={idx:<3} {marks:<12} teardown +{(t-close):.1f}s" if t else
          f"{td.iso(p)[11:19]:>9} {td.iso(close)[11:19]:>10} {close-p:>6.0f}s  frames after close={idx}  (no teardown)")
print(f"\nhistogram of 'record frames emitted after the closing 3- before the teardown':")
for k in sorted(hist): print(f"   {k:>3} frames : {hist[k]:>3} transactions")
