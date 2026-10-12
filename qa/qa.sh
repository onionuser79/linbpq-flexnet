#!/bin/bash
# qa.sh - FlexNet QA campaigns, driven from macmini. The only entry point the
# flexnet-qa skill uses, so every campaign is launched, watched and scored the
# same way.
#
#   qa.sh deploy                         copy the harness to gw (~/qa/bin)
#   qa.sh start PROFILE LABEL [vX.Y.Z]   preflight hosts + launch detached on gw
#   qa.sh status [RUN_ID]                progress of a run (default: newest)
#   qa.sh stop RUN_ID                    stop a run early (scorecard not produced)
#   qa.sh fetch RUN_ID [BASELINE_ID]     pull to research/qa_runs/, score, history
#   qa.sh list                           runs on gw and fetched locally
set -euo pipefail
GW=iw2ohx-gw
BPQ=iw2ohx-bpq
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
LOCAL_RUNS=$REPO/research/qa_runs
TOOLS_NEEDED=(sem_watch.py)

die() { echo "qa.sh: $*" >&2; exit 2; }

newest() { ssh "$GW" 'ls -1t ~/qa/runs 2>/dev/null | head -1'; }

cmd_deploy() {
    ssh "$GW" 'mkdir -p ~/qa/bin ~/qa/runs ~/xnet_investigation_agent/linbpq-flexnet/tools'
    rsync -a "$HERE/qa_run.py" "$HERE/qa_score.py" "$HERE/qa_lib.py" "$HERE/qa_config.json" \
        "$HERE/qa_launch.sh" "$GW:qa/bin/"
    for t in "${TOOLS_NEEDED[@]}"; do
        rsync -a "$REPO/tools/$t" "$GW:xnet_investigation_agent/linbpq-flexnet/tools/"
    done
    ssh "$GW" 'cd ~/qa/bin && sha256sum qa_*.py qa_config.json | cut -c1-12,65-'
}

# Facts only macmini can see: the Windows node's cfg and macmini's own pollers.
hosts_json() {
    local doc ver2 mon
    local cfg='C:\Program Files (x86)\LinBPQ\bpq32.cfg'
    doc=$(ssh "$BPQ" "Select-String -Path '$cfg' -Pattern '^\s*DisconnectOnClose\s*=\s*0' -Quiet" 2>/dev/null | tr -d '\r' || echo False)
    ver2=$(ssh "$BPQ" "Select-String -Path '$cfg' -Pattern '^\s*OnlyVer2point0\s*=\s*1' -Quiet" 2>/dev/null | tr -d '\r' || echo False)
    mon=$(launchctl list 2>/dev/null | grep -c 'iw2ohx' || true)
    printf '{"cfg": {"IW2OHX-12": {"DisconnectOnClose=0": %s, "OnlyVer2point0=1": %s, "readable": true}},' \
        "$( [ "$doc" = True ] && echo true || echo false)" "$( [ "$ver2" = True ] && echo true || echo false)"
    printf ' "macmini_launchagents_iw2ohx": %s, "macmini_utc": "%s"}\n' "$mon" "$(date -u +%FT%TZ)"
}

cmd_start() {
    local profile=${1:-} label=${2:-} expect=${3:-}
    [[ $profile =~ ^(smoke|standard|soak)$ ]] || die "profile must be smoke|standard|soak"
    [ -n "$label" ] || die "label required (e.g. v2.6.3-rc1)"
    local busy
    busy=$(ssh "$GW" 'for d in ~/qa/runs/*/; do [ -f "$d/pid" ] && [ ! -f "$d/DONE" ] && kill -0 "$(cat "$d/pid")" 2>/dev/null && basename "$d"; done; true')
    [ -z "$busy" ] || die "a campaign is already running: $busy (one at a time: two harnesses double the polling)"
    local id="${label}-${profile}-$(date -u +%Y%m%dT%H%MZ)"
    hosts_json >"/tmp/qa-hosts-$id.json"
    scp -q "/tmp/qa-hosts-$id.json" "$GW:qa/hosts-$id.json"
    rm -f "/tmp/qa-hosts-$id.json"
    ssh "$GW" "bash ~/qa/bin/qa_launch.sh '$id' '$profile' '$label' '$expect'"
}

cmd_status() {
    local id=${1:-$(newest)}
    [ -n "$id" ] || die "no runs on gw"
    ssh "$GW" "d=~/qa/runs/$id; echo \"run: $id\";
        if [ -f \$d/DONE ]; then echo state: DONE \$(cat \$d/DONE);
        elif kill -0 \$(cat \$d/pid 2>/dev/null) 2>/dev/null; then echo state: RUNNING;
        else echo state: DEAD; fi
        echo snapshots: \$(grep -c '\"tag\": \"S' \$d/snaps.jsonl 2>/dev/null || echo 0) \
             connects: \$(wc -l <\$d/conn.jsonl 2>/dev/null || echo 0) \
             pathq: \$(wc -l <\$d/pathq.jsonl 2>/dev/null || echo 0)
        echo --- run.log; tail -8 \$d/run.log 2>/dev/null; tail -3 \$d/stdout.log 2>/dev/null"
}

cmd_stop() {
    local id=${1:-}; [ -n "$id" ] || die "RUN_ID required"
    # qa_run.py is a setsid session leader: signal its whole process group
    # (sudo relays TERM to the sem_watch children). Never pkill -f here: the
    # pattern would match this ssh command's own shell.
    ssh "$GW" "d=~/qa/runs/$id; p=\$(cat \$d/pid); kill -TERM -- -\$p 2>/dev/null; echo stopped \$p;
        echo 'STOPPED early' >\$d/STOPPED"
}

cmd_fetch() {
    local id=${1:-}; local base=${2:-}
    [ -n "$id" ] || die "RUN_ID required"
    mkdir -p "$LOCAL_RUNS"
    rsync -a --exclude 'pid' "$GW:qa/runs/$id" "$LOCAL_RUNS/"
    local args=("$LOCAL_RUNS/$id" --config "$HERE/qa_config.json" --history "$HERE/history.csv")
    if [ -n "$base" ]; then
        [ -f "$LOCAL_RUNS/$base/scorecard.json" ] || die "baseline $base not fetched"
        args+=(--baseline "$LOCAL_RUNS/$base/scorecard.json")
    fi
    python3 "$HERE/qa_score.py" "${args[@]}" || true
    gzip -f "$LOCAL_RUNS/$id/snaps.jsonl"
    echo "-> $LOCAL_RUNS/$id/SCORECARD.md"
}

cmd_list() {
    echo "gw:"; ssh "$GW" 'ls -1t ~/qa/runs 2>/dev/null' | sed 's/^/  /'
    echo "local:"; ls -1t "$LOCAL_RUNS" 2>/dev/null | sed 's/^/  /'
}

sub=${1:-}; shift || true
case $sub in
    deploy) cmd_deploy ;;
    start) cmd_start "$@" ;;
    status) cmd_status "$@" ;;
    stop) cmd_stop "$@" ;;
    fetch) cmd_fetch "$@" ;;
    list) cmd_list ;;
    *) sed -n '2,11p' "$0"; exit 2 ;;
esac
