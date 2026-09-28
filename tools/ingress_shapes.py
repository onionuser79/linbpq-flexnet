#!/usr/bin/env python3
"""Classify every AXUDP frame ARRIVING at a LinBPQ instance on iw2ohx-gw by
the shape of its address field — the question behind ROADMAP item 2:
does any peer send a frame whose digi chain is already CONSUMED while its
DEST is not one of ours (i.e. expects us to route on DEST alone)?

usage: ingress_shapes.py <pcap>...     (tcpdump pcap, any linktype)

Local instances are told apart by their UDP port: 10093 = IW2OHX-13
(prod), 10075 = IR2UFV. A call is "ours" if its base call matches the
instance's node call (any SSID) — generous on purpose, so shape C cannot
be inflated by an application SSID we forgot.
"""
import collections
import struct
import sys
import time

GW = "192.168.1.202"
LOCAL = {10093: "IW2OHX-13", 10075: "IR2UFV"}
OURBASE = {10093: ("IW2OHX", {13}), 10075: ("IR2UFV", None)}


def call(b):
    c = "".join(chr(x >> 1) for x in b[:6]).strip()
    s = (b[6] >> 1) & 0x0F
    return c, s


def fmt(c):
    return f"{c[0]}-{c[1]}" if c[1] else c[0]


def ours(port, c):
    base, ssids = OURBASE[port]
    return c[0] == base and (ssids is None or c[1] in ssids)


def packets(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        if len(hdr) < 24:
            return
        link = struct.unpack("<I", hdr[20:24])[0]
        off = {113: 16, 276: 20, 1: 14}.get(link, 16)
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            ts, tu, incl, _ = struct.unpack("<IIII", rh)
            pkt = f.read(incl)
            ip = pkt[off:]
            if len(ip) < 28 or ip[0] >> 4 != 4 or ip[9] != 17:
                continue
            ihl = (ip[0] & 0x0F) * 4
            sport, dport = struct.unpack(">HH", ip[ihl:ihl + 4])
            yield ts, ".".join(map(str, ip[12:16])), ".".join(map(str, ip[16:20])), sport, dport, ip[ihl + 8:]


def addrs(ax):
    out, i = [], 0
    while i + 7 <= len(ax) and len(out) < 10:
        out.append(ax[i:i + 7])
        if ax[i + 6] & 1:
            return out
        i += 7
    return None


def classify(port, a):
    dest, digis = call(a[0]), a[2:]
    pending = [d for d in digis if not d[6] & 0x80]
    if pending:
        nxt = call(pending[0])
        return "A pending digi = us" if ours(port, nxt) else "D pending digi NOT us"
    return "B consumed, DEST ours" if ours(port, dest) else "C consumed, DEST NOT ours"


def main(paths):
    counts = collections.Counter()
    examples = collections.defaultdict(list)
    span = [None, None]
    for p in paths:
        for ts, src, dst, sport, dport, ax in packets(p):
            # Only frames TO a local instance: peers listen on the same port
            # numbers, so the port alone would also count our own output.
            if dport not in LOCAL or dst != GW or src == GW:
                continue
            a = addrs(ax)
            if not a or len(a) < 2:
                continue
            span[0] = ts if span[0] is None else min(span[0], ts)
            span[1] = ts if span[1] is None else max(span[1], ts)
            ctl = ax[7 * len(a)] if 7 * len(a) < len(ax) else 0
            shape = classify(dport, a)
            if shape.startswith("C") and (ctl & 0xEF) == 0x03:
                shape = "C' consumed, DEST NOT ours, UI (broadcast)"
            k = (LOCAL[dport], shape)
            counts[k] += 1
            if len(examples[k]) < 4:
                chain = " ".join(fmt(call(d)) + ("*" if d[6] & 0x80 else "") for d in a[2:])
                examples[k].append(f"{src}: {fmt(call(a[1]))}->{fmt(call(a[0]))} [{chain}]")

    print("span:", time.strftime("%F %TZ", time.gmtime(span[0])), "->",
          time.strftime("%F %TZ", time.gmtime(span[1])))
    for k in sorted(counts):
        print(f"{counts[k]:8d}  {k[0]:10s} {k[1]}")
        for e in examples[k]:
            print("            e.g.", e)


if __name__ == "__main__":
    main(sys.argv[1:])
