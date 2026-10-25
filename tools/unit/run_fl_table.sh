#!/bin/bash
# FL link table ((X)Net L layout): extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c struct:flex_fl_row flex_fl_bytes \
    flex_fl_connect flex_fl_pct flex_fl_rr_tenths flex_fl_ticks_10ms \
    flex_fl_qt_avg flex_fl_num flex_fl_format_row \
    > $U/extracted_fl_table.inc

out="${TMPDIR:-/tmp}/test_fl_table"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_fl_table.c
"$out"
