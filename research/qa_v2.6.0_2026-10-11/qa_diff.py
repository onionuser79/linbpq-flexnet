#!/usr/bin/env python3
"""qa_diff.py - compare FlexNet destination tables across nodes (one snapshot)."""
import json, re, sys
REC = re.compile(r"([A-Z0-9]{3,6})\s+(\d+)-(\d+)\s+(\d+)(!?)")
def parse(text):
    d = {}
    for m in REC.finditer(text):
        d[(m[1], int(m[2]), int(m[3]))] = (int(m[4]), m[5] == "!")
    return d
lines = open(sys.argv[1]).read().splitlines()
rec = json.loads(lines[int(sys.argv[2]) if len(sys.argv) > 2 else -1])
print(rec["utc"])
tabs = {k: parse(v.get("D", "")) for k, v in rec.items() if isinstance(v, dict)}
allk = set().union(*[set(t) for t in tabs.values()])
for k, t in tabs.items():
    print(f"{k:10} {len(t):4} entries")
ref = tabs["IW2OHX-14"]
for k, t in tabs.items():
    if k == "IW2OHX-14": continue
    miss = sorted(set(ref) - set(t)); extra = sorted(set(t) - set(ref))
    print(f"\n== {k}: missing vs -14 {len(miss)}: {miss[:40]}")
    print(f"   extra vs -14 {len(extra)}: {extra[:40]}")
# cost sanity on linbpq: cost vs (-14 cost + link)
print("\n== cost ratio linbpq / -14 (median), and outliers >3x")
for k in ("IW2OHX-13", "IR2UFV", "IW2OHX-12"):
    rs = []; out = []
    for key, (c, _) in tabs[k].items():
        if key in ref and ref[key][0] > 0:
            r = c / ref[key][0]; rs.append(r)
            if r > 3 and c > 20: out.append((key, c, ref[key][0]))
    rs.sort(); print(k, "median", round(rs[len(rs)//2], 2), "outliers", out)
