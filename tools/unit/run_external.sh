#!/bin/bash
# v2.6 FLEXNETEXTERNAL: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c \
        define:FLEX_LOCAL_UNCHECKED define:FLEX_LOCAL_BOUND \
        define:FLEX_LOCAL_UNBOUND define:FLEX_LOCAL_NODECALL \
        define:FLEX_LOCAL_NOPORT define:FLEX_LOCAL_NOFWD \
        struct:FLEXNET_LOCAL_CALL
    echo 'static struct FLEXNET_LOCAL_CALL FlexNetLocalCalls[FLEXNET_MAX_LOCAL_CALLS];'
    echo 'static int FlexNetLocalCount = 0;'
    bash $U/extract.sh FlexNetCode.c \
        flex_normalize_callsign flex_split_call flex_local_add \
        flex_parse_local_line flex_parse_external_line flex_local_find \
        flex_external_port flex_local_covers flex_local_resolve \
        flex_build_route_rec flex_build_own_frame flex_target_is_us \
        FlexNet_IsLocalCall
} > $U/extracted_external.inc

out="${TMPDIR:-/tmp}/test_external"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_external.c
"$out"
