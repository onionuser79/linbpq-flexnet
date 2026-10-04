#!/bin/bash
# v2.5 FlexNet over KISS ports: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

{
    bash $U/extract.sh FlexNetCode.c \
        define:FLEX_LOPT_NO_NBR define:FLEX_LOPT_NO_BEHIND \
        define:FLEX_LOPT_OWN_ONLY define:FLEX_LOPT_PENALTY \
        define:FLEX_LOPT_HIDDEN \
        define:FLEXNET_MAX_PORT_LINKS define:FLEXNET_MAX_FLEX_PORTS \
        define:FLEXNET_PORTLINK_SCAN define:FLEXNET_PORTLINK_FIRST \
        define:FLEXNET_PORTLINK_REOPEN define:FLEXNET_PORTLINK_RETRY \
        define:FLEXNET_PORTLINK_RETRY_MAX define:FLEX_HW_ASYNC \
        define:FLEX_HW_I2C define:FLEXNET_KA_ECHO_GAP \
        struct:FLEXNET_PORT_LINK
    echo 'static struct FLEXNET_PORT_LINK g_port_links[FLEXNET_MAX_PORT_LINKS];'
    echo 'static int    g_port_link_count = 0;'
    echo 'static int    g_flex_ports[FLEXNET_MAX_FLEX_PORTS];'
    echo 'static int    g_flex_port_count = 0;'
    echo 'static time_t g_port_link_scanned = 0;'
    echo 'static time_t g_ka_echo_at[FLEXNET_MAX_SESSIONS];'
    bash $U/extract.sh FlexNetCode.c \
        FlexNet_ParseLinkOpts flex_port_key flex_port_is_flexnet \
        flex_parse_port_link flex_parse_port_block_line \
        flex_port_links_resolve flex_port_link_find FlexNet_PortLinkOpts \
        flex_port_link_open flex_port_links_keep flex_dest_cost_here \
        flex_ka_should_echo
} > $U/extracted_kiss.inc

out="${TMPDIR:-/tmp}/test_kiss_links"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_kiss_links.c
"$out"
