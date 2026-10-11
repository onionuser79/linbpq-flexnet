#!/bin/bash
# B-4 route choice for C / D <call>: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c struct:FLEXNET_DEST_ENTRY
    echo 'static struct FLEXNET_DEST_ENTRY FlexNetDests[16];'
    echo 'static int FlexNetDestCount = 0;'
    bash $U/extract.sh FlexNetCode.c \
        flex_dest_cost_here flex_find_best_dest flex_find_dest_for_target
} > $U/extracted_best_dest.inc

out="${TMPDIR:-/tmp}/test_best_dest"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_best_dest.c
"$out"
