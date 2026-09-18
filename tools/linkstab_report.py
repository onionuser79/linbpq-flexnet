#!/usr/bin/env python3
"""
linkstab_report.py — turn a `linkstab.py` run into the numbers that
decide whether a link-stability change worked.

Three things here are not just "read the events log", and each exists
because the naive version gave a wrong answer during the 2026-09-18 run:

  * **Session episodes are reconstructed, not counted from events.**
    `linkstab` flags a restart by comparing a link's uptime between two
    polls, which silently misses the case that matters most: a link that
    drops out of `FL` altogether and comes back. The ~14:33 IW2OHX-4
    restart was invisible in `events.log` for exactly that reason. Here a
    link is followed as a state machine — present/absent plus uptime
    regression — so a disappearance is an episode boundary like any
    other.

  * **Uptime is reported as a distribution, not a mean.** These links
    fail in bursts: three restarts inside four minutes and then two
    quiet hours. A mean over that is a number no single moment
    resembles. Median and the longest/shortest episode say more.

  * **Advertisement volume is summed from per-interval deltas.** The
    counters are cumulative and reset when the node restarts, so a
    before/after taken across a restart reads as a negative. Intervals
    where the counter went backwards are dropped and reported as gaps
    rather than folded into a total.

Usage:
    linkstab_report.py --dir /tmp/linkstab
    linkstab_report.py --dir /tmp/linkstab --split 2026-09-18T13:52:53Z
"""

import argparse
import json
import os
import statistics
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone


def parse_ts(text):
    return datetime.strptime(text.rstrip("Z"),
                             "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)


def load_jsonl(path):
    if not os.path.exists(path):
        return []
    rows = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue                      # half-written final line
    return rows


def human(seconds):
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f"{h:d}:{m:02d}:{s:02d}"


def build_episodes(polls):
    """Reconstruct per-link session episodes from the FL poll stream.

    An episode ends when the link's uptime goes backwards OR when the
    link disappears from the table. Both are restarts; only the first
    is visible to an uptime-delta check.
    """
    episodes = defaultdict(list)     # call -> [{start, end, max_uptime}]
    open_ep = {}                     # call -> dict
    absent = {}                      # call -> episode parked while missing
    all_calls = set()

    for rec in polls:
        ts = parse_ts(rec["ts"])
        links = rec.get("links") or {}
        all_calls.update(links)

        for call, link in links.items():
            up = link.get("uptime_s")
            cur = open_ep.get(call)
            if cur is None and call in absent:
                # The row came back. A missing row is NOT proof of a
                # restart — `FL` can be read mid-update, or the reply can
                # be truncated. Cross-checking against the wire on
                # 2026-09-18 showed IW2OHX-14 with 6 reconstructed
                # restarts against 3 actual DISCs, all of the difference
                # being single-poll gaps. So: if the uptime on return is
                # at least what it would be had the session never
                # dropped, resume the same episode.
                parked = absent.pop(call)
                gap = (ts - parked["end"]).total_seconds()
                continuous = (up is not None and
                              up >= parked["max_uptime"] + gap * 0.5)
                if continuous:
                    parked["end"] = ts
                    parked["max_uptime"] = up
                    open_ep[call] = parked
                    continue
                parked["reason"] = "vanished from FL"
                episodes[call].append(parked)
                cur = None

            if cur is None:
                open_ep[call] = {"start": ts, "end": ts, "max_uptime": up or 0,
                                 "reason": None}
                continue
            if up is not None and up < cur["max_uptime"]:
                cur["reason"] = "uptime reset"
                episodes[call].append(cur)
                open_ep[call] = {"start": ts, "end": ts, "max_uptime": up,
                                 "reason": None}
            else:
                cur["end"] = ts
                if up is not None:
                    cur["max_uptime"] = up

        for call in list(open_ep):
            if call not in links:
                absent[call] = open_ep.pop(call)

    for call, cur in list(absent.items()):
        cur["reason"] = "vanished from FL"
        episodes[call].append(cur)
    for call, cur in open_ep.items():
        cur["reason"] = "still up at end of run"
        episodes[call].append(cur)
    return episodes, sorted(all_calls)


def report_stability(polls, label):
    episodes, calls = build_episodes(polls)
    if not polls:
        print(f"  (no polls in {label})")
        return
    span = (parse_ts(polls[-1]["ts"]) - parse_ts(polls[0]["ts"])).total_seconds()
    print(f"  window {polls[0]['ts']} .. {polls[-1]['ts']}  "
          f"({human(span)}, {len(polls)} polls)")
    print(f"  {'link':<12}{'episodes':>9}{'restarts':>9}{'median up':>12}"
          f"{'longest':>10}{'shortest':>10}")
    for call in calls:
        eps = episodes.get(call, [])
        durs = [e["max_uptime"] for e in eps]
        ended = [e for e in eps if e["reason"] != "still up at end of run"]
        if not durs:
            continue
        print(f"  {call:<12}{len(eps):>9}{len(ended):>9}"
              f"{human(statistics.median(durs)):>12}"
              f"{human(max(durs)):>10}{human(min(durs)):>10}")
        if span > 0 and ended:
            print(f"  {'':<12}  restarts/hour = "
                  f"{len(ended) * 3600.0 / span:.2f}")
    return episodes


def report_volume(polls, label):
    """Sum advertisement fires from per-interval deltas."""
    total = Counter()
    gaps = 0
    rates = defaultdict(list)
    queues = defaultdict(list)
    for rec in polls:
        log = rec.get("log") or {}
        if not log.get("bytes"):
            gaps += 1
            continue
        for peer, n in (log.get("per_peer_fired") or {}).items():
            total[peer] += n
        by_peer = rec.get("fired_per_min_by_peer") or {}
        for peer, r in by_peer.items():
            rates[peer].append(r)
        for peer, p in (rec.get("peers") or {}).items():
            queues[peer].append(p.get("queued", 0))

    if not total:
        print(f"  (no advertisement data in {label})")
        return
    print(f"  {'peer':<12}{'fires':>9}{'med/min':>10}{'p90/min':>10}"
          f"{'med queue':>11}{'max queue':>11}{'queue>0':>9}")
    for peer in sorted(total, key=lambda p: -total[p]):
        rs = sorted(rates.get(peer, [0]))
        qs = queues.get(peer, [0])
        p90 = rs[min(len(rs) - 1, int(0.9 * len(rs)))] if rs else 0
        nonzero = 100.0 * sum(1 for q in qs if q > 0) / max(1, len(qs))
        print(f"  {peer:<12}{total[peer]:>9}"
              f"{statistics.median(rs):>10.1f}{p90:>10.1f}"
              f"{statistics.median(qs):>11.0f}{max(qs):>11}{nonzero:>8.0f}%")
    if gaps:
        print(f"  ({gaps} polls with no log delta — node restart or first "
              f"poll; excluded)")


def report_connects(connects, label):
    if not connects:
        print(f"  (no connect probes in {label})")
        return
    outcomes = Counter(c["outcome"] for c in connects)
    total = len(connects)
    ok = outcomes.get("CONNECTED", 0)
    print(f"  {total} probes: " + ", ".join(
        f"{k}={v} ({100.0*v/total:.0f}%)"
        for k, v in outcomes.most_common()))
    print(f"  success rate {100.0*ok/total:.1f}%")

    # The `!` path-cached marker is not a reachability claim, but it is
    # read as one. Splitting on it is the whole point of the probe.
    by_dest = defaultdict(Counter)
    for c in connects:
        by_dest[c["dest"]][c["outcome"]] += 1
    always_ok = [d for d, o in by_dest.items()
                 if o.get("CONNECTED", 0) == sum(o.values())]
    never_ok = [d for d, o in by_dest.items() if not o.get("CONNECTED")]
    flaky = [d for d, o in by_dest.items()
             if 0 < o.get("CONNECTED", 0) < sum(o.values())]
    print(f"  destinations: {len(by_dest)} probed — "
          f"{len(always_ok)} always reachable, {len(never_ok)} never, "
          f"{len(flaky)} intermittent")
    if flaky:
        print("  intermittent (the table cannot be trusted for these): "
              + ", ".join(sorted(flaky)[:15]))
    if never_ok:
        print("  never reachable (first 15): " + ", ".join(sorted(never_ok)[:15]))

    secs = [c["seconds"] for c in connects if c["outcome"] == "CONNECTED"]
    if secs:
        print(f"  successful connect time: median {statistics.median(secs):.0f}s, "
              f"max {max(secs):.0f}s")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default="/tmp/linkstab")
    ap.add_argument("--split", help="UTC stamp splitting the run into "
                                    "before/after (e.g. a fix deploy)")
    args = ap.parse_args()

    polls = load_jsonl(os.path.join(args.dir, "fl.jsonl"))
    connects = load_jsonl(os.path.join(args.dir, "connects.jsonl"))
    if not polls:
        print(f"no FL polls under {args.dir}", file=sys.stderr)
        return 1

    if args.split:
        cut = parse_ts(args.split)
        phases = [
            ("BEFORE " + args.split,
             [p for p in polls if parse_ts(p["ts"]) < cut],
             [c for c in connects if parse_ts(c["ts"]) < cut]),
            ("AFTER " + args.split,
             [p for p in polls if parse_ts(p["ts"]) >= cut],
             [c for c in connects if parse_ts(c["ts"]) >= cut]),
        ]
    else:
        phases = [("FULL RUN", polls, connects)]

    for label, ph_polls, ph_connects in phases:
        print("=" * 72)
        print(label)
        print("=" * 72)
        print("\n-- link stability --")
        report_stability(ph_polls, label)
        print("\n-- advertisement volume --")
        report_volume(ph_polls, label)
        print("\n-- connect probes --")
        report_connects(ph_connects, label)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
