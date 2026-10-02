#!/usr/bin/env python3
"""cerecs.py PCAP SRCIP DSTIP — list FlexNet compact CE records sent SRC->DST.
pcap from `tcpdump -i any` (Linux cooked v1). Prints one line per record."""
import struct, sys
path, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
def ip(b): return '.'.join(str(x) for x in b)
def call(b):
    c = ''.join(chr(x >> 1) for x in b[:6]).strip(); s = (b[6] >> 1) & 15
    return f"{c}-{s}" if s else c
d = open(path, 'rb').read()
link = struct.unpack('<I', d[20:24])[0]
off = 24; nrec = 0; nfr = 0
while off + 16 <= len(d):
    ts, us, incl, orig = struct.unpack('<IIII', d[off:off+16]); off += 16
    pkt = d[off:off+incl]; off += incl
    l2 = 16 if link == 113 else 20 if link == 276 else 14
    ipb = pkt[l2:]
    if len(ipb) < 28 or ipb[9] != 17: continue
    ihl = (ipb[0] & 15) * 4
    if ip(ipb[12:16]) != src or ip(ipb[16:20]) != dst: continue
    ax = ipb[ihl+8:]
    i = 14
    while i - 1 < len(ax) and not (ax[i-1] & 1): i += 7
    if i + 2 > len(ax): continue
    ctl, pid = ax[i], ax[i+1]
    if pid != 0xCE: continue
    info = ax[i+2:-2]  # strip AXUDP CRC
    if not info or info[:1] != b'3' or info[:2] in (b'3+', b'3-'): continue
    nfr += 1
    body = info[1:].split(b'\r')[0].decode('latin-1')
    p = 0
    while p + 9 <= len(body):
        c = body[p:p+6].strip(); lo = ord(body[p+6]) - 0x30; hi = ord(body[p+7]) - 0x30
        q = body.index(' ', p+8) if ' ' in body[p+8:] else len(body)
        rtt = body[p+8:q]; p = q + 1
        nrec += 1
        print(f"{ts} {c} {lo}-{hi} {rtt}")
print(f"# frames={nfr} records={nrec}", file=sys.stderr)
