#!/usr/bin/env python3
"""Minimal AX.25-over-UDP (AXUDP) station for forwarding tests.

Answers one callsign: SABM -> UA, then one I-frame banner naming the caller
and the path it arrived on, then DISC. Every frame in and out is logged with
a timestamp, so a test can show that a connect reached *this* station and
not something else that happens to share the callsign.

    axudp_responder.py --call DEST-5 --listen 127.0.0.1:10199 --log /tmp/r.log

The node side needs `MAP DEST-5 127.0.0.1 UDP 10199` on its AXUDP port.
Replies go to the source address of the frame being answered.
"""
import argparse
import socket
import sys
import time

SABM, UA, DISC, DM = 0x2F, 0x63, 0x43, 0x0F
PF = 0x10


def fcs(data: bytes) -> int:
    """AX.25 FCS: CRC-16/X-25, appended low byte first."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def enc_call(call: str, last: bool, high: bool) -> bytes:
    base, _, ssid = call.upper().partition("-")
    out = bytes((ord(c) << 1) for c in base.ljust(6)[:6])
    b = 0x60 | ((int(ssid or 0) & 0x0F) << 1)
    if high:
        b |= 0x80
    if last:
        b |= 0x01
    return out + bytes([b])


def dec_call(a: bytes) -> str:
    base = "".join(chr(x >> 1) for x in a[:6]).strip()
    ssid = (a[6] >> 1) & 0x0F
    return f"{base}-{ssid}" if ssid else base


def parse(frame: bytes):
    addrs = []
    i = 0
    while i + 7 <= len(frame):
        addrs.append(frame[i:i + 7])
        i += 7
        if addrs[-1][6] & 0x01:
            break
    if len(addrs) < 2 or i >= len(frame):
        return None
    return {
        "dest": dec_call(addrs[0]),
        "src": dec_call(addrs[1]),
        "digis": [(dec_call(a), bool(a[6] & 0x80)) for a in addrs[2:]],
        "ctl": frame[i],
        "info": frame[i + 2:] if (frame[i] & 0x01) == 0 else b"",
    }


def build(dest, src, digis, ctl, info=b"", pid=None, command=False):
    """Address field for a frame from `src` to `dest` via `digis` (none
    repeated yet). C bits: command = dest 1 / src 0, response = the reverse."""
    calls = [dest, src] + digis
    out = b""
    for n, c in enumerate(calls):
        last = n == len(calls) - 1
        high = (n == 0 and command) or (n == 1 and not command)
        out += enc_call(c, last, high)
    out += bytes([ctl])
    if pid is not None:
        out += bytes([pid]) + info
    return out + fcs(out).to_bytes(2, "little")


def describe(p):
    path = ",".join(c + ("*" if h else "") for c, h in p["digis"])
    return f'{p["src"]}>{p["dest"]}{(" via " + path) if path else ""} ctl=0x{p["ctl"]:02X}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--call", required=True)
    ap.add_argument("--listen", required=True, help="host:port")
    ap.add_argument("--log", required=True)
    ap.add_argument("--hold", type=float, default=3.0,
                    help="seconds between banner and DISC")
    a = ap.parse_args()
    host, port = a.listen.rsplit(":", 1)
    me = a.call.upper()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, int(port)))
    log = open(a.log, "a", buffering=1)

    def say(msg):
        log.write(time.strftime("%Y-%m-%dT%H:%M:%S ") + msg + "\n")

    say(f"listening {a.listen} as {me}")
    sessions = {}   # peer call -> dict(addr, digis, disc_at, vr)
    sock.settimeout(0.5)
    while True:
        now = time.time()
        for peer, s in list(sessions.items()):
            if s["disc_at"] and now >= s["disc_at"]:
                f = build(peer, me, s["digis"], DISC | PF, command=True)
                sock.sendto(f, s["addr"])
                say(f"TX DISC to {peer}")
                s["disc_at"] = 0
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < 17 or fcs(data[:-2]) != int.from_bytes(data[-2:], "little"):
            say(f"RX bad frame/CRC from {addr} len={len(data)}")
            continue
        p = parse(data[:-2])
        if not p:
            continue
        say(f"RX {describe(p)} from {addr[0]}:{addr[1]}")
        if p["dest"] != me:
            continue
        back = [c for c, _ in reversed(p["digis"])]
        ctl = p["ctl"]
        pf = ctl & PF
        peer = p["src"]
        if (ctl & ~PF) == SABM:
            sock.sendto(build(peer, me, back, UA | pf), addr)
            path = ",".join(c for c, _ in p["digis"]) or "direct"
            text = (f"{me} AXUDP test responder: connected, caller {peer}, "
                    f"path {path}\r").encode()
            sock.sendto(build(peer, me, back, 0x00, text, pid=0xF0,
                              command=True), addr)
            sessions[peer] = {"addr": addr, "digis": back,
                              "disc_at": time.time() + a.hold}
            say(f"TX UA + banner to {peer} via {back}")
        elif (ctl & ~PF) == DISC:
            sock.sendto(build(peer, me, back, UA | pf), addr)
            sessions.pop(peer, None)
            say(f"TX UA (DISC) to {peer}")
        elif (ctl & ~PF) in (UA, DM):
            sessions.pop(peer, None)
        elif (ctl & 0x01) == 0 and peer in sessions:      # I-frame: ack it
            nr = ((ctl >> 1) + 1) & 7
            sock.sendto(build(peer, me, back, 0x01 | (nr << 5) | pf), addr)
        elif (ctl & 0x03) == 0x01 and pf and peer in sessions:   # RR poll
            sock.sendto(build(peer, me, back, 0x01 | (0 << 5) | PF), addr)
        elif peer not in sessions and pf and (ctl & ~PF) != DM:
            sock.sendto(build(peer, me, back, DM | PF), addr)


if __name__ == "__main__":
    sys.exit(main())
