#!/bin/bash
# Pull the functions under test straight out of FlexNetCode.c so the unit
# tests can never drift from the shipped code. All of them return `int`.
#
# usage: extract.sh [SRC [FN ...]]
#
# With no function names the default set is extracted, which is what
# test_compact_pack and test_climb_guard include. A test whose function
# needs globals the others do not declare passes its own list and writes
# its own .inc -- see test_pcf_quiesce.
set -euo pipefail
SRC="${1:-FlexNetCode.c}"
shift || true
if [ "$#" -eq 0 ]; then
    set -- flex_build_route_rec flex_build_route flex_parse_compact_records \
           flex_climb_is_loop
fi
for fn in "$@"; do
    awk -v f="^static int $fn\\\\(" '
        $0 ~ f {inside=1; found=1}
        inside {print}
        inside && /^}$/ {exit}
        END {if (!found) {print "EXTRACT FAILED: " f > "/dev/stderr"; exit 1}}
    ' "$SRC"
    echo
done
