#!/bin/bash
# Pull the wire-format functions straight out of FlexNetCode.c so the unit
# test can never drift from the shipped code.
set -euo pipefail
SRC="${1:-FlexNetCode.c}"
for fn in flex_build_route_rec flex_build_route flex_parse_compact_records; do
    awk -v f="^static int $fn\\\\(" '
        $0 ~ f {inside=1}
        inside {print}
        inside && /^}$/ {exit}
    ' "$SRC"
    echo
done
