#!/usr/bin/env bash
# RFC_TRANSIT_ROLE_V2 §10.1.c D2 + D3 — poison-reverse on session loss.
#
# Drops IR2UFV's AXIP traffic to ONE xnet peer, waits for the FlexNet
# session to go down, then restores it. Exercises both tests at once:
#   D2 — a destination reachable ONLY via the dropped peer must be
#        withdrawn (RTT=60000) to the other peers.
#   D3 — a destination also reachable via a surviving peer must NOT be
#        withdrawn; the log should name the alternate.
#
# Runs ON iw2ohx-gw. Targets UDP/10075 = IR2UFV only; production
# IW2OHX-13 is on 10093 and cannot be affected by the rule below.
# PC/Flexnet IW2OHX-12 is deliberately NOT a valid target: a sustained
# record burst can put PCF in a state needing a manual reset at the far
# end.
set -uo pipefail

PEER_IP=${PEER_IP:-192.168.1.203}      # IW2OHX-4 (LAN xnet peer)
PEER=${PEER:-IW2OHX-4}
PORT=10075
DROP_SECS=${DROP_SECS:-240}
CONSOLE=/tmp/ir2ufv.console
FLEXLOG=/tmp/flexnet_axudp.log

# Fresh, timestamped pcap every run. tcpdump drops privileges to the
# `tcpdump` user after opening its capture handle, so it CANNOT
# overwrite a pcap a previous run left behind: it exits at once with
# "Permission denied" into its own stderr log while the caller keeps
# seeing the stale file at its old size. A fixed path made a whole run's
# capture silently stale — see CLAUDE.md "Live traps".
PCAP=/tmp/ir2ufv-d2d3-$(date +%Y%m%d-%H%M%S).pcap
TCPDUMP_LOG=/tmp/tcpdump-d2d3.log

if [ "$PEER_IP" = "192.168.1.201" ] || [ "$PEER" = "IW2OHX-12" ]; then
    echo "REFUSING: IW2OHX-12 is PC/Flexnet — see header." >&2
    exit 1
fi

cleanup() {
    echo ">> removing DROP rule (idempotent)"
    while sudo iptables -C OUTPUT -d "$PEER_IP" -p udp --dport "$PORT" \
            -j DROP 2>/dev/null; do
        sudo iptables -D OUTPUT -d "$PEER_IP" -p udp --dport "$PORT" -j DROP
    done
    sudo iptables -S OUTPUT | grep -c "$PEER_IP" || true
}
trap cleanup EXIT

mark_console=$(wc -l < "$CONSOLE" 2>/dev/null || echo 1)
mark_flex=$(wc -l < "$FLEXLOG" 2>/dev/null || echo 1)
fwd_before=$(grep -c 'CF-TRANSIT-FWD' "$FLEXLOG" 2>/dev/null || echo 0)

echo ">> console baseline at line $mark_console"

echo ">> starting capture -> $PCAP"
sudo pkill -x tcpdump 2>/dev/null || true
sleep 1
sudo nohup timeout $((DROP_SECS + 180)) tcpdump -i any -s 0 -U \
    -w "$PCAP" "udp port $PORT" >"$TCPDUMP_LOG" 2>&1 &
sleep 4

# Prove tcpdump is RECORDING, not merely alive: "the capture is
# running" is not evidence that bytes are landing in the file.
if ! pgrep -x tcpdump >/dev/null; then
    echo "FAILED to start tcpdump:" >&2
    cat "$TCPDUMP_LOG" >&2
    exit 1
fi
sleep 6
SZ=$(stat -c %s "$PCAP" 2>/dev/null || echo 0)
if [ "$SZ" -le 24 ]; then          # 24 = pcap file header alone
    echo "WARNING: pcap still only ${SZ}B after 10s — check $TCPDUMP_LOG" >&2
fi
echo ">> capture confirmed writing (${SZ}B after 10s)"

echo ">> DROPping IR2UFV -> $PEER ($PEER_IP:$PORT) for ${DROP_SECS}s"
sudo iptables -I OUTPUT 1 -d "$PEER_IP" -p udp --dport "$PORT" -j DROP
date +%H:%M:%S

sleep "$DROP_SECS"

echo ">> restoring link"
cleanup
date +%H:%M:%S

echo ">> letting the session re-establish"
sleep 120

echo
echo "=== POISON lines since baseline ==="
tail -n "+$mark_console" "$CONSOLE" | grep -E 'POISON peer-down' | head -40 || true
echo
echo "=== poison line counts ==="
echo "D2 candidates (no alternate -> withdrawn): $(tail -n "+$mark_console" "$CONSOLE" | grep -c 'alt=(none, poisoning)' || true)"
echo "D3 candidates (alternate exists -> quiet): $(tail -n "+$mark_console" "$CONSOLE" | grep 'POISON peer-down' | grep -vc 'alt=(none' || true)"
echo
echo "=== loop containment (both added after the IR2UFX phantom) ==="
echo "HOLDDOWN suppressions: $(tail -n "+$mark_console" "$CONSOLE" | grep -c 'HOLDDOWN' || true)"
echo "LEARNED-AGE prunes   : $(tail -n "+$mark_console" "$CONSOLE" | grep -c 'LEARNED-AGE' || true)"
echo
echo "=== session lifecycle ==="
tail -n "+$mark_console" "$CONSOLE" \
    | grep -E 'session with .* closed|session started|SEED peer=' | head -20 || true
echo
echo "=== CREQ forwarding (§6) ==="
fwd_after=$(grep -c 'CF-TRANSIT-FWD' "$FLEXLOG" 2>/dev/null || echo 0)
echo "CF-TRANSIT-FWD before=$fwd_before after=$fwd_after"

echo
echo ">> waiting for capture to close"
# Match the process NAME. `pgrep -f "tcpdump.*$PORT"` can match the
# waiter's own command line, in which case this loop never terminates
# and reports the capture as running forever.
while pgrep -x tcpdump >/dev/null; do sleep 5; done
ls -la "$PCAP"
echo ">> analyse with: python3 tools/parse_advertise.py $PCAP --me IR2UFV --pcf IW2OHX-12"
