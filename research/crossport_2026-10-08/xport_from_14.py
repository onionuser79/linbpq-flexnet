#!/usr/bin/env python3
"""v2.6 cross-port test: on (X)Net IW2OHX-14, C IW2OHX-13 IR2UFV.
IR2UFV receives the SABM on AXUDP port 2; -13 is its neighbour on KISS
port 3, so the frame must cross ports and every reply cross back.
usage: xport_from_14.py <pw> [hold-s]"""
import sys
import time

sys.path.insert(0, "/home/iw2ohx/xnet_investigation_agent/linbpq-flexnet/tools")
from xnet_agent import XnetSession, authenticate  # noqa: E402


def stamp(msg):
    print(time.strftime("%H:%M:%S", time.gmtime()), msg, flush=True)


def drain(sess, secs):
    end, buf = time.time() + secs, ""
    while time.time() < end:
        c = sess.read_monitor_chunk(timeout=0.3)
        if c:
            buf += c
    for line in buf.replace("\r", "\n").split("\n"):
        if line.strip():
            stamp("  | " + line.rstrip()[:140])
    return buf


pw = sys.argv[1]                      # credentials from the caller, never in git
hold = int(sys.argv[2]) if len(sys.argv) > 2 else 15
s = XnetSession("44.134.24.2", 23)
s.connect()
authenticate(s, "iw7eas-1", pw, pw.upper())
stamp("on IW2OHX-14: C IW2OHX-13 IR2UFV")
s.send("C IW2OHX-13 IR2UFV")
out = drain(s, 25)
stamp("CONNECTED" if "IW2OHX-13" in out and "failure" not in out.lower() else "NOT CONNECTED")
s.send("V")
drain(s, hold)
stamp("teardown: B")
s.send("B")
drain(s, 8)
s.send("B")
drain(s, 2)
s.close()
stamp("done")
