#!/usr/bin/env python3
"""ROADMAP item 3: what do real FlexNet routers EMIT when they hand a
transit frame to us? Every inbound frame whose pending digi is us was
built by the peer that sent it, so the archive holds the peers' egress
shape for free. Group those frames by sender and by chain shape, with
callsigns abstracted to their role:

  S   = the sending peer (the node that built this frame)
  U   = us (the pending digi)
  N#  = other nodes in the chain, in order
  *   = H bit set

usage: egress_shapes.py <pcap>...
"""
import collections
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ingress_shapes import packets, addrs, call, fmt, ours, LOCAL, GW  # noqa: E402

PEERS = {"44.134.24.4": "IW2OHX-14 (X)Net", "192.168.1.201": "IW2OHX-12 PC/Flexnet",
         "192.168.1.203": "IW2OHX-4 (X)Net"}
PEERCALL = {"44.134.24.4": "IW2OHX-14", "192.168.1.201": "IW2OHX-12",
            "192.168.1.203": "IW2OHX-4"}

shapes = collections.Counter()
examples = {}
for p in sys.argv[1:]:
    for ts, src, dst, sport, dport, ax in packets(p):
        if dport not in LOCAL or dst != GW or src == GW or src not in PEERS:
            continue
        a = addrs(ax)
        if not a or len(a) < 3:
            continue
        digis = a[2:]
        pend = [i for i, d in enumerate(digis) if not d[6] & 0x80]
        if not pend or not ours(dport, call(digis[pend[0]])):
            continue
        roles, others = [], {}
        for i, d in enumerate(digis):
            c = fmt(call(d))
            if ours(dport, call(d)):
                r = "U"
            elif c == PEERCALL[src]:
                r = "S"
            else:
                r = others.setdefault(c, f"N{len(others) + 1}")
            roles.append(r + ("*" if d[6] & 0x80 else ""))
        ctl = ax[7 * len(a)] if 7 * len(a) < len(ax) else 0
        # which way: does the sender sit BEFORE us (forward) or is the
        # chain the reverse of one we extended (sender listed first, H set)?
        key = (PEERS[src], " ".join(roles), len(digis) - 1 - pend[0])
        shapes[key] += 1
        if key not in examples:
            chain = " ".join(fmt(call(d)) + ("*" if d[6] & 0x80 else "") for d in digis)
            examples[key] = f"{fmt(call(a[1]))}->{fmt(call(a[0]))} [{chain}] ctl={ctl:02x}"

for k, n in sorted(shapes.items(), key=lambda kv: (kv[0][0], -kv[1])):
    print(f"{n:6d}  {k[0]:22s} chain [{k[1]}]  digis after us: {k[2]}")
    print(f"          e.g. {examples[k]}")
