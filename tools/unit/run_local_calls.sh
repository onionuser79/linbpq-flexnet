#!/bin/bash
# v2.3 local APPLICATION calls: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

# Struct first, then the globals the extracted code touches (declared
# here so the functions stay a pure lift from FlexNetCode.c), then code.
{
    bash tools/unit/extract.sh FlexNetCode.c struct:FLEXNET_LOCAL_CALL
    echo 'static struct FLEXNET_LOCAL_CALL FlexNetLocalCalls[FLEXNET_MAX_LOCAL_CALLS];'
    echo 'static int FlexNetLocalCount = 0;'
    bash tools/unit/extract.sh FlexNetCode.c \
        flex_normalize_callsign flex_split_call flex_local_add \
        flex_parse_local_line flex_local_find flex_local_covers \
        flex_local_collect_apps flex_local_resolve flex_local_state_name \
        flex_local_format flex_build_route_rec flex_build_own_frame \
        flex_parse_compact_records flex_call_in_range flex_own_base_call \
        flex_own_ssid_range flex_target_is_us \
        FlexNet_IsLocalCall FlexNet_MarkLocalDigi
} > tools/unit/extracted_local.inc

out="${TMPDIR:-/tmp}/test_local_calls"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I tools/unit -o "$out" tools/unit/test_local_calls.c
"$out"
