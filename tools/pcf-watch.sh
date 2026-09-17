#!/usr/bin/env bash
# Phase 3 data collector: sample PC/Flexnet IW2OHX-12's health while
# IR2UFV runs in transit mode, from BOTH sides.
#
# Appends one CSV row per sample to /tmp/pcf-watch.csv:
#   ts,ir2ufv_pcf_lt_s,ir2ufv_pcf_uptime,ir2ufv_pcf_advert,ir2ufv_pcf_queue,session_starts
#
# The authoritative PCF-side cost (its own `L` row) needs a chained
# telnet through IW2OHX-14 and is sampled separately/less often, since
# each sample crosses the FlexNet mesh.
#
# Runs ON iw2ohx-gw. Read-only: no config or routing is touched.
set -uo pipefail

CSV=/tmp/pcf-watch.csv
CONSOLE=/tmp/ir2ufv.console
INTERVAL=${INTERVAL:-300}
ITERATIONS=${ITERATIONS:-0}          # 0 = run until killed

[ -f "$CSV" ] || echo "ts,pcf_lt_s,pcf_uptime,advert,queue,cycles,poison_total,holddown,learned_age" > "$CSV"

n=0
while :; do
    row=$(bash /tmp/verify-node.sh 2525 2>/dev/null | tr -d '\r')
    lt=$(  printf '%s\n' "$row" | awk '/^IW2OHX-12 .*CONNECTED/ {print $4}')
    up=$(  printf '%s\n' "$row" | awk '/^IW2OHX-12 .*CONNECTED/ {print $6}')
    adv=$( printf '%s\n' "$row" | awk '/^IW2OHX-12 +PCF/ {print $5}')
    q=$(   printf '%s\n' "$row" | awk '/^IW2OHX-12 +PCF/ {print $6}')
    # Skip the sample outright if the node was unreachable (restarting,
    # or telnet busy). Writing '?' placeholders pollutes the series with
    # rows that look like data but are not.
    if [ -z "$lt" ] || [ -z "$adv" ]; then
        n=$((n + 1))
        if [ "$ITERATIONS" -gt 0 ] && [ "$n" -ge "$ITERATIONS" ]; then break; fi
        sleep "$INTERVAL"
        continue
    fi

    # `grep -c` ALWAYS prints a count and exits 1 when that count is 0,
    # so `grep -c ... || echo 0` emits BOTH the "0" and the fallback
    # "0" — two lines inside one command substitution, which corrupted
    # this CSV until it was spotted.
    cyc=$(grep -c 'session started on port 2 with IW2OHX-12' "$CONSOLE" 2>/dev/null)
    poi=$(grep -c 'POISON peer-down' "$CONSOLE" 2>/dev/null)
    hld=$(grep -c 'HOLDDOWN' "$CONSOLE" 2>/dev/null)
    age=$(grep -c 'LEARNED-AGE' "$CONSOLE" 2>/dev/null)

    echo "$(date +%Y-%m-%dT%H:%M:%S),$lt,$up,$adv,$q,${cyc:-0},${poi:-0},${hld:-0},${age:-0}" >> "$CSV"

    n=$((n + 1))
    [ "$ITERATIONS" -gt 0 ] && [ "$n" -ge "$ITERATIONS" ] && break
    sleep "$INTERVAL"
done
