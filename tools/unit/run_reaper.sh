#!/bin/bash
# B-5 session reaper decision: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c \
    define:FLEXNET_REAP_GRACE define:FLEX_REAP_KEEP define:FLEX_REAP_WAIT \
    define:FLEX_REAP_NOW flex_l2_connected flex_reap_decide \
    > $U/extracted_reaper.inc

out="${TMPDIR:-/tmp}/test_reaper"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_reaper.c
"$out"
