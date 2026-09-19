#!/bin/bash
# Start the post-IW2OHX-4-removal link-stability watch on IR2UFV.
# Captures -14 and -12 (and -4, to prove it stays away) + 24 h linkstab.
# Kept as a file: `pgrep -f linkstab` typed inline over ssh self-matches.
set -uo pipefail

TAG=no4
echo "== stop any previous collectors =="
sudo pkill -f 'tcpdump.*rc[0-9]-mon' || true
sudo pkill -f "tcpdump.*${TAG}-mon" || true
pkill -f "linkstab.py --out /tmp/${TAG}" || true
sleep 2

echo "== fresh dirs (tcpdump cannot overwrite an existing pcap) =="
for p in 4 12 14; do
    sudo rm -rf "/tmp/${TAG}-mon-$p"
    sudo mkdir -p "/tmp/${TAG}-mon-$p"
    sudo chmod 777 "/tmp/${TAG}-mon-$p"
done
rm -rf "/tmp/${TAG}-linkstab"
mkdir -p "/tmp/${TAG}-linkstab"

echo "== captures (IR2UFV AXIP = 10075; prod -13 is 10093 and is excluded) =="
start_cap() {  # $1 = peer label, $2 = peer IP
    sudo sh -c "nohup tcpdump -i any -n -s 0 -U -W 6 -C 10 \
        -w /tmp/${TAG}-mon-$1/link.pcap \
        'udp port 10075 and not port 10093 and host $2' \
        >/tmp/${TAG}-mon-$1/tcpdump.log 2>&1 &"
}
start_cap 14 44.134.24.4
start_cap 4  192.168.1.203
start_cap 12 192.168.1.201

echo "== linkstab (same knobs as the rc7 baseline, so runs are comparable) =="
cat > /tmp/run-linkstab-${TAG}.sh <<'INNER'
#!/bin/bash
set -euo pipefail
set -a; . /home/iw2ohx/.qw.env; set +a
export LS_USER_UFV="$QW_USER_UFV"
export LS_PW_UFV="$QW_PW_UFV"
exec python3 /tmp/linkstab.py --out /tmp/no4-linkstab --hours 24 \
    --fl-interval 60 --connect-interval 1800 --connect-batch 6 \
    --connect-timeout 60 --pool-interval 3600 --summary-every 5
INNER
chmod +x /tmp/run-linkstab-${TAG}.sh
setsid nohup "/tmp/run-linkstab-${TAG}.sh" >> "/tmp/${TAG}-linkstab/nohup.out" 2>&1 < /dev/null &

echo "== verify: every pcap must GROW, linkstab must be alive =="
sleep 25
for p in 4 12 14; do
    printf "  %s-mon-%-3s " "$TAG" "$p"
    stat -c '%s bytes' "/tmp/${TAG}-mon-$p/link.pcap0" 2>/dev/null || echo MISSING
done
sleep 20
echo "  --- second sample ---"
for p in 4 12 14; do
    printf "  %s-mon-%-3s " "$TAG" "$p"
    stat -c '%s bytes' "/tmp/${TAG}-mon-$p/link.pcap0" 2>/dev/null || echo MISSING
done
ps -o pid=,cmd= -C python3 | grep "linkstab.py --out /tmp/${TAG}" || echo "linkstab NOT RUNNING"
tail -6 "/tmp/${TAG}-linkstab/nohup.out"
