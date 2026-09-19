#!/usr/bin/env python3
"""Full AX.25 frame trace with command/response bits, for one peer.

axudp_teardown.py deliberately ignores the C bits; an RR burst cannot be
read without them, because "poll" and "answer to a poll" look identical
on the P/F bit alone.

usage: frames.py CAPTURE LOCAL_IP PEER_IP [SINCE_ISO] [UNTIL_ISO]
"""
import importlib.util, os, sys
from datetime import datetime, timezone

_here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "td", os.path.join(_here, "axudp_teardown.py"))
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)

path, local, peer = sys.argv[1], sys.argv[2], sys.argv[3]
since = sys.argv[4] if len(sys.argv) > 4 else None
until = sys.argv[5] if len(sys.argv) > 5 else None


def to_ts(s):
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(
        tzinfo=timezone.utc).timestamp()


t0 = to_ts(since) if since else None
t1 = to_ts(until) if until else None

U = {0x2F: "SABM", 0x6F: "SABME", 0x43: "DISC", 0x0F: "DM", 0x63: "UA",
     0x87: "FRMR", 0x03: "UI", 0xAF: "XID", 0xE3: "TEST"}
S = {0x01: "RR", 0x05: "RNR", 0x09: "REJ", 0x0D: "SREJ"}

for ts, linktype, data in td.read_pcap(path):
    if t0 and ts < t0:
        continue
    if t1 and ts >= t1:
        break
    ip = td.strip_link_header(linktype, data)
    if ip is None:
        continue
    u = td.parse_udp(ip)
    if u is None:
        continue
    src_ip, dst_ip, payload = u[0], u[1], u[4]
    if peer not in (src_ip, dst_ip):
        continue
    f = td.decode_ax25(payload)
    if not f:
        continue
    if f["digis"]:
        continue                      # link frames only
    # C bits: dest SSID bit 7 and src SSID bit 7. 1/0 = command, 0/1 = response.
    dest_c = (payload[6] & 0x80) != 0
    src_c = (payload[13] & 0x80) != 0
    cr = "CMD" if (dest_c and not src_c) else (
        "RSP" if (src_c and not dest_c) else "v1 ")
    ctl = f["ctl"]
    pf = "P/F" if (ctl & 0x10) else "   "
    if (ctl & 0x01) == 0:
        kind = f"I   N(S)={(ctl >> 1) & 7} N(R)={(ctl >> 5) & 7}"
    elif (ctl & 0x03) == 0x01:
        kind = f"{S.get(ctl & 0x0F, hex(ctl)):<4}       N(R)={(ctl >> 5) & 7}"
    else:
        kind = f"{U.get(ctl & 0xEF, hex(ctl)):<4}"
    d = "Out" if src_ip == local else "In "
    ts_s = datetime.fromtimestamp(ts, timezone.utc).strftime("%H:%M:%S.%f")[:-3]
    print(f"{ts_s} {d} {cr} {pf} {kind:<22} {f['src']}>{f['dest']} len={len(payload)}")
