#!/bin/bash
# v2.6 path-answer loop guard: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit
bash $U/extract.sh FlexNetCode.c flex_chain_has_call flex_sess_peer_call \
    flex_target_is_direct_peer > $U/extracted_path_loop.inc
out="${TMPDIR:-/tmp}/test_path_loop"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_path_loop.c
"$out"
