#!/usr/bin/env bash
set -euo pipefail

HOST=iw2ohx-gw
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_REMOTE="/home/iw2ohx/xnet_investigation_agent/linbpq-flexnet"
BUILD_REMOTE="/home/iw2ohx/xnet_investigation_agent/linbpq-build"

echo ">> rsync $LOCAL_DIR/ -> $HOST:$SRC_REMOTE/"
rsync -az --delete \
    --exclude '.git' \
    --exclude '*.o' \
    --exclude '*.log' \
    --exclude 'sync-and-build.sh' \
    "$LOCAL_DIR/" "$HOST:$SRC_REMOTE/"

echo ">> remote make in $BUILD_REMOTE"
ssh "$HOST" "cd '$BUILD_REMOTE' && make ${*:-}"
