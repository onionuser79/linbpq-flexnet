#!/bin/bash
# v2.4 per-link routing options: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c \
        define:FLEX_LOPT_NO_NBR define:FLEX_LOPT_NO_BEHIND \
        define:FLEX_LOPT_OWN_ONLY define:FLEX_LOPT_PENALTY \
        define:FLEX_LOPT_HIDDEN define:FLEXNET_LINK_PENALTY \
        struct:FLEXNET_LEARNED_ROUTE struct:FLEXNET_LEARNED_STATE
    echo 'static struct FLEXNET_LEARNED_STATE FlexNetLearned[FLEXNET_MAX_SESSIONS];'
    echo 'static int g_link_opts[FLEXNET_MAX_SESSIONS];'
    bash $U/extract.sh FlexNetCode.c \
        FlexNet_ParseLinkOpts flex_link_opts_format \
        flex_link_opts_source_allows flex_link_opts_cost flex_sess_link_opts \
        flex_sess_peer_call flex_dest_is_session_peer flex_expected_rtt \
        flex_climb_is_loop
} > $U/extracted_link_opts.inc

out="${TMPDIR:-/tmp}/test_link_opts"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_link_opts.c
"$out"
