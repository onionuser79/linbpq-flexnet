#!/bin/bash
# Type-1 link-time value we send: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c define:FLEXNET_WIRE_LT flex_wire_lt_value > $U/extracted_wire_lt.inc

out="${TMPDIR:-/tmp}/test_wire_lt"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_wire_lt.c
"$out"
