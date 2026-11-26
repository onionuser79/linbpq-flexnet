#!/bin/bash
# Monitor FlexNet / other-PID decode (Moncode.c): extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh Moncode.c define:MON_IP_PREVIEW define:MON_IP_FIELD \
    define:MON_CE_BUDGET define:MON_CE_CELLS define:MON_CE_INFINITY define:MON_CE_TEXT \
    struct:MonCERec MonPut MonIPPreview MonCERecord MonCECell MonCERoutes MonCEText \
    MonCEPath DISPLAYFLEXNET DISPLAYOTHERPID > $U/extracted_flexnet_decode.inc

out="${TMPDIR:-/tmp}/test_flexnet_decode"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_flexnet_decode.c
"$out"
