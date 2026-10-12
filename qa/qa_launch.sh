#!/bin/bash
# qa_launch.sh - start one campaign detached on iw2ohx-gw (called by qa.sh start).
# usage: qa_launch.sh RUN_ID PROFILE LABEL [EXPECT_VERSION]
set -euo pipefail
RUN_ID=$1 PROFILE=$2 LABEL=$3 EXPECT=${4:-}
BIN=$HOME/qa/bin
OUT=$HOME/qa/runs/$RUN_ID
VENV=$HOME/pr-digi-mcp/.venv/bin/python
mkdir -p "$OUT"
[ -f "$HOME/qa/hosts-$RUN_ID.json" ] && mv "$HOME/qa/hosts-$RUN_ID.json" "$OUT/hosts.json"
ARGS=(--profile "$PROFILE" --label "$LABEL" --out "$OUT" --hosts-json "$OUT/hosts.json")
[ -n "$EXPECT" ] && ARGS+=(--expect-version "$EXPECT")
cd "$HOME/pr-digi-mcp"
PYTHONPATH=src PYTHON_KEYRING_BACKEND=keyring.backends.null.Keyring \
    setsid nohup "$VENV" "$BIN/qa_run.py" "${ARGS[@]}" >"$OUT/stdout.log" 2>&1 </dev/null &
echo $! >"$OUT/pid"
sleep 2
if kill -0 "$(cat "$OUT/pid")" 2>/dev/null; then
    echo "started $RUN_ID pid $(cat "$OUT/pid") -> $OUT"
else
    echo "FAILED to start $RUN_ID:"; cat "$OUT/stdout.log"; exit 1
fi
