#!/usr/bin/env python3
"""Loop-free 2-hop transit through IR2UFV, driven from IW2OHX-4:
   (on -4) C IW2OHX-13 IR2UFV  ->  IR2UFV sees [IW2OHX-4* IR2UFV], its
   route to -13 is via IW2OHX-14 (not in the chain) -> it must APPEND -14,
   and contract every reply. usage: transit_from_4.py <pw> [hold-s]"""
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


pw = sys.argv[1]
hold = int(sys.argv[2]) if len(sys.argv) > 2 else 15
s = XnetSession("44.134.24.3", 23)
s.connect()
authenticate(s, "iw7eas-2", pw, pw.upper())
stamp("on IW2OHX-4: C IW2OHX-13 IR2UFV")
s.send("C IW2OHX-13 IR2UFV")
out = drain(s, 20)
stamp("CONNECTED" if "IW2OHX-13" in out and "failure" not in out else "NOT CONNECTED")
s.send("V")                     # one command each way through the circuit
drain(s, hold)
stamp("teardown: B to IW2OHX-13 (far side sends the DISC)")
s.send("B")
drain(s, 8)
s.send("B")
drain(s, 2)
s.close()
stamp("done")
