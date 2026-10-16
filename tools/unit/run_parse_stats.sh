#!/bin/bash
# FL "Parser:" counters + footer: extract + build (-Werror, ASan/UBSan) + run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c \
    define:FLEX_PARSE_UNKNOWN define:FLEX_PARSE_MALFORMED \
    struct:FLEX_PARSE_STATS flex_parse_note flex_parse_footer \
    > $U/extracted_parse_stats.inc

out="${TMPDIR:-/tmp}/test_parse_stats"
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$out" $U/test_parse_stats.c
"$out"
