#!/usr/bin/env python3
"""Watch BPQ's global semaphore and name whoever holds it too long.

The node's ~1 s stalls burn no CPU and sit in hrtimer_nanosleep, which
is exactly what `_GetSemaphore`'s `while (Flag) Sleep(10)` looks like:
the main loop is locked out, so no port gets polled and inbound AX.25
frames sit unread until the holder lets go. `struct SEM` records the
holder's file and line, so this turns "the node was asleep" into a call
site.

Read-only: one open of /proc/<pid>/mem, no ptrace stop.

usage: sem_watch.py PID BINARY OUT_LOG SECONDS [MIN_HOLD_MS] [HZ]
"""
import os
import struct
import subprocess
import sys
import time

pid, binary, out, secs = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
min_hold = (float(sys.argv[5]) if len(sys.argv) > 5 else 60.0) / 1000.0
hz = float(sys.argv[6]) if len(sys.argv) > 6 else 200.0

sym = subprocess.run(["nm", "-a", binary], capture_output=True, text=True).stdout
addr = next((int(l.split()[0], 16) for l in sym.splitlines()
             if len(l.split()) == 3 and l.split()[2] == "Semaphore"), None)
if addr is None:
    sys.exit("Semaphore symbol not found")
base = next((int(l.split("-")[0], 16) for l in open(f"/proc/{pid}/maps")
             if binary in l and l.split()[1].startswith("r-x")), None)
if base is None:
    sys.exit("binary not mapped")
target = base + addr

# struct SEM, offsets from the binary's own debug info:
#   0 Flag  4 Clashes  8 Gets  12 Rels  16 SemProcessID  24 SemThreadID
#   32 Line  36 File[250]
SIZE = 100
period = 1.0 / hz
end = time.time() + secs

mem = open(f"/proc/{pid}/mem", "rb", buffering=0)
log = open(out, "a", buffering=1)
log.write(f"# sem_watch start {time.time():.3f} pid={pid} "
          f"min_hold={min_hold*1000:.0f}ms hz={hz:.0f}\n")

held_since = None
held_id = None
last_clashes = None

while time.time() < end:
    t = time.time()
    try:
        mem.seek(target)
        blob = mem.read(SIZE)
    except OSError:
        break
    if not blob or len(blob) < 40:
        break
    flag, clashes, gets, rels, procid = struct.unpack_from("<IiiiI", blob, 0)
    thread, = struct.unpack_from("<Q", blob, 24)
    line, = struct.unpack_from("<i", blob, 32)
    fname = blob[36:].split(b"\x00")[0].decode("ascii", "replace")
    who = f"{fname}:{line} thread=0x{thread:x}"

    if last_clashes is None:
        last_clashes = clashes
    elif clashes != last_clashes:
        log.write(f"{t:.3f} CLASH total={clashes} (+{clashes-last_clashes}) "
                  f"holder={who}\n")
        last_clashes = clashes

    if flag:
        if held_since is None:
            held_since, held_id = t, who
        elif who != held_id:
            # holder changed without us seeing a gap: treat as a new hold
            held_since, held_id = t, who
    else:
        if held_since is not None:
            dur = t - held_since
            if dur >= min_hold:
                log.write(f"{t:.3f} HOLD {dur*1000:8.1f} ms by {held_id}\n")
            held_since = held_id = None

    time.sleep(max(0.0, period - (time.time() - t)))

log.write(f"# sem_watch end {time.time():.3f}\n")
