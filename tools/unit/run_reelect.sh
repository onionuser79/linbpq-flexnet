#!/bin/bash
# B-2 D-table next-hop re-election: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c \
        define:FLEX_LOPT_PENALTY define:FLEXNET_LINK_PENALTY \
        struct:FLEXNET_LEARNED_ROUTE struct:FLEXNET_LEARNED_STATE \
        struct:FLEXNET_DEST_ENTRY
    echo 'static struct FLEXNET_LEARNED_STATE FlexNetLearned[FLEXNET_MAX_SESSIONS];'
    echo 'static struct FLEXNET_DEST_ENTRY FlexNetDests[16];'
    echo 'static int FlexNetDestCount = 0;'
    echo 'static int g_link_opts[FLEXNET_MAX_SESSIONS];'
    echo 'static BOOL g_flexnet_transit_enabled = TRUE;'
    bash $U/extract.sh FlexNetCode.c \
        flex_link_opts_cost flex_sess_link_opts flex_dest_cost_here \
        flex_sess_peer_call flex_dest_reelect flex_dest_reelect_via
} > $U/extracted_reelect.inc

out="${TMPDIR:-/tmp}/test_reelect"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_reelect.c
"$out"
