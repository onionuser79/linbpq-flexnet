#!/bin/bash
# Extract the L2 transit circuit code from FlexNetCode.c and run
# test_l2_circuit under ASan/UBSan. Run from the repo root.
set -euo pipefail
cd "$(dirname "$0")/../.."
U=tools/unit
OUT="${TMPDIR:-/tmp}/test_l2_circuit"

bash $U/extract.sh FlexNetCode.c \
    define:FLEXNET_L2_MAX_DIGIS define:FLEXNET_MAX_L2_TRANSIT \
    define:FLEXNET_L2_TRANSIT_LINGER define:FLEXNET_L2_TRANSIT_EVICT \
    define:FLEXNET_L2_TRANSIT_IDLE define:FLEXNET_L2_MAX_FRAME \
    define:FLEX_L2K_OTHER define:FLEX_L2K_SABM define:FLEX_L2K_DISC \
    define:FLEX_L2K_UA define:FLEX_L2K_DM \
    define:FLEX_L2S_LIVE define:FLEX_L2S_EVICTABLE define:FLEX_L2S_EXPIRED \
    struct:FLEXNET_L2_TRANSIT > $U/extracted_l2_types.inc

bash $U/extract.sh FlexNetCode.c \
    flex_l2_same_call flex_l2_addr_last flex_l2_digi_count \
    flex_l2_call_in_chain flex_l2_append_digi flex_l2_remove_digi \
    flex_l2_ctl_kind flex_l2_slot_state flex_l2_must_repin \
    flex_l2_is_our_hop flex_l2_note_ctl flex_l2_self_repeated \
    flex_l2_active_circuits flex_l2_find > $U/extracted_l2.inc

gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I $U \
    -o "$OUT" $U/test_l2_circuit.c
"$OUT"
