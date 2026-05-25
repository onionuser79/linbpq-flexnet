#!/bin/bash
# IR2UFV ↔ IW2OHX-12 AXUDP wire monitor — tcpdump only.
# Run on iw2ohx-gw. Idempotent: a previous run is killed first.
set -euo pipefail

RUN_DIR="/tmp/ir2ufv-monitor"
mkdir -p "$RUN_DIR"
cd "$RUN_DIR"

if [ -f tcpdump.pid ]; then
    sudo kill "$(cat tcpdump.pid)" 2>/dev/null || true
    rm -f tcpdump.pid
fi

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
PCAP="capture-${STAMP}.pcap"
TCPDUMP_LOG="tcpdump-${STAMP}.log"

# Filter: IR2UFV↔IW2OHX-12 only, excluding production iw2ohx-13 (port 10093).
FILTER='udp and host 192.168.1.201 and port 10075 and not port 10093'

# -U flushes per packet so the file is recoverable if killed.
sudo nohup tcpdump -i any -n -U -s 0 -w "$PCAP" "$FILTER" \
    > "$TCPDUMP_LOG" 2>&1 &
TCPDUMP_PID=$!
echo "$TCPDUMP_PID" | sudo tee tcpdump.pid > /dev/null

{
    echo "stamp=$STAMP"
    echo "pcap=$PCAP"
    echo "tcpdump_log=$TCPDUMP_LOG"
    echo "tcpdump_pid=$TCPDUMP_PID"
    echo "filter=$FILTER"
    echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > current.run

sleep 2
echo "=== started ==="
cat current.run
echo
echo "=== ps ==="
ps -p "$TCPDUMP_PID" -o pid,etime,cmd 2>/dev/null || echo "tcpdump pid $TCPDUMP_PID not running"
echo
echo "=== first tcpdump log ==="
head -5 "$TCPDUMP_LOG" 2>/dev/null || true
