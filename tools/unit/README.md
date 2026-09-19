# Unit tests

No test framework existed in this repo; these are self-contained C
programs with no dependencies beyond libc.

`extract.sh` lifts the functions under test **verbatim out of
`FlexNetCode.c`** into `extracted.inc`, so a test can never drift from
the code that ships. The functions are `static` and the file cannot be
compiled standalone, which is why they are extracted rather than linked.

## Running

```sh
bash tools/unit/extract.sh > tools/unit/extracted.inc
gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -g \
    -fsanitize=address,undefined -I tools/unit \
    -o /tmp/test_compact_pack tools/unit/test_compact_pack.c
/tmp/test_compact_pack
```

`extracted.inc` is generated; it is not committed.

## test_compact_pack.c

Pins the compact CE route encoding — one `'3'` per **frame**, then N
records, then `CR`. Guards the defect fixed in v2.2.0-rc6, where the
emitter sent one record per AX.25 I-frame (15 bytes of a 236-byte
`PACLEN`) while every peer packs to 205–248 bytes.

Several cases assert against **bytes captured from real (X)Net and
PC/Flexnet peers on 2026-09-19**, so the test also documents the wire
format. See `research/link_stability_2026-09-19/PACKED_ADVERTISEMENTS.md`.
