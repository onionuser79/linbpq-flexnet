#!/bin/bash
# v2.6 own-record guard (base call + SSID range): extract + build
# (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit
bash $U/extract.sh FlexNetCode.c flex_own_ssid_range flex_record_is_ours \
    > $U/extracted_own_record.inc
out="${TMPDIR:-/tmp}/test_own_record"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_own_record.c
"$out"
