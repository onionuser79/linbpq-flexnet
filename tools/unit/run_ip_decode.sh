#!/bin/bash
# Monitor IP decode (Moncode.c): extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh Moncode.c define:MON_IP_PREVIEW define:MON_IP_FIELD MonPut MonIPPreview MonICMPName \
    MonTCP MonUDP MonICMP DISPLAYIPDATAGRAM > $U/extracted_ip_decode.inc

out="${TMPDIR:-/tmp}/test_ip_decode"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_ip_decode.c
"$out"
