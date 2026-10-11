#!/usr/bin/env python3
"""ce_window.py - print CE/CF frames between two nodes in a pcap window.

usage: ce_window.py <pcap> <callA> <callB> <HH:MM:SS> <HH:MM:SS>  (local time)
Linux cooked (SLL/SLL2) or Ethernet captures of AXUDP; prints time,
direction, control and the CE payload as text.
"""
import struct, sys, time

def call(b):
    c = "".join(chr(x >> 1) for x in b[:6]).strip()
    s = (b[6] >> 1) & 0x0F
    return f"{c}-{s}" if s else c

def frames(path):
    with open(path, "rb") as f:
        link = struct.unpack("<I", f.read(24)[20:24])[0]
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            ts, us, cap, _ = struct.unpack("<IIII", rh)
            pkt = f.read(cap)
            off = {113: 16, 276: 20, 1: 14}[link]
            ip = pkt[off:]
            ihl = (ip[0] & 0x0F) * 4
            yield ts + us / 1e6, ip[ihl + 8:]

pcap, a, b, t0, t1 = sys.argv[1:6]
for ts, ax in frames(pcap):
    hhmmss = time.strftime("%H:%M:%S", time.localtime(ts))
    if not (t0 <= hhmmss <= t1) or len(ax) < 17:
        continue
    dst, src = call(ax[0:7]), call(ax[7:14])
    if {dst, src} != {a, b}:
        continue
    i = 14
    while not (ax[i - 1] & 1) and i + 7 <= len(ax):
        i += 7
    ctl = ax[i]
    pid = ax[i + 1] if ctl & 1 == 0 or ctl & 0xEF == 0x03 else None
    body = ax[i + 2:-2] if pid is not None else b""
    kind = "I" if ctl & 1 == 0 else f"{ctl:02x}"
    txt = body.decode("latin-1").replace("\r", "\\r")
    if pid == 0xCE and txt.startswith("2 "):
        txt = f"2 + {len(body) - 1} spaces (keepalive)"
    print(f"{hhmmss}.{int(ts % 1 * 1000):03d} {src:>9}->{dst:<9} {kind:>3} "
          f"{'' if pid is None else f'pid={pid:02X}'} {txt[:150]}")
