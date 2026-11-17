#!/bin/bash
# Slow-poller (WAMPES) detection + type-1 interval: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c define:FLEXNET_SLOW_POLL_GAP \
    define:FLEXNET_LT_INTERVAL_PCF define:FLEXNET_LT_INTERVAL_XNET \
    flex_ka_gap_is_slow flex_lt_interval > $U/extracted_slow_poll.inc

out="${TMPDIR:-/tmp}/test_slow_poll"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_slow_poll.c
"$out"
