#!/usr/bin/env python3
"""ufv_log_bench.py - benchmark one IR2UFV flexdebug run (v2.6.0 QA).

Input: the slice of /tmp/flexnet_axudp.log from the last PATH-CACHE-LOAD
(= process start) to now. Lines carry only HH:MM:SS, so a day is added each
time the clock goes backwards.

Reports:
  * learned-age prunes, and for every withdrawal (exp=60000 FIRED) caused by
    a prune: how long until the same dest was re-advertised finite to the
    same peer (= the outage that peer saw)
  * poison (peer-down) events per peer
  * path-probe answer rate and latency
  * hold-down / climb-suppress hot spots
"""
import re
import sys
from collections import Counter, defaultdict

TS = re.compile(r"^(\d\d):(\d\d):(\d\d) ")
AGE = re.compile(r"LEARNED-AGE from=(\S+) dest=(\S+) age=(\d+)s")
ADV = re.compile(r"ADVERT-CHECK peer=(\S+) dest=(\S+) exp=(\d+) last=(\d+)")
POI = re.compile(r"POISON peer-down=(\S+) dest=(\S+) alt=(\S+)")
PREQ = re.compile(r"PATH-REQ-TX: .*target=(\S+) qso=(\d+)")
PREP = re.compile(r"PATH-REP-RX: qso=(\d+) .*\((?:matched [^,]*, )?(\d+)s elapsed")
HOLD = re.compile(r"(HOLDDOWN|CLIMB-SUPPRESS) peer=(\S+) dest=(\S+)")
INF = 60000


def seconds(lines):
    day, prev = 0, -1
    for ln in lines:
        m = TS.match(ln)
        if not m:
            continue
        t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3])
        if prev >= 0 and t < prev - 60:
            day += 1
        prev = t
        yield day * 86400 + t, ln


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else None


def main(path):
    with open(path, errors="replace") as f:
        lines = f.readlines()
    prunes = []
    pending = {}            # (peer, dest) -> t of withdrawal after a prune
    last_prune = {}         # dest -> t
    outages, never = [], 0
    poison = Counter()
    poison_events = defaultdict(set)
    req_t, rep_lat = {}, []
    n_req = 0
    hold = Counter()
    t0 = t1 = None
    for t, ln in seconds(lines):
        t0 = t if t0 is None else t0
        t1 = t
        if m := AGE.search(ln):
            prunes.append((t, m[1], m[2], int(m[3])))
            last_prune[m[2]] = t
        elif m := ADV.search(ln):
            peer, dest, exp, last = m[1], m[2], int(m[3]), int(m[4])
            if exp >= INF and last < INF and t - last_prune.get(dest, -999) <= 1:
                pending[(peer, dest)] = t
            elif exp < INF and last >= INF and (peer, dest) in pending:
                outages.append(t - pending.pop((peer, dest)))
        elif m := POI.search(ln):
            poison[m[1]] += 1
            poison_events[m[1]].add(t // 60)
        elif m := PREQ.search(ln):
            n_req += 1
            req_t[m[2]] = t
        elif m := PREP.search(ln):
            rep_lat.append(int(m[2]))
        elif m := HOLD.search(ln):
            hold[(m[1], m[3])] += 1
    never = len(pending)
    hours = (t1 - t0) / 3600
    print(f"span {hours:.1f} h, {len(lines)} lines")
    print(f"\nLEARNED-AGE prunes: {len(prunes)} ({len(prunes)/hours:.0f}/h)")
    print("  by source:", dict(Counter(p[1] for p in prunes)))
    ages = [p[3] for p in prunes]
    print(f"  age at prune: min {min(ages)} med {pct(ages, .5)} max {max(ages)} s")
    print(f"  withdrawals (60000) caused by a prune: {len(outages) + never}")
    print(f"    re-advertised later: {len(outages)}, never (still withdrawn): {never}")
    if outages:
        print(f"    outage seen by the peer: med {pct(outages, .5)} s, "
              f"p90 {pct(outages, .9)} s, max {max(outages)} s")
    top = Counter(p[2] for p in prunes).most_common(8)
    print("  most-pruned dests:", top)
    print("\nPOISON (peer-down) events, distinct minutes per peer:")
    for k, v in poison.items():
        print(f"  {k:10} lines={v:4} events={len(poison_events[k])}")
    print(f"\nPath probes: {n_req} sent, {len(rep_lat)} answered "
          f"({100*len(rep_lat)/max(n_req,1):.0f}%), latency med {pct(rep_lat,.5)} s "
          f"p90 {pct(rep_lat,.9)} s")
    print("\nHold-down / climb-suppress hot spots:")
    for (kind, dest), n in hold.most_common(8):
        print(f"  {kind:15} {dest:16} {n}")


if __name__ == "__main__":
    main(sys.argv[1])
