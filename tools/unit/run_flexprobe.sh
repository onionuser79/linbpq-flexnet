#!/bin/bash
# FLEXPROBE parser + result ring: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c \
        define:FLEXNET_MAX_CALLSIGN define:FLEXNET_MAX_PATH_HOPS \
        define:FLEXNET_SYSOP_PROBE_RING define:FLEX_SPROBE_PENDING \
        define:FLEX_SPROBE_ANSWERED define:FLEX_SPROBE_LOOP \
        define:FLEX_SPROBE_WRONG_END define:FLEX_SPROBE_TIMEOUT \
        struct:FLEXNET_SYSOP_PROBE
    echo 'static struct FLEXNET_SYSOP_PROBE FlexNetSysopProbes[FLEXNET_SYSOP_PROBE_RING];'
    echo 'static int g_sysop_probe_next = 0;'
    bash $U/extract.sh FlexNetCode.c flex_sysop_probe_add flex_sysop_probe_close \
        flex_parse_probe_target
} > $U/extracted_flexprobe.inc

out="${TMPDIR:-/tmp}/test_flexprobe"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_flexprobe.c
"$out"
