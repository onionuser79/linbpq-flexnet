#!/usr/bin/env python3
"""Decode AXUDP frames in a pcap (tcpdump -i any): addresses with H bits,
control byte, frame type. usage: axdecode.py <pcap> [filter-substring]"""
import struct
import sys


def call(b):
    c = "".join(chr(x >> 1) for x in b[:6]).strip()
    ssid = (b[6] >> 1) & 0x0F
    return f"{c}-{ssid}" if ssid else c


def ctl_name(c):
    if c & 1 == 0:
        return f"I{(c >> 1) & 7}{(c >> 5) & 7}"
    if c & 3 == 1:
        return {0x01: "RR", 0x05: "RNR", 0x09: "REJ"}.get(c & 0x0F, "S?") + str(c >> 5)
    return {0x2F: "SABM", 0x6F: "SABME", 0x43: "DISC", 0x63: "UA", 0x0F: "DM",
            0x03: "UI", 0x87: "FRMR"}.get(c & 0xEF, f"U{c:02x}") + ("+" if c & 0x10 else "")


def frames(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        link = struct.unpack("<I", hdr[20:24])[0]
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            ts, tu, incl, _ = struct.unpack("<IIII", rh)
            pkt = f.read(incl)
            off = {113: 16, 276: 20, 1: 14}.get(link, 16)
            ip = pkt[off:]
            if not ip or ip[0] >> 4 != 4:
                continue
            ihl = (ip[0] & 0x0F) * 4
            src = ".".join(map(str, ip[12:16]))
            dst = ".".join(map(str, ip[16:20]))
            udp = ip[ihl:]
            sport, dport = struct.unpack(">HH", udp[:4])
            yield ts + tu / 1e6, f"{src}:{sport}", f"{dst}:{dport}", udp[8:]


def decode(ax):
    addrs = []
    i = 0
    while i + 7 <= len(ax):
        addrs.append(ax[i:i + 7])
        if ax[i + 6] & 1:
            break
        i += 7
    if len(addrs) < 2:
        return None
    ctl = ax[7 * len(addrs)] if 7 * len(addrs) < len(ax) else 0
    digis = " ".join(call(a) + ("*" if a[6] & 0x80 else "") for a in addrs[2:])
    return f"{call(addrs[1])}->{call(addrs[0])} [{digis}] {ctl_name(ctl)}"


if __name__ == "__main__":
    flt = sys.argv[2] if len(sys.argv) > 2 else ""
    import time
    for t, s, d, ax in frames(sys.argv[1]):
        txt = decode(ax)
        if txt and flt in txt:
            print(time.strftime("%H:%M:%S", time.gmtime(t)) + f".{int(t % 1 * 1000):03d}",
                  f"{s:>21} > {d:<21}", txt)
