#!/usr/bin/env bash
#
# Two-stage sync:
#   1. macmini → SRC_REMOTE: mirror this repo (the "source of truth").
#   2. SRC_REMOTE → BUILD_REMOTE: overlay the FlexNet additions and modified
#      BPQ files on top of the full BPQ32 source tree where `make` runs.
#
# Step 2 is what was missing in the first version of this script: the rsync
# only updated SRC_REMOTE, but make runs in BUILD_REMOTE — so changes never
# reached the compiler. The overlay rsync uses `-a` without `--delete` so it
# updates files in linbpq-build without removing unmodified BPQ source files
# (6pack.c, adif.c, ...) that only live there.
set -euo pipefail

HOST=iw2ohx-gw
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_REMOTE="/home/iw2ohx/xnet_investigation_agent/linbpq-flexnet"
BUILD_REMOTE="/home/iw2ohx/xnet_investigation_agent/linbpq-build"

echo ">> stage 1: rsync $LOCAL_DIR/ -> $HOST:$SRC_REMOTE/"
rsync -az --delete \
    --exclude '.git' \
    --exclude '*.o' \
    --exclude '*.log' \
    --exclude 'sync-and-build.sh' \
    "$LOCAL_DIR/" "$HOST:$SRC_REMOTE/"

echo ">> stage 2: overlay $SRC_REMOTE/ -> $BUILD_REMOTE/ (no --delete)"
ssh "$HOST" "rsync -a \
    --exclude '.git' \
    --exclude '*.o' \
    --exclude '*.d' \
    --exclude '*.log' \
    --exclude 'sync-and-build.sh' \
    --exclude '.gitattributes' \
    --exclude '.gitignore' \
    --exclude 'README.md' \
    --exclude 'ROADMAP.md' \
    --exclude 'FEASIBILITY.md' \
    --exclude 'V1.3_DESIGN.md' \
    '$SRC_REMOTE/' '$BUILD_REMOTE/'"

echo ">> remote make in $BUILD_REMOTE"
ssh "$HOST" "cd '$BUILD_REMOTE' && make ${*:-}"
