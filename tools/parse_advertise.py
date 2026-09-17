#!/usr/bin/env python3
"""Analyse a v2.2 transit re-advertisement capture (RFC §10.1.b, B2-B6).

Reads a pcap of the IR2UFV AXIP port and reports, per peer, the CE
compact records we EMITTED: their cadence, the inter-record gap
distribution against the peer's token-bucket rate, which destinations
were advertised, and whether any route went back toward the peer it
came from (a split-horizon violation).

Usage:
    parse_advertise.py <pcap> --me <IR2UFV|...> [--peer-port MAP]

A capture alone cannot tell which peer a UDP datagram belongs to when
several peers share one port, so peers are keyed by the remote IP:port
of the AXUDP datagram. Pass --map to label them.
"""
import argparse
import collections
import struct
import sys

# AXUDP: datagram is a raw AX.25 frame (addresses, control, PID, info).
AX25_ADDR_LEN = 7


def ax25_call(raw):
    """Decode one 7-byte AX.25 address field to CALL-SSID."""
    call = ''.join(chr(b >> 1) for b in raw[:6]).strip()
    ssid = (raw[6] >> 1) & 0x0F
    return f"{call}-{ssid}" if ssid else call


def parse_ax25(payload):
    """Return (dst, src, pid, info) or None if not a parseable I/UI frame."""
    if len(payload) < 2 * AX25_ADDR_LEN + 2:
        return None
    dst = ax25_call(payload[0:7])
    src = ax25_call(payload[7:14])
    i = 14
    # Skip digipeaters: the address field ends when bit 0 of byte 7 is set.
    if not payload[13] & 0x01:
        while i + AX25_ADDR_LEN <= len(payload):
            last = payload[i + 6] & 0x01
            i += AX25_ADDR_LEN
            if last:
                break
    if i + 2 > len(payload):
        return None
    ctrl = payload[i]
    # I-frame: bit 0 clear. UI: 0x03.
    if ctrl & 0x01 and ctrl != 0x03:
        return None
    pid = payload[i + 1]
    return dst, src, pid, payload[i + 2:]


def parse_compact_records(info):
    """Yield (call, ssid_lo, ssid_hi, rtt) for each '3'-prefixed record.

    Wire shape per RFC §5.8:  '3' CALL(6) SSID_LO SSID_HI RTT ' ' CR
    """
    text = info.decode('latin-1')
    for chunk in text.split('\r'):
        if not chunk.startswith('3') or len(chunk) < 10:
            continue
        body = chunk[1:]
        if body.startswith(('+', '-')):
            continue
        call = body[0:6].strip()
        lo = ord(body[6]) - 0x30
        hi = ord(body[7]) - 0x30
        rtt = body[8:].strip()
        if not rtt.isdigit():
            continue
        yield call, lo, hi, int(rtt)


def parse_tokens(info):
    text = info.decode('latin-1')
    return [t for t in ('3+', '3-') if text.startswith(t)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pcap')
    ap.add_argument('--me', required=True,
                    help='our node call, e.g. IR2UFV — selects emissions')
    ap.add_argument('--bucket-pcf', type=float, default=5.0)
    ap.add_argument('--bucket-xnet', type=float, default=2.0)
    args = ap.parse_args()

    try:
        from scapy.all import PcapReader, UDP  # noqa: N811
    except ImportError:
        sys.exit("needs scapy: pip install scapy")

    emitted = collections.defaultdict(list)   # peer -> [(ts, rec)]
    received = collections.defaultdict(set)   # peer -> {(call, lo, hi)}
    tokens = collections.defaultdict(list)    # peer -> [(ts, token)]

    with PcapReader(args.pcap) as pcap:
        for pkt in pcap:
            if UDP not in pkt:
                continue
            payload = bytes(pkt[UDP].payload)
            frame = parse_ax25(payload)
            if not frame:
                continue
            dst, src, pid, info = frame
            if pid != 0xCE or not info:
                continue
            ts = float(pkt.time)
            me = args.me.upper()
            if src.split('-')[0] == me:
                peer = dst
                for rec in parse_compact_records(info):
                    emitted[peer].append((ts, rec))
                for tok in parse_tokens(info):
                    tokens[peer].append((ts, tok))
            elif dst.split('-')[0] == me:
                for call, lo, hi, _rtt in parse_compact_records(info):
                    received[src].add((call, lo, hi))

    if not emitted:
        print("no outbound CE compact records found — check --me and the pcap")
        return

    print(f"=== Emitted CE compact records (from {args.me}) ===\n")
    for peer in sorted(emitted):
        recs = emitted[peer]
        gaps = [round(b[0] - a[0], 2)
                for a, b in zip(recs, recs[1:]) if b[0] > a[0]]
        span = recs[-1][0] - recs[0][0] if len(recs) > 1 else 0.0
        dests = {(r[1][0], r[1][1], r[1][2]) for r in recs}
        poison = [r for r in recs if r[1][3] >= 60000]

        print(f"peer {peer}")
        print(f"  records          : {len(recs)} over {span:.0f}s "
              f"({len(recs) / span * 60:.1f}/min)" if span else
              f"  records          : {len(recs)}")
        print(f"  distinct dests   : {len(dests)}")
        print(f"  poison (RTT>=60k): {len(poison)}")
        if gaps:
            gaps_sorted = sorted(gaps)
            print(f"  inter-record gap : min={gaps_sorted[0]}s "
                  f"median={gaps_sorted[len(gaps_sorted) // 2]}s "
                  f"max={gaps_sorted[-1]}s")
            # B5: no gap below the peer's bucket period (allow 0 for
            # records sharing one I-frame, which the bucket permits on
            # accumulated credit up to the burst size).
            for limit, name in ((args.bucket_pcf, 'PCF'),
                                (args.bucket_xnet, 'xnet')):
                under = [g for g in gaps if 0 < g < limit]
                print(f"  gaps < {limit}s ({name:4s}) : {len(under)}")
        toks = collections.Counter(t for _, t in tokens[peer])
        print(f"  tokens           : {dict(toks) or 'none'}")

        # B6 — split-horizon: nothing learned from this peer may be
        # advertised back to it.
        back = dests & received.get(peer, set())
        status = 'OK' if not back else f'VIOLATION: {sorted(back)[:5]}'
        print(f"  split-horizon    : {status}")
        print()

    print("=== Received CE compact records (per peer) ===")
    for peer in sorted(received):
        print(f"  {peer}: {len(received[peer])} distinct dests")


if __name__ == '__main__':
    main()
