#!/usr/bin/env python3
"""Catch the node mid-stall and name the call site with a real backtrace.

`sem_watch.py` proved the main loop holds BPQ's global `Semaphore` for a
flat ~1002 ms, roughly once a minute, from `LinBPQ.c:1790` — i.e. inside
`Semaphored100msCode()`. That names the *acquirer*, not the blocking
callee. This watches the same flag and, the moment a hold runs past a
threshold no healthy hold ever reaches (measured: every hold >= 60 ms in
3 h was one of these events), attaches gdb for one batch backtrace.

Trigger low: the whole event is ~1 s, and gdb needs a few hundred ms to
attach, so waiting for 300 ms leaves too little margin.

usage: sem_stack.py PID BINARY OUT_LOG SECONDS [TRIG_MS] [MAX_CATCHES]
"""
import os
import struct
import subprocess
import sys
import time

pid, binary, out, secs = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
trig = (float(sys.argv[5]) if len(sys.argv) > 5 else 80.0) / 1000.0
max_catches = int(sys.argv[6]) if len(sys.argv) > 6 else 8

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

SIZE = 100
end = time.time() + secs
mem = open(f"/proc/{pid}/mem", "rb", buffering=0)
log = open(out, "a", buffering=1)
log.write(f"# sem_stack start {time.time():.3f} pid={pid} trig={trig*1000:.0f}ms\n")


def snapshot(tag):
    """Cheap, no-stop evidence: what syscall is each thread sitting in."""
    for tid in sorted(os.listdir(f"/proc/{pid}/task")):
        try:
            wchan = open(f"/proc/{pid}/task/{tid}/wchan").read().strip()
            sc = open(f"/proc/{pid}/task/{tid}/syscall").read().strip()
        except OSError:
            continue
        if wchan in ("0", ""):
            continue
        log.write(f"    {tag} tid={tid} wchan={wchan} syscall={sc}\n")


held_since = None
catches = 0
while time.time() < end:
    t = time.time()
    try:
        mem.seek(target)
        blob = mem.read(SIZE)
    except OSError:
        break
    if not blob or len(blob) < 40:
        break
    flag, = struct.unpack_from("<I", blob, 0)
    line, = struct.unpack_from("<i", blob, 32)
    fname = blob[36:].split(b"\x00")[0].decode("ascii", "replace")

    if flag:
        if held_since is None:
            held_since = t
        elif (t - held_since) >= trig and catches < max_catches:
            catches += 1
            log.write(f"\n=== catch {catches} at {t:.3f} "
                      f"held {(t-held_since)*1000:.0f} ms by {fname}:{line} ===\n")
            snapshot("pre")
            bt = subprocess.run(
                ["gdb", "-p", pid, "-batch", "-nx",
                 "-ex", "set confirm off",
                 "-ex", "thread apply all bt 25"],
                capture_output=True, text=True, timeout=60)
            log.write(bt.stdout)
            if bt.stderr.strip():
                log.write("STDERR: " + bt.stderr.strip()[:2000] + "\n")
            log.write(f"=== end catch {catches} ({time.time()-t:.2f} s in gdb) ===\n\n")
            held_since = None
            time.sleep(2.0)          # let the node breathe before re-arming
    else:
        held_since = None

    time.sleep(0.005)

log.write(f"# sem_stack end {time.time():.3f} catches={catches}\n")
print(f"done, {catches} catches -> {out}")
