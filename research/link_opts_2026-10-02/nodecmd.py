#!/usr/bin/env python3
"""nodecmd.py HOST PORT USER PASS WAIT CMD... — telnet in, run commands, print.
Each CMD is sent after WAIT seconds of quiet-ish reading. Use '@N' as a CMD to
sleep N extra seconds."""
import socket, sys, time
host, port, user, pw, wait = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], float(sys.argv[5])
cmds = sys.argv[6:]
s = socket.create_connection((host, port), timeout=10)
s.settimeout(0.3)
buf = b""
def drain(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d: break
            # strip telnet IAC negotiation
            out = bytearray(); i = 0
            while i < len(d):
                if d[i] == 255 and i + 2 < len(d): i += 3; continue
                out.append(d[i]); i += 1
            buf += bytes(out)
        except socket.timeout:
            pass
drain(1.5); s.sendall(user.encode() + b"\r\n")
drain(1.5); s.sendall(pw.encode() + b"\r\n")
drain(2.0)
for c in cmds:
    if c.startswith('@'):
        drain(float(c[1:])); continue
    s.sendall(c.encode() + b"\r")
    drain(wait)
s.close()
sys.stdout.write(buf.decode('latin-1').replace('\r', '\n'))
