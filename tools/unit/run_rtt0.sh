#!/bin/bash
# Cost-0 record = withdrawal of the sender's offer: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c \
    struct:FLEXNET_DEST_ENTRY \
    define:FLEX_RTT0_NONE define:FLEX_RTT0_WITHDRAW define:FLEX_RTT0_DROP \
    flex_rtt0_rewrite \
    > $U/extracted_rtt0.inc

out="${TMPDIR:-/tmp}/test_rtt0"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_rtt0.c
"$out"
