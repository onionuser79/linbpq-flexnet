#!/bin/bash
# FLOG route-event ring: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c \
    define:FLEX_RLOG_SIZE define:FLEV_LINK_UP define:FLEV_LINK_DOWN \
    define:FLEV_REACHABLE define:FLEV_UNREACHABLE define:FLEV_NEXTHOP \
    define:FLEV_TABLE_REQ struct:FLEX_ROUTE_EVENT struct:FLEX_RLOG \
    flex_rlog_put flex_rlog_nth flex_rlog_classify flex_rlog_line \
    flex_rlog_match \
    > $U/extracted_rlog.inc

out="${TMPDIR:-/tmp}/test_rlog"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_rlog.c
"$out"
