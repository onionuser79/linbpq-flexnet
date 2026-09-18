#!/usr/bin/env bash
# quad-report.sh — one-command morning read of the overnight FlexNet watch.
#
# Runs ON iw2ohx-gw. Read-only. Prints, in the order you want to read it:
#   1. whether the collectors are still alive at all (a silent collector
#      makes an empty alerts.log look like good news)
#   2. alerts.log grouped by check ID
#   3. PCF's cost to IR2UFV and to its (X)Net peers, from PCF's own table
#   4. IR2UFV's link uptimes — proof it stayed up, or when it did not
#   5. the L2 append/contract deltas, which is the open question
#
# `set -e` is deliberately NOT used: every section must print even when an
# earlier one has nothing to show.
set -uo pipefail

OUT=${OUT:-/tmp/quad-watch}
WIRE=${WIRE:-/tmp/ir2ufv-wire}
CSV=${CSV:-/tmp/pcf-watch.csv}

hr() { printf '\n== %s %s\n' "$1" "$(printf '=%.0s' $(seq 1 $((60 - ${#1}))))"; }

hr "collectors"
# An empty alerts.log means "nothing flagged" ONLY if the collector ran.
#
# LIVENESS IS NOT FRESHNESS. A `pgrep` hit says a process exists, not
# that it is still writing: a tcpdump that cannot open its file, or a
# sampler wedged on a socket read, both stay "alive". That distinction
# cost time twice on 2026-09-18 -- once reading a stalled capture as
# healthy, once declaring a healthy watcher stalled because its
# timestamps are UTC and the host clock is local. So report the age of
# what each collector last WROTE, and let the file decide.
check_collector () {   # $1 = pgrep pattern, $2 = file it should be writing
    local pat="$1" f="$2" pid age
    if pgrep -f "$pat" >/dev/null 2>&1; then pid="running"; else pid="NO PROCESS"; fi
    if [ -f "$f" ]; then
        age=$(( $(date +%s) - $(stat -c %Y "$f") ))
        printf '  %-28s %-11s last wrote %ss ago' "$pat" "$pid" "$age"
        [ "$age" -gt 1800 ] && printf '  <- STALE, results below are not current'
        printf '\n'
    else
        printf '  %-28s %-11s no output file yet\n' "$pat" "$pid"
    fi
}
check_collector "quad""-watch.py"          "$OUT/samples.jsonl"
check_collector "tcp""dump.*ir2ufv-wire"   "$(ls $WIRE/ufv.pcap* 2>/dev/null | head -1)"
check_collector "pcf""-watch.sh"           "$CSV"
echo "  (timestamps INSIDE quad-watch samples are UTC; the host clock is local)"
if [ -d "$OUT" ]; then
    n=$(grep -c '^--- sample' "$OUT/watch.log" 2>/dev/null || true)
    echo "  samples collected: ${n:-0}"
    echo "  first: $(grep -m1 '^--- sample' "$OUT/watch.log" 2>/dev/null)"
    echo "  last:  $(grep '^--- sample' "$OUT/watch.log" 2>/dev/null | tail -1)"
fi
echo "  wire ring: $(du -sh "$WIRE" 2>/dev/null | cut -f1) in $(ls -1 "$WIRE"/ufv.pcap* 2>/dev/null | wc -l) files"

hr "alerts by check ID"
if [ -s "$OUT/alerts.log" ]; then
    awk '{for(i=1;i<=NF;i++) if($i ~ /^[AI][0-9]+$/){print $i; break}}' \
        "$OUT/alerts.log" | sort | uniq -c | sort -rn
    echo
    echo "-- first occurrence of each --"
    awk '{for(i=1;i<=NF;i++) if($i ~ /^[AI][0-9]+$/){id=$i; break}}
         !seen[id]++ {print}' "$OUT/alerts.log"
else
    echo "  (no alerts file, or empty)"
fi

hr "A4 session restarts / A1 advert mismatches in full"
grep -hE ' (A4|A1) ' "$OUT/alerts.log" 2>/dev/null | tail -20 || echo "  (none)"

hr "PCF (IW2OHX-12) own table — cost to us vs to its xnet peers"
# Straight from the last sample the watcher took; no new mesh traffic.
python3 - "$OUT/samples.jsonl" <<'PY' 2>/dev/null || echo "  (no parsed PCF sample yet)"
import json, sys
rows = []
for line in open(sys.argv[1]):
    try:
        s = json.loads(line)
    except ValueError:
        continue
    if s.get("n12"):
        rows.append((s["ts"], s["n12"]))
if not rows:
    raise SystemExit(1)
print("  samples containing a PCF table:", len(rows))
for ts, tbl in rows[:1] + (rows[-1:] if len(rows) > 1 else []):
    print(f"  {ts}")
    for k, v in sorted(tbl.items()):
        mark = "  <- us" if k.startswith("IR2UFV") else ""
        print(f"     {k:<16} cost={v['cost']:<6} qual={v['qual']}{mark}")
if len(rows) > 1:
    a = next((v for k, v in rows[0][1].items() if k.startswith("IR2UFV")), None)
    b = next((v for k, v in rows[-1][1].items() if k.startswith("IR2UFV")), None)
    if a and b:
        print(f"  TREND cost to IR2UFV: {a['cost']} -> {b['cost']} "
              f"({b['cost'] - a['cost']:+d})")
PY

hr "IR2UFV link uptimes (did it stay up?)"
grep -hE '^  IW2OHX-(4|12|14) ' "$OUT/watch.log" 2>/dev/null | tail -9 \
    || echo "  (no samples)"

hr "L2 forwarding deltas — the open question"
grep -h 'I6 L2_DELTA' "$OUT/alerts.log" 2>/dev/null | tail -12 || echo "  (none)"
echo
grep -h 'A6 L2_CONTRACT_ONLY' "$OUT/alerts.log" 2>/dev/null | tail -5 \
    || echo "  (no contract-only windows)"
echo
echo "  latest counters:"
grep -h '^  L2 ' "$OUT/watch.log" 2>/dev/null | tail -3 || echo "  (none)"

hr "PATH_REQ answer lengths (the rc5 guard)"
# A hops=N answer means N-1 digis; anything over 8 was unanswerable and
# is what the guard now suppresses. The histogram says whether the
# condition even arises.
echo "  PATH-REP-TX hop counts we have SENT:"
grep -aoE 'PATH-REP-TX.*hops=[0-9]+' /tmp/flexnet_axudp.log 2>/dev/null \
    | grep -oE 'hops=[0-9]+' | sort -t= -k2 -n | uniq -c | tail -8 \
    || echo "    (none)"
echo "  guard firings (PATH-REQ-TOOLONG):"
grep -ac 'PATH-REQ-TOOLONG' /tmp/flexnet_axudp.log 2>/dev/null || echo 0
grep -a 'PATH-REQ-TOOLONG' /tmp/flexnet_axudp.log 2>/dev/null | tail -3

hr "fix validation verdict"
[ -s /tmp/validate-fix.log ] && tail -25 /tmp/validate-fix.log \
    || echo "  (not armed / no output)"

hr "pcf-watch.csv tail (IR2UFV-side PCF health)"
[ -s "$CSV" ] && { head -1 "$CSV"; tail -6 "$CSV"; } || echo "  (no csv)"
