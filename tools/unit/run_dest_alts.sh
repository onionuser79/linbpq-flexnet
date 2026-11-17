#!/bin/bash
# D <call> * row layout: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c flex_dest_age flex_dest_alt_row \
    > $U/extracted_dest_alts.inc

out="${TMPDIR:-/tmp}/test_dest_alts"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_dest_alts.c
"$out"
