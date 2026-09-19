#!/usr/bin/env python3
"""Sample the node 10x/s: CPU time, kernel wait channel, console-log size,
path-cache mtime.

The teardowns follow ~1 s of silence from IR2UFV, and the two candidate
explanations look completely different here:
  * a compute loop advances utime/stime by ~1 s and has an empty wchan;
  * a block shows a stable CPU time and a wchan naming what it waits on
    (jbd2/ext4 journal, socket, sleep).

Zero perturbation: three /proc reads and two stats per sample.

usage: sampler.py PID LOGFILE CACHEFILE OUT_CSV SECONDS
"""
import os
import sys
import time

pid, logfile, cachefile, out, secs = (sys.argv[1], sys.argv[2], sys.argv[3],
                                      sys.argv[4], float(sys.argv[5]))
clk = os.sysconf("SC_CLK_TCK")
end = time.time() + secs

with open(out, "w", buffering=1) as fh:
    fh.write("epoch,utime_s,stime_s,wchan,logsize,cache_mtime\n")
    while time.time() < end:
        t = time.time()
        try:
            with open(f"/proc/{pid}/stat", "rb") as f:
                parts = f.read().split(b") ")[-1].split()
            ut, st = int(parts[11]) / clk, int(parts[12]) / clk
        except (OSError, IndexError, ValueError):
            break
        try:
            with open(f"/proc/{pid}/wchan") as f:
                w = f.read().strip() or "-"
        except OSError:
            w = "?"
        try:
            sz = os.stat(logfile).st_size
        except OSError:
            sz = -1
        try:
            cm = os.stat(cachefile).st_mtime
        except OSError:
            cm = 0
        fh.write(f"{t:.3f},{ut:.2f},{st:.2f},{w},{sz},{cm:.0f}\n")
        time.sleep(max(0.0, 0.1 - (time.time() - t)))
