#!/bin/bash
# B-3 invented neighbour entry: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c \
        define:FLEX_LOPT_NO_NBR define:FLEX_LOPT_NO_BEHIND \
        define:FLEX_LOPT_OWN_ONLY define:FLEX_LOPT_PENALTY \
        define:FLEX_LOPT_HIDDEN define:FLEXNET_LINK_PENALTY \
        struct:FLEXNET_LEARNED_ROUTE struct:FLEXNET_LEARNED_STATE \
        struct:FLEXNET_DEST_ENTRY
    echo 'static struct FLEXNET_LEARNED_STATE FlexNetLearned[FLEXNET_MAX_SESSIONS];'
    echo 'static struct FLEXNET_DEST_ENTRY FlexNetDests[16];'
    echo 'static int FlexNetDestCount = 0;'
    echo 'static int g_link_opts[FLEXNET_MAX_SESSIONS];'
    echo 'static BOOL g_flexnet_transit_enabled = TRUE;'
    echo 'static BOOL g_flexnet_l2_transit_enabled = TRUE;'
    echo 'static BOOL g_flexnet_crossport_enabled = FALSE;'
    bash $U/extract.sh FlexNetCode.c \
        flex_crossport_active flex_link_opts_source_allows flex_link_opts_cost \
        flex_sess_link_opts flex_dest_cost_here flex_sess_peer_call \
        flex_dest_is_session_peer flex_find_dest flex_dest_reelect \
        flex_learned_retire_synthetic flex_expected_rtt
} > $U/extracted_synthetic.inc

out="${TMPDIR:-/tmp}/test_synthetic"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_synthetic.c
"$out"
