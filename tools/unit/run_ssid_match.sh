#!/bin/bash
# v2.6 SSID-aware path queries: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit
bash $U/extract.sh FlexNetCode.c \
    define:FLEXNET_LOCAL_BASE_MAX define:FLEX_TARGET_NODE \
    define:FLEX_TARGET_LOCAL \
    flex_split_call flex_call_in_range flex_chain_ends_at \
    flex_own_base_call flex_own_ssid_range flex_target_is_us \
    flex_session_for_call flex_probe_session > $U/extracted_ssid_match.inc
out="${TMPDIR:-/tmp}/test_ssid_match"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_ssid_match.c
"$out"
