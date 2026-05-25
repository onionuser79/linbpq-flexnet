#!/usr/bin/env python3
"""Decode a libpcap of AXUDP frames (BPQAXIP UDP encapsulation) and
classify each AX.25 frame: direction, control type (I/S/U + subtype),
PID (if any), and a short rendering of the info field for I-frames.

Reads:  argv[1] = pcap path
Writes: human-readable table to stdout
"""
from __future__ import annotations

import struct
import sys
from datetime import datetime, timezone

# libpcap magic numbers
PCAP_MAGIC = 0xa1b2c3d4        # us-resolution, little-endian read
PCAP_MAGIC_REV = 0xd4c3b2a1
PCAP_MAGIC_NS = 0xa1b23c4d     # nanosecond
PCAP_MAGIC_NS_REV = 0x4d3cb2a1

LINKTYPE_LINUX_SLL2 = 276
LINKTYPE_LINUX_SLL = 113
LINKTYPE_ETHERNET = 1


def decode_call(b: bytes) -> tuple[str, int, int, int]:
    """Return (callsign-w-ssid, ssid, c_bit, e_bit) from a 7-byte AX.25 addr."""
    assert len(b) == 7
    cs = "".join(chr((c >> 1) & 0x7f) for c in b[:6]).rstrip()
    ssid_byte = b[6]
    ssid = (ssid_byte >> 1) & 0x0f
    c_bit = (ssid_byte >> 7) & 0x01
    e_bit = ssid_byte & 0x01
    tag = f"{cs}-{ssid}" if ssid else cs
    return tag, ssid, c_bit, e_bit


def decode_ax25(payload: bytes) -> dict:
    """Decode an AX.25 frame. Assumes BPQAXIP-style payload incl. 2-byte FCS."""
    if len(payload) < 16:
        return {"err": "too short"}
    info = {}
    dest, _, dc, _de = decode_call(payload[0:7])
    src, _, sc, src_e = decode_call(payload[7:14])
    info["dest"] = dest
    info["src"] = src
    cmd_resp = "?"
    if dc == 1 and sc == 0:
        cmd_resp = "C"   # command (V2)
    elif dc == 0 and sc == 1:
        cmd_resp = "R"   # response (V2)
    elif dc == sc:
        cmd_resp = "V1"
    info["cr"] = cmd_resp

    # Digipeater path: present iff source E-bit is 0. (Dest E-bit is always 0
    # in V2 frames; the END marker lives on the *last* address, which is
    # either the source if no digis or the last digi otherwise.)
    pos = 14
    digis = []
    if src_e == 0:
        while pos + 7 <= len(payload):
            tag, _, h_bit, e2 = decode_call(payload[pos:pos+7])
            digis.append((tag, h_bit))
            pos += 7
            if e2 == 1:
                break
    info["digis"] = digis

    if pos >= len(payload):
        return {**info, "err": "no control"}
    ctrl = payload[pos]
    pos += 1
    info["ctrl"] = ctrl

    # Classify by low 2 bits.
    if (ctrl & 0x01) == 0:
        # I-frame
        ns = (ctrl >> 1) & 0x07
        nr = (ctrl >> 5) & 0x07
        pf = (ctrl >> 4) & 0x01
        info["type"] = "I"
        info["ns"] = ns
        info["nr"] = nr
        info["pf"] = pf
        if pos < len(payload):
            info["pid"] = payload[pos]
            pos += 1
            # Strip 2-byte FCS at the tail.
            tail = payload[pos:]
            if len(tail) >= 2:
                tail = tail[:-2]
            info["info"] = tail
    elif (ctrl & 0x03) == 0x01:
        # S-frame
        sub = (ctrl >> 2) & 0x03
        nr = (ctrl >> 5) & 0x07
        pf = (ctrl >> 4) & 0x01
        info["type"] = "S"
        info["sub"] = {0: "RR", 1: "RNR", 2: "REJ", 3: "SREJ"}.get(sub, f"S?{sub}")
        info["nr"] = nr
        info["pf"] = pf
    elif (ctrl & 0x03) == 0x03:
        # U-frame
        mask = ctrl & 0xef  # strip P/F
        pf = (ctrl >> 4) & 0x01
        info["type"] = "U"
        info["pf"] = pf
        info["sub"] = {
            0x2f: "SABM",
            0x6f: "SABME",
            0x43: "DISC",
            0x0f: "DM",
            0x63: "UA",
            0x87: "FRMR",
            0x03: "UI",
            0xaf: "XID",
            0xe3: "TEST",
        }.get(mask, f"U?{mask:02x}")
        # UI carries PID + info
        if mask == 0x03 and pos < len(payload):
            info["pid"] = payload[pos]
            pos += 1
            tail = payload[pos:]
            if len(tail) >= 2:
                tail = tail[:-2]
            info["info"] = tail
    return info


def render_info(b: bytes) -> str:
    """Compact ASCII/hex rendering of an info field."""
    if not b:
        return ""
    # Detect ASCII-with-trailing-fill patterns and abbreviate.
    if len(b) >= 4 and b[0:1] == b"2" and all(c == 0x20 for c in b[1:-1]):
        last = b[-1]
        tail = "CR" if last == 0x0d else f"0x{last:02x}"
        return f"'2'+{len(b)-2}sp+{tail}"
    if len(b) >= 4 and b[0:1] == b"2" and all(c == 0x20 for c in b[1:]):
        return f"'2'+{len(b)-1}sp+NONE"
    # Otherwise print printable ASCII run; mark non-printables.
    printable = all(0x20 <= c < 0x7f or c in (0x0a, 0x0d) for c in b)
    if printable:
        return repr(b.decode("latin-1")).strip("'")
    # Mixed — show up to 32 bytes hex + ASCII suffix.
    head = " ".join(f"{c:02x}" for c in b[:32])
    asc = "".join(chr(c) if 0x20 <= c < 0x7f else "." for c in b[:32])
    return f"hex[{head}] '{asc}'{'...' if len(b) > 32 else ''}"


def parse_pcap(path: str):
    with open(path, "rb") as f:
        hdr = f.read(24)
        magic = struct.unpack("<I", hdr[:4])[0]
        if magic == PCAP_MAGIC or magic == PCAP_MAGIC_NS:
            endian = "<"
            ns = (magic == PCAP_MAGIC_NS)
        elif magic == PCAP_MAGIC_REV or magic == PCAP_MAGIC_NS_REV:
            endian = ">"
            ns = (magic == PCAP_MAGIC_NS_REV)
        else:
            raise ValueError(f"unknown magic {magic:08x}")
        link = struct.unpack(f"{endian}I", hdr[20:24])[0]
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                break
            ts_sec, ts_sub, incl, orig = struct.unpack(f"{endian}IIII", rh)
            data = f.read(incl)
            if len(data) < incl:
                break
            ts = ts_sec + (ts_sub / 1_000_000_000 if ns else ts_sub / 1_000_000)
            yield ts, link, data


def extract_udp_payload(link: int, frame: bytes):
    """Return (src_ip, dst_ip, sport, dport, udp_payload) or None."""
    # Skip link-layer header to find IPv4 packet.
    if link == LINKTYPE_LINUX_SLL2:
        # SLL2 header is 20 bytes. proto at offset 0 (2 bytes).
        if len(frame) < 20:
            return None
        eth_proto = struct.unpack(">H", frame[0:2])[0]
        ip_off = 20
    elif link == LINKTYPE_LINUX_SLL:
        if len(frame) < 16:
            return None
        eth_proto = struct.unpack(">H", frame[14:16])[0]
        ip_off = 16
    elif link == LINKTYPE_ETHERNET:
        if len(frame) < 14:
            return None
        eth_proto = struct.unpack(">H", frame[12:14])[0]
        ip_off = 14
    else:
        return None
    if eth_proto != 0x0800:
        return None
    ip = frame[ip_off:]
    if len(ip) < 20:
        return None
    ihl = (ip[0] & 0x0f) * 4
    proto = ip[9]
    if proto != 17:
        return None
    src_ip = ".".join(str(b) for b in ip[12:16])
    dst_ip = ".".join(str(b) for b in ip[16:20])
    udp = ip[ihl:]
    if len(udp) < 8:
        return None
    sport, dport, ulen, _ = struct.unpack(">HHHH", udp[:8])
    payload = udp[8:ulen]
    return src_ip, dst_ip, sport, dport, payload


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "snapshot.pcap"
    counters = {"I": 0, "S": 0, "U": 0, "err": 0}
    sub_counters: dict[str, int] = {}
    dir_counters: dict[str, int] = {}
    info_renders: list[str] = []
    disc_events = []
    last_ts = None
    for ts, link, frame in parse_pcap(path):
        udp = extract_udp_payload(link, frame)
        if not udp:
            continue
        src_ip, dst_ip, sp, dp, payload = udp
        info = decode_ax25(payload)
        direction = f"{src_ip}->{dst_ip}"
        dir_counters[direction] = dir_counters.get(direction, 0) + 1
        if "type" not in info:
            counters["err"] += 1
            continue
        t = info["type"]
        counters[t] += 1
        sub = info.get("sub", "?")
        sub_counters[f"{t}:{sub}"] = sub_counters.get(f"{t}:{sub}", 0) + 1
        wall = datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%H:%M:%S.%f")[:-3]
        dt = f"{ts - last_ts:+6.2f}s" if last_ts else "       "
        last_ts = ts
        # Compact direction tag.
        if src_ip == "192.168.1.202":
            dtag = "IR2UFV→-12"
        elif src_ip == "192.168.1.201":
            dtag = " -12→IR2UFV"
        else:
            dtag = f"{src_ip}→{dst_ip}"

        if t == "I":
            pid = info.get("pid", 0)
            rendered = render_info(info.get("info", b""))
            line = f"{wall} {dt} {dtag} I N(S)={info['ns']} N(R)={info['nr']} P={info['pf']} PID=0x{pid:02x} info({len(info.get('info', b''))}B)={rendered}"
        elif t == "S":
            line = f"{wall} {dt} {dtag} S {info['sub']} N(R)={info['nr']} P/F={info['pf']}"
        elif t == "U":
            sub = info.get("sub", "?")
            line = f"{wall} {dt} {dtag} U {sub} P/F={info['pf']}"
            if sub in ("DISC", "SABM", "DM"):
                disc_events.append((wall, dtag, sub))
        info_renders.append(line)

    print("=" * 78)
    print(f"frames decoded: {sum(counters.values())} "
          f"(I={counters['I']} S={counters['S']} U={counters['U']} err={counters['err']})")
    print()
    print("by direction:")
    for k, v in sorted(dir_counters.items()):
        print(f"  {k:36s} {v:6d}")
    print()
    print("by control subtype:")
    for k in sorted(sub_counters):
        print(f"  {k:14s} {sub_counters[k]:6d}")
    print()
    print(f"connection-management events: {len(disc_events)}")
    for ts, dt, sub in disc_events:
        print(f"  {ts} {dt} {sub}")
    print()
    print("frames (chronological):")
    for line in info_renders:
        print("  " + line)


if __name__ == "__main__":
    sys.exit(main())
