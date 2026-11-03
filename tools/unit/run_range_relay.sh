#!/bin/bash
# v2.10 range relay: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c \
        define:FLEXNET_L2_MAX_DIGIS define:FLEXNET_L2_MAX_FRAME \
        define:FLEX_LOCAL_UNCHECKED define:FLEX_LOCAL_BOUND \
        define:FLEX_LOCAL_UNBOUND define:FLEX_LOCAL_NODECALL \
        define:FLEX_LOCAL_NOPORT define:FLEX_LOCAL_NOFWD \
        define:FLEX_LOCAL_OWNRANGE define:FLEX_LOCAL_COVERED \
        define:FLEX_RELAY_MAX define:FLEX_RELAY_IDLE_S \
        struct:FLEXNET_LOCAL_CALL struct:FLEX_RELAY_CIRCUIT
    echo 'static struct FLEXNET_LOCAL_CALL FlexNetLocalCalls[FLEXNET_MAX_LOCAL_CALLS];'
    echo 'static int FlexNetLocalCount = 0;'
    echo 'static struct FLEX_RELAY_CIRCUIT g_relay[FLEX_RELAY_MAX];'
    echo 'static unsigned long g_relay_out = 0, g_relay_back = 0, g_relay_drop = 0;'
    bash $U/extract.sh FlexNetCode.c \
        flex_normalize_callsign flex_split_call flex_local_add \
        flex_parse_external_line flex_local_find flex_local_resolve \
        flex_own_ssid_range \
        flex_l2_same_call flex_l2_addr_last flex_l2_digi_count \
        flex_l2_call_in_chain flex_l2_append_digi flex_l2_remove_digi \
        flex_relay_find flex_relay_live flex_relay_covered_port \
        flex_relay_send FlexNet_RangeRelayOut FlexNet_RangeRelayBack
} > $U/extracted_range_relay.inc

out="${TMPDIR:-/tmp}/test_range_relay"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_range_relay.c
"$out"
