#!/bin/bash
# Stop the IR2UFV monitor. Idempotent.
set -euo pipefail
RUN_DIR="/tmp/ir2ufv-monitor"
cd "$RUN_DIR"

if [ -f tcpdump.pid ]; then
    PID="$(cat tcpdump.pid)"
    sudo kill "$PID" 2>/dev/null || true
    echo "stopped tcpdump pid=$PID"
    rm -f tcpdump.pid
fi
if [ -f poll.pid ]; then
    PID="$(cat poll.pid)"
    kill "$PID" 2>/dev/null || true
    echo "stopped poll pid=$PID"
    rm -f poll.pid
fi

if [ -f current.run ]; then
    echo "=== final state ==="
    cat current.run
    echo "stopped_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
fi

echo
echo "=== artifacts ==="
ls -lh "$RUN_DIR"
