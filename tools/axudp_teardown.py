#!/usr/bin/env python3
"""
axudp_teardown.py — who tears an AX.25 link down first, from an AXUDP pcap.

The link-instability work needs one fact per peer that no table snapshot
can give: for each session cycle, which end sent the DISC (or the fresh
SABM that implies the other end had already gone). A `FL` uptime tells
you that a link restarted; only the wire tells you who restarted it.

Input is a tcpdump capture of the AXUDP transport (`udp port 10075`),
written with `-i any`, so the link type is LINUX_SLL or LINUX_SLL2. The
UDP payload is the bare AX.25 frame starting at the destination address
field — BPQ's `SendFrame()` passes `&buff->DEST[0]` straight to
`sendto()`, with no AXUDP header of its own.

Direction is taken from the IP header, not from the AX.25 callsigns: a
digipeated frame carries someone else's calls but still tells us which
host put it on the wire.

Usage:
    axudp_teardown.py CAPTURE.pcap [CAPTURE.pcap ...] [--local-ip IP]
                      [--peer-ip IP] [--json OUT.jsonl] [--quiet]

With no --local-ip the most frequent source address is assumed local.
"""

import argparse
import json
import struct
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone

# pcap link types we can strip a frame header off.
DLT_EN10MB = 1
DLT_RAW = 101
DLT_LINUX_SLL = 113
DLT_LINUX_SLL2 = 276

# AX.25 unnumbered / supervisory control bytes, P/F bit (0x10) masked off.
U_FRAMES = {
    0x2F: "SABM",
    0x6F: "SABME",
    0x43: "DISC",
    0x0F: "DM",
    0x63: "UA",
    0x87: "FRMR",
    0x03: "UI",
}
S_FRAMES = {0x01: "RR", 0x05: "RNR", 0x09: "REJ", 0x0D: "SREJ"}

# A teardown is one of these; everything else is carrying traffic.
TEARDOWN = {"DISC", "DM", "FRMR"}
SETUP = {"SABM", "SABME"}


class PcapError(Exception):
    pass


def read_pcap(path):
    """Yield (epoch_float, linktype, raw_frame_bytes) for a classic pcap.

    Raises PcapError on a pcapng file or an unreadable header; the caller
    turns that into a skipped file rather than a crash, because a rolling
    tcpdump can leave a zero-length or half-written member behind.
    """
    with open(path, "rb") as fh:
        hdr = fh.read(24)
        if len(hdr) < 24:
            raise PcapError(f"{path}: short file ({len(hdr)} bytes)")
        magic = hdr[:4]
        if magic == b"\x0a\x0d\x0d\x0a":
            raise PcapError(f"{path}: pcapng, not supported")
        if magic == b"\xa1\xb2\xc3\xd4":
            endian, nano = ">", False
        elif magic == b"\xd4\xc3\xb2\xa1":
            endian, nano = "<", False
        elif magic == b"\xa1\xb2\x3c\x4d":
            endian, nano = ">", True
        elif magic == b"\x4d\x3c\xb2\xa1":
            endian, nano = "<", True
        else:
            raise PcapError(f"{path}: not a pcap (magic {magic!r})")
        linktype = struct.unpack(endian + "I", hdr[20:24])[0]

        rec = struct.Struct(endian + "IIII")
        while True:
            rh = fh.read(16)
            if len(rh) < 16:
                return
            ts_sec, ts_frac, caplen, _origlen = rec.unpack(rh)
            data = fh.read(caplen)
            if len(data) < caplen:
                return                       # truncated tail: stop cleanly
            ts = ts_sec + (ts_frac / 1e9 if nano else ts_frac / 1e6)
            yield ts, linktype, data


def strip_link_header(linktype, data):
    """Return the IP payload of a captured frame, or None if not IPv4."""
    if linktype == DLT_LINUX_SLL2:
        if len(data) < 20:
            return None
        proto = struct.unpack(">H", data[0:2])[0]
        return data[20:] if proto == 0x0800 else None
    if linktype == DLT_LINUX_SLL:
        if len(data) < 16:
            return None
        proto = struct.unpack(">H", data[14:16])[0]
        return data[16:] if proto == 0x0800 else None
    if linktype == DLT_EN10MB:
        if len(data) < 14:
            return None
        proto = struct.unpack(">H", data[12:14])[0]
        return data[14:] if proto == 0x0800 else None
    if linktype == DLT_RAW:
        return data
    return None


def parse_udp(ip_pkt):
    """Return (src_ip, dst_ip, sport, dport, payload) for a UDP datagram."""
    if len(ip_pkt) < 20 or (ip_pkt[0] >> 4) != 4:
        return None
    ihl = (ip_pkt[0] & 0x0F) * 4
    if ip_pkt[9] != 17 or len(ip_pkt) < ihl + 8:
        return None
    src = ".".join(str(b) for b in ip_pkt[12:16])
    dst = ".".join(str(b) for b in ip_pkt[16:20])
    sport, dport, ulen, _ck = struct.unpack(">HHHH", ip_pkt[ihl:ihl + 8])
    payload = ip_pkt[ihl + 8:ihl + ulen] if ulen >= 8 else ip_pkt[ihl + 8:]
    return src, dst, sport, dport, payload


def decode_call(field):
    """Decode one 7-byte AX.25 address field to CALL-SSID."""
    call = "".join(chr(b >> 1) for b in field[:6]).strip()
    ssid = (field[6] >> 1) & 0x0F
    return f"{call}-{ssid}"


def decode_ax25(payload):
    """Decode an AX.25 frame header.

    Returns a dict with dest/src/digis/ctl/kind, or None if the frame is
    not plausibly AX.25. The address field is self-delimiting (bit 0 of
    the SSID byte marks the last one), which is also the validity check:
    a non-AX.25 payload runs off the end.
    """
    if len(payload) < 15:
        return None
    addrs = []
    off = 0
    while off + 7 <= len(payload):
        field = payload[off:off + 7]
        # Callsign characters are ASCII shifted left one bit.
        for b in field[:6]:
            if (b & 0x01) or not (0x40 <= b <= 0xBF):
                return None
        addrs.append(field)
        off += 7
        if field[6] & 0x01:
            break
        if len(addrs) > 10:
            return None
    if len(addrs) < 2 or not (addrs[-1][6] & 0x01) or off >= len(payload):
        return None

    ctl = payload[off]
    pf = bool(ctl & 0x10)
    if (ctl & 0x01) == 0:
        kind = "I"
    elif (ctl & 0x03) == 0x01:
        kind = S_FRAMES.get(ctl & 0x0F, f"S?{ctl:02X}")
    else:
        kind = U_FRAMES.get(ctl & 0xEF, f"U?{ctl:02X}")
    return {
        "dest": decode_call(addrs[0]),
        "src": decode_call(addrs[1]),
        "digis": [decode_call(a) for a in addrs[2:]],
        "ctl": ctl,
        "kind": kind,
        "pf": pf,
    }


def iso(ts):
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--local-ip", help="our address; default = most frequent source")
    ap.add_argument("--peer-ip", help="restrict to one peer address")
    ap.add_argument("--json", help="write one JSON object per event here")
    ap.add_argument("--quiet", action="store_true", help="summary only")
    ap.add_argument("--link-only", action="store_true",
                    help="drop digipeated frames: count only our own L2 sessions")
    # tcpdump keeps writing into the same rolling file across a config
    # change, so before/after has to be cut by time, not by file.
    ap.add_argument("--since", help="ignore frames before this UTC time, "
                                    "'YYYY-MM-DDTHH:MM:SSZ' or an epoch")
    ap.add_argument("--until", help="ignore frames at or after this UTC time")
    args = ap.parse_args()

    def as_epoch(text):
        if not text:
            return None
        try:
            return float(text)
        except ValueError:
            pass
        stamp = datetime.strptime(text.rstrip("Z"), "%Y-%m-%dT%H:%M:%S")
        return stamp.replace(tzinfo=timezone.utc).timestamp()

    since, until = as_epoch(args.since), as_epoch(args.until)

    frames = []
    for path in args.captures:
        try:
            for ts, linktype, data in read_pcap(path):
                if since is not None and ts < since:
                    continue
                if until is not None and ts >= until:
                    continue
                ip_pkt = strip_link_header(linktype, data)
                if ip_pkt is None:
                    continue
                udp = parse_udp(ip_pkt)
                if udp is None:
                    continue
                src, dst, _sp, _dp, payload = udp
                ax = decode_ax25(payload)
                if ax is None:
                    continue
                frames.append((ts, src, dst, ax))
        except PcapError as exc:
            print(f"skip: {exc}", file=sys.stderr)
    if not frames:
        print("no AX.25-over-UDP frames decoded", file=sys.stderr)
        return 1
    frames.sort(key=lambda f: f[0])

    local = args.local_ip or Counter(f[1] for f in frames).most_common(1)[0][0]

    events = []
    per_peer = defaultdict(lambda: Counter())
    per_l2 = defaultdict(lambda: Counter())
    for ts, src, dst, ax in frames:
        if src == local:
            direction, peer_ip = "Out", dst
        elif dst == local:
            direction, peer_ip = "In", src
        else:
            continue
        if args.peer_ip and peer_ip != args.peer_ip:
            continue
        # A frame carrying digipeaters is somebody else's session passing
        # through us. Counting those as link events is how a transit
        # connect attempt gets mistaken for our own peer cycling — it
        # inflated the first -14 reading from 7 SABMs to 69.
        transit = bool(ax["digis"])
        if args.link_only and transit:
            continue
        per_peer[peer_ip][f"{direction} {ax['kind']}"] += 1
        if not transit:
            pair = " <-> ".join(sorted((ax["src"], ax["dest"])))
            per_l2[(peer_ip, pair)][f"{direction} {ax['kind']}"] += 1
        if ax["kind"] in TEARDOWN or ax["kind"] in SETUP:
            events.append({
                "ts": iso(ts), "epoch": round(ts, 3), "peer_ip": peer_ip,
                "dir": direction, "kind": ax["kind"], "pf": ax["pf"],
                "src": ax["src"], "dest": ax["dest"], "digis": ax["digis"],
                "transit": transit,
            })

    span = ""
    if frames:
        span = f"  window {iso(frames[0][0])} .. {iso(frames[-1][0])}"
    if not args.quiet:
        print(f"local address assumed: {local}{span}")
        print(f"decoded {len(frames)} AX.25 frames, "
              f"{len(events)} setup/teardown events\n")
    else:
        print(f"window:{span.strip()}")
        print("=== session setup / teardown timeline ===")
        for ev in events:
            digi = (" via " + ",".join(ev["digis"])) if ev["digis"] else ""
            tag = "transit" if ev["transit"] else "LINK   "
            print(f"{ev['ts']}  {tag} {ev['peer_ip']:<15} {ev['dir']:<3} "
                  f"{ev['kind']:<5}{'P/F' if ev['pf'] else '   '} "
                  f"{ev['src']}>{ev['dest']}{digi}")
        print()

    print("=== per-peer frame census ===")
    for peer_ip in sorted(per_peer):
        counts = per_peer[peer_ip]
        total = sum(counts.values())
        print(f"\n{peer_ip}  ({total} frames)")
        for key in sorted(counts, key=lambda k: -counts[k]):
            print(f"    {key:<12} {counts[key]}")

    print("\n=== per-L2-session census (digipeated frames excluded) ===")
    for key in sorted(per_l2):
        peer_ip, pair = key
        counts = per_l2[key]
        print(f"\n{pair}   [{peer_ip}]  ({sum(counts.values())} frames)")
        for name in sorted(counts, key=lambda k: -counts[k]):
            print(f"    {name:<12} {counts[name]}")

    print("\n=== who initiates, per peer (own L2 sessions only) ===")
    for peer_ip in sorted(per_peer):
        peer_events = [e for e in events
                       if e["peer_ip"] == peer_ip and not e["transit"]]
        out_d = sum(1 for e in peer_events if e["dir"] == "Out" and e["kind"] in TEARDOWN)
        in_d = sum(1 for e in peer_events if e["dir"] == "In" and e["kind"] in TEARDOWN)
        out_s = sum(1 for e in peer_events if e["dir"] == "Out" and e["kind"] in SETUP)
        in_s = sum(1 for e in peer_events if e["dir"] == "In" and e["kind"] in SETUP)
        verdict = ("we tear down" if out_d > in_d * 2 else
                   "peer tears down" if in_d > out_d * 2 else
                   "both" if (out_d or in_d) else "no teardown seen")
        print(f"{peer_ip:<15} teardown Out={out_d} In={in_d} | "
              f"setup Out={out_s} In={in_s}   -> {verdict}")

    if args.json:
        with open(args.json, "w") as fh:
            for ev in events:
                fh.write(json.dumps(ev) + "\n")
        print(f"\nwrote {len(events)} events to {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
