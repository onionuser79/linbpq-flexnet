#!/bin/bash
# Peer re-INIT answer decision: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c \
    define:FLEXNET_REINIT_GUARD flex_reinit_answer \
    > $U/extracted_reinit.inc

out="${TMPDIR:-/tmp}/test_reinit"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_reinit.c
"$out"
