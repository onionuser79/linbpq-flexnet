#!/usr/bin/env bash
# IR2UFV patch-0001 test control. Run on iw2ohx-gw with sudo.
#   ufvctl.sh backup                 save cfg + current binary (once)
#   ufvctl.sh cfg <0|1>              test cfg with SECURETELNET=<n>
#   ufvctl.sh run <binary>           install binary and restart IR2UFV
#   ufvctl.sh restore                original cfg + binary, restart
set -euo pipefail

D=/home/bpq-ufv
CFG=$D/bpq32.cfg
BK=/home/iw2ohx/patchtest
ORIG_CFG=$BK/bpq32.cfg.orig
ORIG_BIN=$BK/linbpq.v2.3.0.orig

restart() {
    pkill -9 -f "^$D/linbpq" || true
    sleep 2
    cp "$1" $D/linbpq
    cd $D
    setsid nohup $D/linbpq >> $D/nohup.out 2>&1 < /dev/null &
    sleep 8
    echo "pid $(pgrep -f "^$D/linbpq" | head -1)  md5 $(md5sum < $D/linbpq | cut -c1-12)"
}

case "$1" in
backup)
    [ -e "$ORIG_CFG" ] || cp -p $CFG "$ORIG_CFG"
    [ -e "$ORIG_BIN" ] || cp -p $D/linbpq "$ORIG_BIN"
    ls -l "$ORIG_CFG" "$ORIG_BIN"
    ;;
cfg)
    cp "$ORIG_CFG" $CFG
    sed -i "s|^\(\s*\)DisconnectOnClose=0|&\n\1SECURETELNET=$2   ; patchtest|" $CFG
    sed -i "s|^APPLICATION 1,BBS,,IR2UFV-8,UFVBBS,255|&\nAPPLICATION 2,UFVT,ATTACH 1 127.0.0.1 63999 S,IR2UFX,UFVTST,0   ; patchtest\nFLEXNETLOCAL IR2UFX   ; patchtest|" $CFG
    grep -n "patchtest" $CFG
    ;;
run)
    restart "$2"
    ;;
restore)
    cp "$ORIG_CFG" $CFG
    restart "$ORIG_BIN"
    grep -c patchtest $CFG || true
    ;;
esac
