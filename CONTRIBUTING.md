# Contributing

## How the repository is organised

This is an **overlay** on upstream LinBPQ (`g8bpq/LinBPQ`, configured as
the `upstream` remote). It holds only files that are new or differ from
upstream:

| New | Modified from upstream |
|---|---|
| `FlexNetCode.c`, `flexnet_l3.c`, `flexnet_l3.h` | `Cmd.c`, `L2Code.c`, `asmstrucs.h`, `bpqaxip.c`, `makefile` |

The five modified files are the whole conflict surface when rebasing onto
a new upstream version. Rebase with a three-way merge (old upstream →
new upstream, applied to ours), normalising line endings first — upstream
files mix CRLF and LF. Check that every line of our delta survives.

`tools/upstream-watch/` checks upstream weekly and flags changes to those
five files.

## Building for development

Follow the README installation steps. `sync-and-build.sh` is a helper
that copies this repository to a remote build tree over rsync and runs
`make` there; edit it for your own build host.

- `make flexdebug` enables `FlexNet_Log()` (`/tmp/flexnet_axudp.log`) and
  the per-frame trace. Use it while investigating; build a standard or
  silent binary for anything you release.
- Run `make clean` between flavours (see README).
- `struct FLEXNET_SESSION` is defined twice: the live definition is in
  `asmstrucs.h`; the copy in `FlexNetCode.c` is a fallback compiled only
  without it. Add fields to `asmstrucs.h`.

## Tests

`tools/unit/` holds self-contained C tests. `extract.sh` copies the
functions under test verbatim out of `FlexNetCode.c`, so tests cannot
drift from the shipped code:

```bash
bash tools/unit/extract.sh > tools/unit/extracted.inc
gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -g \
    -fsanitize=address,undefined -I tools/unit \
    -o /tmp/test_compact_pack tools/unit/test_compact_pack.c && /tmp/test_compact_pack
```

Some tests have their own runner script (`run_l2_circuit.sh`,
`run_local_calls.sh`, `run_link_opts.sh`, `run_kiss_links.sh`). They use
AddressSanitizer, which does not start on every aarch64 kernel (a 39-bit
address space); run them on a desktop host if it refuses. Add or update a test for every function you change.

## Code conventions

- C11. Fixed-width `stdint.h` types for anything on the wire, `stdbool.h`
  semantics for flags, no `strcpy` / `strcat` / `sprintf` / `gets`.
- 4-space indent, 100 columns, `snake_case` functions and variables,
  `SCREAMING_SNAKE_CASE` constants, file-local helpers `static`.
- `ConvFromAX25()` writes more than 10 characters: normalised-callsign
  buffers are `char buf[20]`.
- Compare AX.25 callsigns with `flex_l2_same_call()`, never
  `memcmp(..., 7)`: the last address byte carries flag bits that differ
  between a link entry and a received frame.
- The upstream makefile enables no warning flags, so a clean build proves
  nothing. Check a changed file on its own:
  `gcc -DLINBPQ -MMD -g -fcommon -Wall -Wextra -Wshadow -c -o /tmp/w.o <file>`
  and leave its warning count no higher than you found it.
- Comments explain *why* — an invariant, a wire constraint, an observed
  peer behaviour — not *what*.

## Wire-protocol discipline

- **Capture first, then code.** Derive every wire format from a capture
  of real peers, and verify with a new capture after the change. A wrong
  byte produces silence, not an error.
- **Cite every wire constant** — the protocol spec section, or a captured
  frame.
- **Test both directions.** For request/reply frames, test the node as
  the originator *and* as the responder.
- For forwarding questions, capture on both sides of the forwarding node;
  a correlator seen in both captures is the only proof that a frame
  propagated.
- Describe peers in observation terms in anything public: "captures
  show…", "observed on the live network…". Do not describe other
  implementations' internals.
- Update [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md) when you learn something
  about the wire.

## Releases

1. Bump `FLEXNET_VERSION_STR` in `FlexNetCode.c` — on every release,
   including upstream-only rebases. Bump `FLEXNET_VERSION_PROTO` (the
   L3RTT identity peers see) only if wire-visible identity changes.
2. Update the version in the README title and `V` example, add an entry
   to `RELEASE_NOTES.md`, and update `ROADMAP.md`.
3. Run the unit tests; build the silent flavour and check it
   (`strings linbpq | grep -c 'FlexNet: '` — 5 as of v2.5.0: the
   operator warnings, nothing informational).
4. Run the release on a live node before tagging.
5. Annotated tag `vX.Y.Z`, push, and publish a GitHub release from the
   release-notes entry.
