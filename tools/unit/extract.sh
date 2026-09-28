#!/bin/bash
# Pull the code under test straight out of FlexNetCode.c so the unit
# tests can never drift from the shipped code.
#
# usage: extract.sh [SRC [ITEM ...]]
#
# An ITEM is one of
#   NAME           a static function, whatever it returns — including one
#                  whose return type sits on the line above the name
#                  (`static struct X *` / `NAME(...)`);
#   define:NAME    a single-line `#define NAME ...`;
#   struct:NAME    a `struct NAME { ... };` definition.
#
# With no items the default set is extracted, which is what
# test_compact_pack and test_climb_guard include. A test that needs more
# passes its own list and writes its own .inc -- see test_pcf_quiesce and
# test_l2_circuit.
set -euo pipefail
SRC="${1:-FlexNetCode.c}"
shift || true
if [ "$#" -eq 0 ]; then
    set -- flex_build_route_rec flex_build_route flex_parse_compact_records \
           flex_climb_is_loop
fi

fail() { echo "EXTRACT FAILED: $1" >&2; exit 1; }

for item in "$@"; do
    case "$item" in
    define:*)
        name="${item#define:}"
        grep -E "^#define ${name}([[:space:]]|$)" "$SRC" | head -1 | grep . \
            || fail "#define $name"
        ;;
    struct:*)
        name="${item#struct:}"
        awk -v s="^struct ${name}\$" '
            $0 ~ s {inside=1; found=1}
            inside {print}
            inside && /^};/ {exit}
            END {if (!found) exit 1}
        ' "$SRC" || fail "struct $name"
        ;;
    *)
        # Same line: `static <type> NAME(`. Split: `static <type>` then a
        # line starting `NAME(`. Either way the candidate is held until we
        # know what it is: a `{` line makes it the definition, a line
        # ending in `;` first makes it a forward prototype — discarded,
        # because the file declares most of its statics up front.
        awk -v same="^static .*[ *]${item}\\\\(" -v brk="^${item}\\\\(" '
            function start(s) { held = s; cand = 1 }
            inside {print}
            inside && /^}$/ {exit}
            inside {next}
            cand {
                held = held "\n" $0
                if ($0 ~ /^\{/) { print held; inside = 1; found = 1; cand = 0 }
                else if ($0 ~ /;[ \t]*$/) cand = 0
                next
            }
            $0 ~ same {
                if ($0 ~ /;[ \t]*$/) { prev = $0; next }   # 1-line prototype
                start($0); prev = $0; next
            }
            $0 ~ brk && prev ~ /^static / { start(prev "\n" $0); prev = $0; next }
            {prev = $0}
            END {if (!found) exit 1}
        ' "$SRC" || fail "function $item"
        ;;
    esac
    echo
done
