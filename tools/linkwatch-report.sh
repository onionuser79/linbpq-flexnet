#!/bin/bash
# One-shot read-out of the post-IW2OHX-4-removal watch on IR2UFV.
# Safe to run at any time; everything here is read-only.
set -uo pipefail
L=192.168.1.202
echo "############ collectors ############"
pgrep -af 'tcpdump.*no4-mon' | sed 's/^/  /' || echo "  NO CAPTURES RUNNING"
pgrep -af 'linkstab.py --out /tmp/no4' | sed 's/^/  /' || echo "  NO LINKSTAB"
pgrep -af 'sampler.py' | grep -v sudo | sed 's/^/  /' || echo "  NO SAMPLER"

echo
echo "############ link events (linkstab) ############"
grep -E "SESSION_RESTART|CONNECT_FAIL|COUNTER_RESET" /tmp/no4-linkstab/events.log 2>/dev/null | tail -20
echo "  -- last FL --"
grep '^2026.*FL ' /tmp/no4-linkstab/watch.log 2>/dev/null | tail -3

echo
echo "############ IW2OHX-4 must stay silent ############"
printf "  no4-mon-4 pcap: "; stat -c '%s bytes' /tmp/no4-mon-4/link.pcap0 2>/dev/null || echo MISSING

for p in 14 12; do
    case $p in 14) IP=44.134.24.4;; 12) IP=192.168.1.201;; esac
    echo
    echo "############ IW2OHX-$p ($IP) ############"
    sudo python3 /tmp/axudp_teardown.py /tmp/no4-mon-$p/link.pcap* \
        --local-ip $L --link-only 2>&1 | grep -E "^decoded|teardown Out"
    sudo python3 /tmp/poll_latency.py /tmp/no4-mon-$p/link.pcap* $L $IP 2>&1 \
        | grep -vE "^     " | head -12
    echo "  -- slow acks, with what the process was doing --"
    sudo python3 /tmp/ack_latency.py /tmp/no4-mon-$p/link.pcap* $L $IP 400 \
        /tmp/no4-linkstab/proc-sample.csv 2>&1 | head -20
done
