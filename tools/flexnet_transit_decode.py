#!/usr/bin/env python3
"""
flexnet_transit_decode.py — decode a capture taken ON a FlexNet transit
node and show what it did to each frame.

Built for the undocumented part of FlexNet: what a real router does when
it forwards for someone else. `flexnetd/PROTOCOL_SPEC.md` §5.1 lists the
two *legal* patterns (pure L2 digipeat, or L3 forwarding with a fresh L2
session per hop) but not which one the real routers use, and §3.3's CREQ
description was written from partial captures.

Reads pcapng (what Windows `pktmon etl2pcap` produces — the transit node
here is PC/Flexnet on Windows, where pktmon is the only capture tool)
as well as classic pcap, and prints each AX.25 frame grouped by UDP port.
Each flexkrnl UDP port is one FlexNet peer link, so the port tells you
which side of the transit node you are looking at: the same user frame
appearing on two ports, with the addresses or the envelope changed
between them, is the forwarding behaviour itself.

    ./flexnet_transit_decode.py cap.pcapng
    ./flexnet_transit_decode.py cap.pcapng --follow IGATE
"""

import argparse
import struct
import sys
from collections import Counter, defaultdict

PID_NAMES = {0xCE: "CE(flex)", 0xCF: "CF(netrom)", 0xF0: "F0(text)",
             0xCD: "CD", 0x08: "08"}


# ── container readers ───────────────────────────────────────────────────
def read_pcapng(data):
    """Yield (ts, linktype, frame) from a pcapng byte string."""
    off, linktypes, endian = 0, {}, "<"
    n_if = 0
    while off + 12 <= len(data):
        btype, blen = struct.unpack(endian + "II", data[off:off + 8])
        if blen < 12 or off + blen > len(data):
            break
        body = data[off + 8:off + blen - 4]
        if btype == 0x0A0D0D0A:                      # section header
            if struct.unpack("<I", body[:4])[0] != 0x1A2B3C4D:
                endian = ">"
            n_if = 0
        elif btype == 0x00000001:                    # interface description
            linktypes[n_if] = struct.unpack(endian + "H", body[:2])[0]
            n_if += 1
        elif btype == 0x00000006:                    # enhanced packet
            iface, tsh, tsl, cap, _orig = struct.unpack(endian + "IIIII",
                                                        body[:20])
            ts = ((tsh << 32) | tsl) / 1e6
            yield ts, linktypes.get(iface, 1), body[20:20 + cap]
        off += blen


def read_pcap(data):
    magic = struct.unpack("<I", data[:4])[0]
    end = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    nanos = magic in (0xA1B23C4D, 0x4D3CB2A1)
    linktype = struct.unpack(end + "I", data[20:24])[0]
    off = 24
    while off + 16 <= len(data):
        s, f, cap, _o = struct.unpack(end + "IIII", data[off:off + 16])
        off += 16
        yield s + f / (1e9 if nanos else 1e6), linktype, data[off:off + cap]
        off += cap


def frames(path):
    data = open(path, "rb").read()
    if data[:4] == b"\x0a\x0d\x0d\x0a":
        yield from read_pcapng(data)
    else:
        yield from read_pcap(data)


# ── layers ──────────────────────────────────────────────────────────────
def udp_of(linktype, frame):
    """Return (sport, dport, payload) or None."""
    if linktype == 1:                                # Ethernet
        if len(frame) < 14:
            return None
        et = struct.unpack(">H", frame[12:14])[0]
        pkt = frame[14:]
    elif linktype == 101:
        et, pkt = 0x0800, frame
    elif linktype == 113:
        et, pkt = struct.unpack(">H", frame[14:16])[0], frame[16:]
    elif linktype == 276:
        et, pkt = struct.unpack(">H", frame[0:2])[0], frame[20:]
    else:
        return None

    if et == 0x0800:
        if len(pkt) < 20 or pkt[9] != 17:
            return None
        body = pkt[(pkt[0] & 0x0F) * 4:]
    elif et == 0x86DD:
        if len(pkt) < 40 or pkt[6] != 17:
            return None
        body = pkt[40:]
    else:
        return None
    if len(body) < 8:
        return None
    sport, dport, ulen = struct.unpack(">HHH", body[:6])
    return sport, dport, body[8:max(8, min(len(body), ulen))]


def ax25_call(raw):
    call = "".join(chr(b >> 1) for b in raw[:6]).strip()
    ssid = (raw[6] >> 1) & 0x0F
    return f"{call}-{ssid}" if ssid else call


def ctl_name(ctl):
    if (ctl & 1) == 0:
        return f"I  S{(ctl >> 1) & 7} R{(ctl >> 5) & 7}"
    if (ctl & 3) == 1:
        return {0x01: "RR", 0x05: "RNR", 0x09: "REJ"}.get(ctl & 0x0F, "S?") \
               + f" R{(ctl >> 5) & 7}"
    return {0x2F: "SABM", 0x3F: "SABME", 0x43: "DISC", 0x0F: "DM",
            0x63: "UA", 0x87: "FRMR", 0x03: "UI", 0xAF: "XID",
            0xE3: "TEST"}.get(ctl & ~0x10, f"U(0x{ctl:02X})")


def decode_ax25(payload):
    """Decode an AXUDP payload. Returns a dict or None."""
    for base in (0, 2):                              # some peers prefix 2 bytes
        b = payload[base:]
        if len(b) < 15 or any(x & 1 for x in b[0:6]):
            continue
        dst, src = ax25_call(b[0:7]), ax25_call(b[7:14])
        if not dst or not src:
            continue
        digis, i = [], 14
        if not b[13] & 1:
            while i + 7 <= len(b):
                digis.append((ax25_call(b[i:i + 7]), bool(b[i + 6] & 0x80)))
                last = b[i + 6] & 1
                i += 7
                if last:
                    break
        if i >= len(b):
            continue
        ctl = b[i]
        pid = b[i + 1] if (ctl & 1) == 0 or ctl in (0x03, 0x13) else None
        info = b[i + 2:] if pid is not None else b""
        return {"dst": dst, "src": src, "digis": digis, "ctl": ctl,
                "ctl_s": ctl_name(ctl), "pid": pid, "info": info}
    return None


def digi_str(digis):
    return " ".join(c + ("*" if h else "") for c, h in digis) or "-"


def cf_summary(info):
    """Best-effort NetROM/FlexNet L3 summary (PROTOCOL_SPEC §3)."""
    if len(info) < 15:
        return ""
    src, dst = ax25_call(info[0:7]), ax25_call(info[7:14])
    ttl = info[14]
    out = f"L3 {src}->{dst} ttl={ttl}"
    if len(info) >= 20:
        idx, sid, txs, rxs, op = info[15], info[16], info[17], info[18], info[19]
        opn = {1: "CREQ", 2: "CACK", 3: "DREQ", 4: "DACK",
               5: "INFO", 6: "IACK"}.get(op & 0x0F, f"op{op & 0x0F:X}")
        out += f" idx={idx} id={sid} S{txs} R{rxs} {opn}"
        if (op & 0x0F) == 1 and len(info) >= 34:
            out += f" user={ax25_call(info[20:27])} node={ax25_call(info[27:34])}"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--follow", help="only frames mentioning this callsign")
    ap.add_argument("--ports", help="comma list of UDP ports to include")
    ap.add_argument("--no-ce", action="store_true",
                    help="hide PID=CE routing chatter (keepalives, records)")
    args = ap.parse_args()

    want = {int(p) for p in args.ports.split(",")} if args.ports else None
    rows, stats, per_port = [], Counter(), defaultdict(Counter)
    # pktmon logs each packet once per monitored component (NIC, filter
    # driver, ...), so the same frame arrives 5-8 times with the same
    # timestamp. Without this every count is inflated by that factor.
    seen = set()

    for ts, lt, frame in frames(args.pcap):
        u = udp_of(lt, frame)
        if not u:
            continue
        sport, dport, payload = u
        if want and sport not in want and dport not in want:
            continue
        key = (round(ts, 4), sport, dport, payload)
        if key in seen:
            stats["dup"] += 1
            continue
        seen.add(key)
        stats["udp"] += 1
        f = decode_ax25(payload)
        if not f:
            stats["undecoded"] += 1
            continue
        stats["ax25"] += 1
        link = min(sport, dport)          # flexkrnl's port = the peer link
        per_port[link][PID_NAMES.get(f["pid"], str(f["pid"]))] += 1

        if args.no_ce and f["pid"] == 0xCE:
            continue
        text = f"{f['src']}->{f['dst']}"
        if args.follow:
            hay = " ".join([f["src"], f["dst"]] + [c for c, _ in f["digis"]])
            if args.follow.upper() not in hay.upper():
                continue
        extra = ""
        if f["pid"] == 0xCF and f["dst"] not in ("NODES", "QST", "ID"):
            extra = cf_summary(f["info"])
        elif f["pid"] == 0xCF:
            extra = f"<{f['dst']} broadcast, {len(f['info'])}B>"
        elif f["pid"] == 0xF0 and f["info"]:
            extra = repr(f["info"][:40].decode("latin-1"))
        elif f["pid"] == 0xCE and f["info"]:
            # CE payloads are short ASCII control strings; the exact byte
            # count matters (a 3-byte "1n\r" and a longer "1nn \r" are
            # different frame TYPES to a FlexNet parser), so show both.
            body = f["info"]
            if len(body) > 24 and body[1:].strip(b" ") == b"":
                extra = f"KEEPALIVE {len(body)}B"      # '2' + padding
            else:
                extra = f"{len(body)}B {body[:24].decode('latin-1')!r}"
        rows.append((ts, sport, dport, text, digi_str(f["digis"]),
                     f["ctl_s"], PID_NAMES.get(f["pid"], "-"), extra))

    if not rows:
        print("no matching frames")
    else:
        t0 = rows[0][0]
        print(f"{'t+':>7}  {'sport':>5}->{'dport':<5} "
              f"{'src->dst':<26} {'digis':<24} {'ctl':<10} {'pid':<10} info")
        print("-" * 130)
        for ts, sp, dp, text, dg, ctl, pid, extra in rows:
            print(f"{ts - t0:7.3f}  {sp:>5}->{dp:<5} {text:<26} {dg:<24} "
                  f"{ctl:<10} {pid:<10} {extra}")

    print(f"\nunique-udp={stats['udp']} ax25={stats['ax25']} "
          f"undecoded={stats['undecoded']} pktmon-dups-dropped={stats['dup']}")
    print("per peer-link (flexkrnl UDP port):")
    for p in sorted(per_port):
        inner = "  ".join(f"{k}={v}" for k, v in sorted(per_port[p].items()))
        print(f"  port {p}: {inner}")


if __name__ == "__main__":
    main()
