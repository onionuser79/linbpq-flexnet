#!/bin/bash
# FL "Q/T" column cell: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c flex_fl_qt_cell \
    > $U/extracted_fl_qt.inc

out="${TMPDIR:-/tmp}/test_fl_qt"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_fl_qt.c
"$out"
