#!/usr/bin/env bash
# Capture IR2UFV's AXIP port in STEADY STATE (post-convergence), which
# is the window B3 and B7 actually need.
#
# Two traps this replaces, both hit on 2026-09-17:
#  1. tcpdump drops privileges to the `tcpdump` user, so it cannot
#     overwrite a pcap left behind by a previous run — it exits with
#     "Permission denied" and the caller sees an empty file. Always
#     write to a fresh path.
#  2. `pgrep -f 'tcpdump.*10075'` matches the WAITER's own command line,
#     so a wait-loop built on it never terminates and reports the
#     capture as running forever. Match the process name, not a pattern
#     that appears in your own argv.
set -euo pipefail

SECS=${SECS:-900}
PCAP=/tmp/ir2ufv-steady-$(date +%H%M%S).pcap

# Clear any previous waiters/captures without matching ourselves.
sudo pkill -x tcpdump 2>/dev/null || true
sleep 1

sudo nohup timeout "$SECS" tcpdump -i any -s 0 -U -w "$PCAP" \
    'udp port 10075' >"/tmp/tcpdump-steady.log" 2>&1 &
sleep 4

if ! pgrep -x tcpdump >/dev/null; then
    echo "FAILED to start tcpdump:"
    cat /tmp/tcpdump-steady.log
    exit 1
fi
# Prove it is actually writing, not just alive.
sleep 6
SZ=$(stat -c %s "$PCAP")
echo "tcpdump up, pcap=$PCAP size=${SZ}B after 10s"
[ "$SZ" -gt 24 ] || { echo "WARNING: only the pcap header so far"; }
echo "$PCAP" > /tmp/steady-pcap-path
echo "will run ${SECS}s from $(date +%H:%M:%S)"
