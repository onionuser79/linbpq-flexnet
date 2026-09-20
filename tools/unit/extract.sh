#!/bin/bash
# Pull the functions under test straight out of FlexNetCode.c so the unit
# tests can never drift from the shipped code. All of them return `int`.
set -euo pipefail
SRC="${1:-FlexNetCode.c}"
for fn in flex_build_route_rec flex_build_route flex_parse_compact_records \
          flex_climb_is_loop; do
    awk -v f="^static int $fn\\\\(" '
        $0 ~ f {inside=1}
        inside {print}
        inside && /^}$/ {exit}
    ' "$SRC"
    echo
done
