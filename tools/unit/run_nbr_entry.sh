#!/bin/bash
# B-3 neighbour entry range from INIT: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c \
        struct:FLEXNET_LEARNED_ROUTE struct:FLEXNET_LEARNED_STATE \
        struct:FLEXNET_DEST_ENTRY
    echo 'static struct FLEXNET_LEARNED_STATE FlexNetLearned[FLEXNET_MAX_SESSIONS];'
    echo 'static BOOL g_nbr_entered[FLEXNET_MAX_SESSIONS];'
    echo 'static BOOL g_flexnet_transit_enabled = TRUE;'
    echo 'static int flex_dtable_merge(struct FLEXNET_DEST_ENTRY * in, struct FLEXNET_SESSION * sess);'
    bash $U/extract.sh FlexNetCode.c flex_add_neighbour_entry
} > $U/extracted_nbr_entry.inc

out="${TMPDIR:-/tmp}/test_nbr_entry"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_nbr_entry.c
"$out"
