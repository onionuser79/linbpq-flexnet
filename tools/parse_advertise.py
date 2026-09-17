#!/usr/bin/env python3
"""Analyse a v2.2 transit re-advertisement capture (RFC §10.1.b, B2-B6).

Reads a pcap of a node's AXIP port and reports, per peer, the CE
compact records we EMITTED: count and cadence, the inter-record gap
distribution against that peer's token-bucket period, distinct
destinations, poison records, 3+/3- tokens, and whether any route went
back toward the peer it was learned from (a split-horizon violation).

No dependencies — pcap and AX.25 are parsed directly, because the hosts
that hold these captures are packet-radio nodes, not analysis boxes.

Usage:
    parse_advertise.py <pcap> --me IR2UFV
    parse_advertise.py <pcap> --me IR2UFV --pcf IW2OHX-12
"""
import argparse
import collections
import struct
import sys

PCAP_MAGIC_LE = 0xA1B2C3D4
PCAP_MAGIC_BE = 0xD4C3B2A1
PCAP_MAGIC_LE_NS = 0xA1B23C4D
PCAP_MAGIC_BE_NS = 0x4D3CB2A1

LINKTYPE_ETHERNET = 1
LINKTYPE_LINUX_SLL = 113
LINKTYPE_LINUX_SLL2 = 276
LINKTYPE_RAW = 101

AX25_ADDR_LEN = 7
PID_CE = 0xCE


def read_pcap(path):
    """Yield (timestamp, link-layer frame) for each record."""
    with open(path, 'rb') as fh:
        hdr = fh.read(24)
        if len(hdr) < 24:
            sys.exit(f"{path}: truncated pcap header")
        magic = struct.unpack('<I', hdr[:4])[0]
        if magic in (PCAP_MAGIC_LE, PCAP_MAGIC_LE_NS):
            end = '<'
        elif magic in (PCAP_MAGIC_BE, PCAP_MAGIC_BE_NS):
            end = '>'
        else:
            sys.exit(f"{path}: not a pcap file (magic {magic:#010x}); "
                     "pcapng is not supported — capture with -w plain pcap")
        nanos = magic in (PCAP_MAGIC_LE_NS, PCAP_MAGIC_BE_NS)
        linktype = struct.unpack(end + 'I', hdr[20:24])[0]
        while True:
            rec = fh.read(16)
            if len(rec) < 16:
                return
            ts_sec, ts_frac, incl, _orig = struct.unpack(end + 'IIII', rec)
            data = fh.read(incl)
            if len(data) < incl:
                return          # truncated tail: tcpdump killed mid-write
            ts = ts_sec + ts_frac / (1e9 if nanos else 1e6)
            yield ts, linktype, data


def strip_linklayer(linktype, frame):
    """Return (ethertype, payload) after the link-layer header."""
    if linktype == LINKTYPE_LINUX_SLL2:
        if len(frame) < 20:
            return None, b''
        proto = struct.unpack('>H', frame[0:2])[0]
        return proto, frame[20:]
    if linktype == LINKTYPE_LINUX_SLL:
        if len(frame) < 16:
            return None, b''
        proto = struct.unpack('>H', frame[14:16])[0]
        return proto, frame[16:]
    if linktype == LINKTYPE_ETHERNET:
        if len(frame) < 14:
            return None, b''
        return struct.unpack('>H', frame[12:14])[0], frame[14:]
    if linktype == LINKTYPE_RAW:
        return 0x0800, frame
    return None, b''


def udp_payload(ethertype, packet):
    """Extract the UDP payload from an IPv4/IPv6 packet, or None."""
    if ethertype == 0x0800:
        if len(packet) < 20 or (packet[0] >> 4) != 4:
            return None
        ihl = (packet[0] & 0x0F) * 4
        if packet[9] != 17 or len(packet) < ihl + 8:
            return None
        body = packet[ihl:]
    elif ethertype == 0x86DD:
        if len(packet) < 40 or packet[6] != 17:
            return None
        body = packet[40:]
    else:
        return None
    length = struct.unpack('>H', body[4:6])[0]
    # UDP length covers the 8-byte header; clamp to what we captured.
    end = min(len(body), max(8, length))
    return body[8:end]


def ax25_call(raw):
    call = ''.join(chr(b >> 1) for b in raw[:6]).strip()
    ssid = (raw[6] >> 1) & 0x0F
    return f"{call}-{ssid}" if ssid else call


def parse_ax25(payload):
    """Return (dst, src, pid, info), or None if not an I/UI frame.

    AXUDP carries the bare AX.25 frame; some peers prefix a 2-byte
    AXUDP header (0x00 0x00), so try both alignments.
    """
    for offset in (0, 2):
        buf = payload[offset:]
        if len(buf) < 2 * AX25_ADDR_LEN + 2:
            continue
        # Address fields end on the byte with bit 0 set; sanity-check
        # that the callsign bytes are plausible shifted ASCII.
        if any(b & 0x01 for b in buf[0:6]):
            continue
        dst = ax25_call(buf[0:7])
        src = ax25_call(buf[7:14])
        if not dst or not src:
            continue
        i = 14
        if not buf[13] & 0x01:
            while i + AX25_ADDR_LEN <= len(buf):
                last = buf[i + 6] & 0x01
                i += AX25_ADDR_LEN
                if last:
                    break
        if i + 2 > len(buf):
            continue
        ctrl = buf[i]
        if ctrl & 0x01 and ctrl != 0x03:      # not an I-frame, not UI
            continue
        return dst, src, buf[i + 1], buf[i + 2:]
    return None


def parse_records(info):
    """Yield (call, ssid_lo, ssid_hi, rtt) from a CE compact batch.

    Mirrors flex_parse_compact_records() in FlexNetCode.c, which is the
    format real peers emit — and it is NOT one record per '3'. A frame
    carries a single leading '3', then repeated groups of

        CALL(6) SSID_LO(1) SSID_HI(1) RTT_DECIMAL

    space-separated, optionally ending in '-' to mark the whole batch as
    a withdrawal (RTT becomes infinity), then CR. Our own emissions are
    one record per frame, so they are simply one-record batches; xnet
    packs many into one frame. Splitting on '\r' and expecting a '3'
    per record parses our own traffic and silently drops every peer's.
    """
    text = info.decode('latin-1')
    if not text or text[0] != '3':
        return
    body = text[1:].rstrip('\r\n')
    withdrawal = False
    body = body.rstrip()
    if body.endswith('-'):
        withdrawal = True
        body = body[:-1].rstrip()

    i = 0
    n = len(body)
    while i < n:
        while i < n and body[i] == ' ':
            i += 1
        if i + 8 > n:
            return
        call = body[i:i + 6].replace(' ', '')
        if not call:
            return
        lo = ord(body[i + 6]) - 0x30
        hi = ord(body[i + 7]) - 0x30
        i += 8
        digits = ''
        while i < n and body[i].isdigit() and len(digits) < 6:
            digits += body[i]
            i += 1
        rtt = 60000 if withdrawal else (int(digits) if digits else 0)
        yield call, max(0, min(15, lo)), max(0, min(15, hi)), rtt


def parse_tokens(info):
    text = info.decode('latin-1')
    return [t for t in ('3+', '3-') if text.startswith(t)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pcap')
    ap.add_argument('--me', required=True,
                    help='our node base call, e.g. IR2UFV')
    ap.add_argument('--pcf', action='append', default=[],
                    help='peer call to judge at the PCF bucket rate '
                         '(repeatable); others use the xnet rate')
    ap.add_argument('--pcf-period', type=float, default=5.0)
    ap.add_argument('--xnet-period', type=float, default=2.0)
    args = ap.parse_args()

    me = args.me.upper().split('-')[0]
    pcf_peers = {p.upper() for p in args.pcf}

    emitted = collections.defaultdict(list)
    received = collections.defaultdict(set)
    tokens = collections.defaultdict(list)
    frames = ce_frames = supervisory = 0

    for ts, linktype, frame in read_pcap(args.pcap):
        ethertype, packet = strip_linklayer(linktype, frame)
        payload = udp_payload(ethertype, packet) if packet else None
        if not payload:
            continue
        frames += 1
        parsed = parse_ax25(payload)
        if not parsed:
            # Supervisory frames (RR/RNR/REJ) carry no PID or info and
            # are the bulk of an idle AXIP link — not a parse failure.
            supervisory += 1
            continue
        dst, src, pid, info = parsed
        if pid != PID_CE or not info:
            continue
        ce_frames += 1
        if src.split('-')[0] == me:
            for rec in parse_records(info):
                emitted[dst].append((ts, rec))
            for tok in parse_tokens(info):
                tokens[dst].append((ts, tok))
        elif dst.split('-')[0] == me:
            for call, lo, hi, _rtt in parse_records(info):
                received[src].add((call, lo, hi))

    print(f"{args.pcap}: {frames} UDP datagrams, {ce_frames} CE frames, "
          f"{supervisory} supervisory/other\n")
    if not emitted:
        print("no outbound CE compact records — check --me against the pcap")
        return

    # Every peer we exchange CE with is a DIRECT neighbour, which we
    # advertise from our own link measurement, not from anything a peer
    # told us. Such a record can never be a split-horizon violation
    # however it looks in the received sets.
    peer_bases = {p.split('-')[0] for p in set(emitted) | set(received)}

    print(f"=== CE compact records EMITTED by {me} ===\n")
    for peer in sorted(emitted):
        recs = emitted[peer]
        span = recs[-1][0] - recs[0][0] if len(recs) > 1 else 0.0
        gaps = [b[0] - a[0] for a, b in zip(recs, recs[1:])]
        dests = {r[1][:3] for r in recs}
        # Withdrawals are exempt from the split-horizon test below:
        # telling a peer "I can no longer reach X" is correct even when
        # X is X's own route, and is in fact REQUIRED — see the D2/D3
        # note in the B6 comment.
        live_dests = {r[1][:3] for r in recs if r[1][3] < 60000}
        poison = [r for r in recs if r[1][3] >= 60000]
        is_pcf = peer.upper() in pcf_peers
        period = args.pcf_period if is_pcf else args.xnet_period

        rate = f"{len(recs) / span * 60:.1f}/min" if span > 0 else "n/a"
        print(f"peer {peer}  [{'PCF' if is_pcf else 'xnet'} bucket, "
              f"1 record / {period:g}s]")
        print(f"  records            : {len(recs)} over {span:.0f}s ({rate})")
        print(f"  distinct dests     : {len(dests)}")
        print(f"  poison (RTT>=60k)  : {len(poison)}")
        if gaps:
            srt = sorted(gaps)
            print(f"  inter-record gap   : min={srt[0]:.2f}s "
                  f"median={srt[len(srt) // 2]:.2f}s max={srt[-1]:.2f}s")
            # B5: the bucket permits records inside one burst (up to the
            # burst size) but never a sustained rate above 1/period.
            fast = [g for g in gaps if g < period * 0.9]
            print(f"  gaps < {period:g}s          : {len(fast)}"
                  f"{'  <-- check against burst size' if fast else ''}")
        tok = collections.Counter(t for _, t in tokens[peer])
        print(f"  tokens             : {dict(tok) or 'none'}")

        # B6 — split-horizon. Advertising a destination back to a peer
        # that also knows it is NOT a violation: with two xnet peers
        # most of the table is learned from both, and the record sent
        # to -14 legitimately carries the path learned from -4. The
        # violation is advertising a destination back to the ONLY peer
        # that could have taught it to us — then the path must be its
        # own. A plain set intersection flags the legitimate case as a
        # failure; it reported 38 and 39 "violations" on a clean run.
        # Poison records must be excluded. When a peer dies, every
        # destination whose only OTHER source was that peer becomes
        # unreachable *from the surviving peer's point of view*, because
        # split-horizon bars us from using the surviving peer's own
        # route — so we correctly send it RTT=60000 for routes it taught
        # us. Measured in the 2026-09-17 D2 run: 57 such withdrawals to
        # IW2OHX-14, which a poison-blind check reports as three
        # "violations" (HB9AK-1, IW2OHX-4, K2PUT-1). Withdrawing a route
        # toward its owner is not advertising it back.
        from_peer = received.get(peer, set())
        elsewhere = set().union(
            *[v for k, v in received.items() if k != peer]) \
            if len(received) > 1 else set()
        echoed = live_dests & from_peer
        # Drop direct neighbours: their source is our own link.
        echoed = {d for d in echoed if d[0] not in peer_bases}
        real = echoed - elsewhere
        print(f"  split-horizon (B6) : "
              f"{len(echoed)} live dests also known by this peer, "
              f"{len(echoed & elsewhere)} with an alternate source"
              + (f" ({len(poison)} withdrawals exempt)" if poison else ""))
        print("                       " +
              ("OK — no destination advertised back to its only source"
               if not real
               else f"SUSPECT: {sorted(real)[:5]}"))
        if real:
            # A short capture can miss a peer's own advertisement of a
            # destination and so under-populate the alternate-source
            # set — most visible right after a peer reconnects, when its
            # re-advertisement trails the window. Re-check over a window
            # that contains a full advertisement cycle from every peer
            # before treating this as real.
            print("                       (verify against a longer "
                  "window — a peer's advert may fall outside this one)")
        print()

    print(f"=== CE compact records RECEIVED by {me} ===")
    for peer in sorted(received):
        print(f"  {peer}: {len(received[peer])} distinct destinations")


if __name__ == '__main__':
    main()
