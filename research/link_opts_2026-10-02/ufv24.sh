#!/usr/bin/env bash
# IR2UFV v2.4 per-link options test control. Run on iw2ohx-gw with sudo.
#   ufv24.sh backup                       save cfg + current binary (once)
#   ufv24.sh opts <14suffix> <12suffix>   MAP F-suffix per peer ('-' none... use 'F' forms: F, F>, F+ ...)
#   ufv24.sh run <binary>                 install binary and restart IR2UFV
#   ufv24.sh restore                      original cfg + binary, restart
set -euo pipefail
D=/home/bpq-ufv
CFG=$D/bpq32.cfg
BK=/home/iw2ohx/v24test
ORIG_CFG=$BK/bpq32.cfg.orig
ORIG_BIN=$BK/linbpq.v2.3.1.orig

restart() {
    pkill -9 -f "^$D/linbpq" || true
    sleep 2
    cp "$1" $D/linbpq
    setsid bash -c "cd $D && exec nohup $D/linbpq </dev/null >>$D/nohup.out 2>&1" </dev/null >/dev/null 2>&1 &
    sleep 8
    echo "pid $(pgrep -f "^$D/linbpq" | head -1)  md5 $(md5sum < $D/linbpq | cut -c1-12)"
}

case "$1" in
backup)
    [ -e "$ORIG_CFG" ] || cp -p $CFG "$ORIG_CFG"
    [ -e "$ORIG_BIN" ] || cp -p $D/linbpq "$ORIG_BIN"
    ls -l "$ORIG_CFG" "$ORIG_BIN"
    ;;
opts)
    cp "$ORIG_CFG" $CFG
    sed -i "s|^\(\s*MAP IW2OHX-14 44.134.24.4 UDP 10075 B\) F\s*$|\1 $2   ; v24test|" $CFG
    sed -i "s|^\(\s*MAP IW2OHX-12 192.168.1.201 UDP 10075\) F\s*$|\1 $3   ; v24test|" $CFG
    grep -n "^\s*MAP IW2OHX-1[24]" $CFG
    ;;
run)
    restart "$2"
    ;;
restore)
    cp "$ORIG_CFG" $CFG
    restart "$ORIG_BIN"
    grep -c v24test $CFG || true
    ;;
esac
