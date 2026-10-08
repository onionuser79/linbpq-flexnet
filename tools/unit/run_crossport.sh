#!/bin/bash
# v2.6 cross-port L2 forwarding: extract FlexNet_L2Transit() and everything
# it calls, then build (-Werror, ASan/UBSan) and run test_crossport.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c \
    define:FLEXNET_L2_MAX_DIGIS define:FLEXNET_MAX_L2_TRANSIT \
    define:FLEXNET_L2_TRANSIT_LINGER define:FLEXNET_L2_TRANSIT_EVICT \
    define:FLEXNET_L2_TRANSIT_IDLE define:FLEXNET_L2_MAX_FRAME \
    define:FLEX_L2K_OTHER define:FLEX_L2K_SABM define:FLEX_L2K_DISC \
    define:FLEX_L2K_UA define:FLEX_L2K_DM \
    define:FLEX_L2S_LIVE define:FLEX_L2S_EVICTABLE define:FLEX_L2S_EXPIRED \
    struct:FLEXNET_L2_TRANSIT > $U/extracted_l2_types.inc

bash $U/extract.sh FlexNetCode.c \
    define:FLEX_LOCAL_BOUND struct:FLEXNET_LOCAL_CALL \
    > $U/extracted_xport_types.inc

bash $U/extract.sh FlexNetCode.c \
    flex_crossport_active flex_normalize_callsign flex_split_call \
    flex_local_find flex_external_port flex_session_for_call \
    flex_l2_same_call flex_l2_addr_last flex_l2_digi_count \
    flex_l2_call_in_chain flex_l2_append_digi flex_l2_remove_digi \
    flex_l2_ctl_kind flex_l2_slot_state flex_l2_must_repin \
    flex_l2_is_our_hop flex_l2_note_ctl flex_l2_self_repeated \
    flex_l2_find flex_l2_out_eff flex_l2_find_rev flex_l2_out_port \
    flex_l2_resolve_hop flex_l2_contract flex_l2_pick_hop flex_l2_extend \
    flex_l2_external_port flex_l2_peer_port flex_port_carries_flexnet \
    flex_l2_note_adjacent flex_l2_choose_port FlexNet_L2Transit \
    > $U/extracted_xport.inc

out="${TMPDIR:-/tmp}/test_crossport"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_crossport.c
"$out"
