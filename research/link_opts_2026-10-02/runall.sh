#!/usr/bin/env bash
# runall.sh — v2.4 option matrix on IR2UFV. Run on gw with sudo.
set -u
T=/home/iw2ohx/v24test
BIN=$T/linbpq.v2.4.0-rc1.flexdebug
OUT=$T/matrix.txt
: > $OUT
run() {   # name opt14 opt12
    local name=$1 o14=$2 o12=$3
    local P=/tmp/v24-$name-$(date +%H%M%S).pcap
    echo "=== $name  -14:$o14  -12:$o12  pcap=$P" >> $OUT
    $T/ufv24.sh opts "$o14" "$o12" >> $OUT
    timeout 260 tcpdump -i any -s0 -w $P 'udp port 10075 and (host 192.168.1.201 or host 44.134.24.4)' >/dev/null 2>&1 &
    sleep 2
    local start=$(date +%s)
    $T/ufv24.sh run $BIN >> $OUT
    echo "start=$start" >> $OUT
    sleep 230
    sudo -u iw2ohx python3 $T/nodecmd.py 127.0.0.1 2525 Marco <pw> 3 FL | tr -s '\r\n' '\n' | sed -n '/FlexNet Links/,$p' >> $OUT
    grep -a "unknown link option" /home/bpq-ufv/nohup.out | tail -2 >> $OUT
    wait
    echo "--- records sent after start (new process only):" >> $OUT
    for dst in 192.168.1.201 44.134.24.4; do
        src=192.168.1.202; [ $dst = 44.134.24.4 ] && src=44.144.128.131
        python3 $T/cerecs.py $P $src $dst 2>/dev/null | awk -v s=$start -v d=$dst '$1>=s {n++; if ($2=="IR2UFV") own++; else if ($4>=60000) w++; else if ($4>=2000) pen++; c[$2" "$3]++} END {printf "to %s: records=%d own=%d withdrawn=%d penalised=%d other=%d\n", d, n, own, w, pen, n-own-w-pen}' >> $OUT
        python3 $T/cerecs.py $P $src $dst 2>/dev/null | awk -v s=$start '$1>=s && $2 ~ /^IW2OHX$/' | sort -u -k2 >> $OUT
    done
}
run run2 'F!' 'F'
run run3 'F-' 'F'
run run4 'F'  'F='
run run5 'F'  'F*'
echo DONE >> $OUT
