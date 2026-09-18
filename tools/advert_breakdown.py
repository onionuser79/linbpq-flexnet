#!/usr/bin/env python3
"""Break the ADVERT-CHECK stream down by why each advertisement fired.

Which of the four causes dominates decides which fix is worth writing:
a first-time fire after a session re-init is a re-dump problem, a small
delta on an already-advertised route is a threshold problem, and the
infinity pair is a hold-down problem.
"""
import collections
import re
import sys

PAT = re.compile(
    r"ADVERT-CHECK peer=(\S+) dest=(\S+) exp=(-?\d+) last=(-?\d+) "
    r"delta=(-?\d+) (FIRED|SUPPRESSED)"
)
INF = 60000

first = collections.Counter()
jitter = collections.Counter()
withdraw = collections.Counter()
restore = collections.Counter()
suppressed = collections.Counter()
total = collections.Counter()
deltas = collections.defaultdict(list)

for line in open(sys.argv[1], errors="replace"):
    m = PAT.search(line)
    if not m:
        continue
    peer, _dest, exp, last, delta, verdict = (
        m.group(1), m.group(2), int(m.group(3)), int(m.group(4)),
        int(m.group(5)), m.group(6))
    total[peer] += 1
    if verdict == "SUPPRESSED":
        suppressed[peer] += 1
        continue
    if last == -1:
        first[peer] += 1
    elif exp >= INF:
        withdraw[peer] += 1
    elif last >= INF:
        restore[peer] += 1
    else:
        jitter[peer] += 1
        deltas[peer].append((last, delta))

hdr = ("peer", "total", "first", "jitter", "withdraw", "restore", "suppr")
print("%-12s%7s%8s%8s%9s%8s%7s" % hdr)
for p in sorted(total, key=lambda x: -total[x]):
    print("%-12s%7d%8d%8d%9d%8d%7d" % (p, total[p], first[p], jitter[p],
                                       withdraw[p], restore[p], suppressed[p]))

print("\njitter fires by size of the change (RTT ticks of 100 ms):")
for p in sorted(deltas):
    ds = deltas[p]
    buckets = collections.Counter()
    for _last, d in ds:
        buckets["d<=1" if d <= 1 else
                "d<=2" if d <= 2 else
                "d<=5" if d <= 5 else
                "d<=20" if d <= 20 else "d>20"] += 1
    order = ["d<=1", "d<=2", "d<=5", "d<=20", "d>20"]
    parts = ["%s:%d(%.0f%%)" % (k, buckets[k], 100.0 * buckets[k] / len(ds))
             for k in order if buckets[k]]
    print("  %-12s n=%-6d %s" % (p, len(ds), "  ".join(parts)))
