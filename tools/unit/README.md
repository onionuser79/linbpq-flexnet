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

## test_pcf_quiesce.c

Pins `flex_records_allowed()`, the gate added in v2.2.2 that stops us
pushing compact records at a PC/Flexnet peer once it has closed its `3+`
exchange. See `research/link_stability_2026-09-22/`.

It needs globals the other tests do not declare, so it extracts its own
function into its own include:

```sh
bash tools/unit/extract.sh FlexNetCode.c flex_records_allowed \
     > tools/unit/extracted_quiesce.inc
gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -g \
    -fsanitize=address,undefined -I tools/unit \
    -o /tmp/test_pcf_quiesce tools/unit/test_pcf_quiesce.c
/tmp/test_pcf_quiesce
```

`test_zeroed_session_may_advertise` is the load-bearing one: a reconnect
clears the flag only because `FlexNet_InitSession` memsets the session,
so the zeroed state *must* mean "may advertise". Inverting the sense of
the flag would otherwise silence every peer after the first reconnect.

## test_l2_circuit.c

Pins the L2 transit circuit table behind `FlexNet_L2Transit()` — the
v2.2.4 hardening. The defect it guards: up to v2.2.3 every forward frame
re-resolved the next hop and overwrote the circuit's `appended` hop, so
after a route change mid-circuit the frames still returning over the old
hop were not contracted and reached the originator with a digi it never
sent. Covers the control-byte classifier (an I-frame shaped like a DISC
must not read as one), the LINGER / EVICT / IDLE slot horizons, the
re-pin policy, the teardown lifecycle, eviction order (closed first, then
the longest-silent, **never** a live circuit), the loop check, and the
route-change scenario end to end on real frame bytes.

It needs a `#define`, a `struct` and a dozen helpers, so it uses the
`define:` / `struct:` items `extract.sh` gained for it, and has a runner:

```sh
bash tools/unit/run_l2_circuit.sh     # extract + build (-Werror, ASan/UBSan) + run
```

The helpers index the frame through a byte pointer from `DEST` onward,
as BPQ does; a test that writes `m->DEST[14]` trips UBSan's bounds check
even though the layout is intended.

## test_local_calls.c

Pins v2.3's local `APPLICATION` calls (`FLEXNETLOCAL`,
`FLEXNETLOCALAPPS`, issue #1) at the four places the feature can
half-work:

- **config** — parsing, dedupe (`SR4BBX-0` is `SR4BBX`), the 16-entry cap,
  and `FLEXNETLOCALAPPS` *not* being consumed by the `FLEXNETLOCAL` parser,
  whose keyword is its prefix;
- **black holes** — an entry no `APPLICATION` answers, or one on
  NODECALL's base, is never advertised, answered for, or delivered;
- **the wire** — node record + every local call in ONE compact frame, the
  worst case (16 six-character calls) fitting `FLEXNET_ADVERT_FRAME_BYTES`,
  and with no locals the frame byte-identical to v2.2.4's;
- **the answers and L2** — `flex_target_is_us()` knows the local list, and
  `FlexNet_MarkLocalDigi()` sets the H-bit only on our own node call when
  we send as a local call.

It stubs `ConvFromAX25`, `CompareCalls` and `MYCALL` and extracts the
public `FlexNet_*` hooks too (`extract.sh` handles non-static functions
since v2.3). Has a runner:

```sh
bash tools/unit/run_local_calls.sh    # extract + build (-Werror, ASan/UBSan) + run
```

On the Raspberry Pi ASan cannot start (its shadow-memory layout does not
fit the kernel's address space — `CHECK failed:
sanitizer_allocator_primary64.h`); build there with `-fsanitize=undefined`
only.

## test_link_opts.c

Pins v2.4's per-link routing options (the `F` suffix of an AXUDP `MAP`
entry):

- **parsing** — every option, combinations (`F+)`, `F-!` = `F>`), and an
  unknown character failing with the result zeroed, so the link comes up
  with default policy;
- **the source filter** — `-`, `!`, `>` and `=` remove a link as a source
  in `flex_expected_rtt()`; a destination also reachable over an
  unrestricted link is still offered, at that link's cost, rather than
  withdrawn;
- **the neighbour test** — "the neighbour itself" matches the peer's own
  range record (`NODEB 0-15`), not only the entry session start flagged;
- **`+`** — added once, never to the withdrawal sentinel or the RTT=0
  marker, and reported to the caller;
- **the climb guard** — a failover onto a penalised link must not be
  latched as a count-to-infinity loop (a control series shows that on raw
  costs it would be).

```sh
bash tools/unit/run_link_opts.sh      # extract + build (-Werror, ASan/UBSan) + run
```

## test_kiss_links.c

Pins v2.5's FlexNet over KISS ports:

- **the PORT block parser** — `FLEXNET=` and `FLEXNETLINK=` only inside a
  block, matched exactly (`FLEXNET` must not swallow `FLEXNETLINK` or the
  global `FLEXNETTRANSIT`), numbered as `config.c` numbers ports (a later
  `PORTNUM=` wins), never read from a driver's `CONFIG` section; bad
  values, callsigns and options warn; the table is capped;
- **resolution** — usable only on an existing ASYNC or I2C port that has
  `FLEXNET=YES`; first attempt delayed with jitter;
- **the lookup** — matched like the AXIP `MAP` lookup: C/H and
  end-of-address bits ignored, SSID significant, port significant;
- **the link keeper** (with stubbed `FindLink` / `SENDSABM`) — opens when
  due, once per pass, as a circuit-less downlink from the node call;
  back-off 60 → 900 s while unanswered; starts the CE session on UA but
  not on a digipeated link; reopens 10 s after a live link drops; waits
  when no LINK slot is free;
- **next-hop cost** — reported cost plus our link time, infinity kept;
- **the KA echo gate** — one echo per 60 s per session towards
  (X)Net-like peers, always towards PC/Flexnet, and two echoing nodes
  stop after one echo each way.

`test_link_opts.c` also covers v2.5's cross-port rule in
`flex_expected_rtt()`.

```sh
bash tools/unit/run_kiss_links.sh     # extract + build (-Werror, ASan/UBSan) + run
```
