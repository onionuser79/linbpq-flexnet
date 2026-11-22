#!/bin/bash
# v2.11.3 LOCALS / direct APPLICATION connects: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

{
    bash tools/unit/extract.sh FlexNetCode.c flex_split_call flex_local_reach \
        FlexNet_StripStayFlag flex_alias_targets
} > tools/unit/extracted_locals.inc

out="${TMPDIR:-/tmp}/test_locals"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I tools/unit -o "$out" tools/unit/test_locals.c
"$out"
