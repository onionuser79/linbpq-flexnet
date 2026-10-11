#!/bin/bash
# B-1 full-table resync (FLEXNETRESYNC): extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c \
    define:FLEXNET_RESYNC_QUIET define:FLEXNET_RESYNC_TIMEOUT \
    define:FLEXNET_RESYNC_MIN_PCT \
    define:FLEX_RESYNC_WAIT define:FLEX_RESYNC_SWEEP define:FLEX_RESYNC_ABORT \
    struct:FLEXNET_LEARNED_ROUTE struct:FLEXNET_LEARNED_STATE \
    flex_resync_begin flex_resync_note flex_resync_tally flex_resync_verdict \
    flex_resync_is_stale flex_parse_int_line \
    > $U/extracted_resync.inc

out="${TMPDIR:-/tmp}/test_resync"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_resync.c
"$out"
