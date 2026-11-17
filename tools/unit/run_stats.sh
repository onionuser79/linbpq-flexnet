#!/bin/bash
# FLSTAT gauges and per-neighbour link counters: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c \
    struct:FLEX_STAT_GAUGE struct:FLEX_STAT_NBR \
    flex_stat_gauge flex_stat_nbr flex_stat_link_event \
    > $U/extracted_stats.inc

out="${TMPDIR:-/tmp}/test_stats"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_stats.c
"$out"
