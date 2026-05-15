# linbpq-flexnet — Roadmap

## Current production: v1.9.9 (2026-05-15)

linbpq-flexnet is a **leaf node** participating in a FlexNet mesh
alongside its existing NET/ROM stack. v1.9.9 is the production tip
on `main`, deployed on iw2ohx-gw. It fixes a long-standing
double-memmove bug in the v1.9.5 `case 0xcf` fall-through path
that broke `C IW2OHX-4` / `C IW2OHX-14` (and any other NetROM L4
connect terminating at a FlexNet neighbour) from the BPQ console.
v1.9.8's `CE_FRAME_STATUS_1N` classifier remains.

What works today, from the v1.x line that shipped:

- Node identity preservation in outbound digi chain (v1.2).
- All six P1 protocol-correctness items: L3RTT counters, link-down
  guard, L3 INFO envelope on replies, IIR-smoothed link time,
  dtable RTT=0 skip, KA cadence (v1.3.x).
- M5 path discovery — CE type-6/7 PATH_REQ/PATH_REP with on-disk
  cache (v1.9.0 / v1.9.1).
- Multi-FlexNet-neighbour with cost-based routing (v1.9.2).
- AXIP byte-6 SSID normalisation + session-table hygiene (v1.9.3).
- `C <flexnet-neighbour>` fixes — no-digi when target ==
  neighbour + `case 0xcf` fall-through to NetROM L4 (v1.9.5).
- 3-column D output, `CE-UNKNOWN` log entry (2026-05-14 cosmetic
  commit).
- `CE_FRAME_STATUS_1N` classifier for the `"1n\r"` status family,
  cleaning up the `CE-UNKNOWN` log spam without changing on-wire
  behaviour (v1.9.8).
- L2Code.c `case 0xcf` no longer falls through to `flexnet_default`
  after `FlexNet_ProcessCF` returns 0 — the second memmove was
  reading from a now-corrupted source position and overwriting
  the PID byte with the L3 TTL. `C IW2OHX-4` and `C IW2OHX-14`
  from the BPQ console now print "Connected to" and the banner,
  closing the last visible asymmetry between FlexNet-link L2
  digi-chain connects (v1.9.5 path) and L4 NetROM connects (v1.9.9).

What was tried and reverted:

- v1.9.4 — transit-role D-table re-advertisement. Reverted in
  v1.9.7 because the L2 digipeat path it implied broke AX.25 V2
  reciprocity on the return frame, and the simpler chain-preserving
  variant could not be validated end-to-end. linbpq is back to a
  pure leaf with no transit forwarding.

For the full release timeline, test numbers, and investigation
narrative, see the `project_linbpq_v1_9_release.md` and
`project_linbpq_v1_9_5_test_results.md` memory files.

---

## v2.0 GA — outstanding items

Only two features remain on the GA list. Everything else from the
earlier roadmap has been resolved, deferred indefinitely, or
removed as out of scope for a leaf node.

### 1. `CE-UNKNOWN` investigation + parser entry — _shipped in v1.9.8_

Shipped on 2026-05-14 (v1.9.8). The previously-unclassified 3-byte
`"12\r"` frame is now part of a recognised `"1n\r"` (n=1..9) status
family, handled by the new `CE_FRAME_STATUS_1N` classifier as a
benign status notification.

What v1.9.8 did:

1. Added `CE_FRAME_STATUS_1N` enum + parser match in
   `flex_parse_ce_frame` for the 3-byte shape `'1' [1-9] '\r'`.
   `"10\r"` keeps its existing dedicated `CE_FRAME_STATUS_10` entry.
2. New `case CE_FRAME_STATUS_1N` in the `FlexNet_ProcessCE` switch
   logs `CE-STATUS-1n: from=<peer> digit=<n>` (under debug builds,
   via `FlexNet_Log`) and returns without further action — the
   wire-level behaviour is unchanged from the previous default
   branch.
3. The default `CE-UNKNOWN` branch is kept in place for any
   genuinely new frame shape future peers may emit.

Phase 1 inventory was satisfied by the prior multi-day debug
capture (see project memory): only `"12\r"` was observed on the
wire; the generic classifier covers the entire `1n` family without
needing per-digit handlers (Option A — see project memory). If a
new digit ever appears in a future debug-build capture, it surfaces
as a `CE-STATUS-1n` line with the digit identified, and the
operator can decide whether per-digit semantics need encoding.

Acceptance: met. `"12\r"` is no longer flagged as `CE-UNKNOWN`; it
is classified, named, and intentionally treated as benign.

### 2. SSID-range internal application binding

Currently the FlexNet identity is the single `NODECALL` from
`bpq32.cfg` (e.g. `IW2OHX-13`). Only that specific SSID is
advertised — no range.

A real FlexNet node typically binds multiple SSIDs on its callsign
to different internal applications: BBS, chat, mail gateway,
DXspider, Winlink, etc. Today, BPQ users dial those through the BPQ
command parser (`BBS`, `CHAT`, etc.). FlexNet users coming from the
cloud should be able to reach each application directly by
addressing the right SSID at our node call.

The GA work:

1. Configurable mapping in `bpq32.cfg` between SSIDs and BPQ
   applications. Example shape:
   ```
   FLEXNET_APPL
       SSID=3   APPL=BBS
       SSID=7   APPL=CHAT
       SSID=11  APPL=MAILGW
   END
   ```
2. Advertise the configured SSIDs to FlexNet neighbours as part of
   the routes we send for `MYCALL` (so the cloud sees that
   `IW2OHX-3`, `IW2OHX-7`, `IW2OHX-11` are reachable through us).
3. On incoming connect, dispatch the SABM to the bound application
   based on the destination SSID, not just the bare BPQ command
   parser.
4. Document the new config block in `README.md` once shipped.

This unlocks "FlexNet user can hit our BBS directly by connecting
to `IW2OHX-3`" without having to go through the BPQ node prompt
first.

---

## Out of scope for v2.0 GA

The following items were considered earlier in the v1.9.x cycle and
are deliberately **not** on the GA path:

- **Transit-role re-advertisement (the reverted v1.9.4 mechanism).**
  Would need a re-design that preserves AX.25 V2 reciprocity (e.g.
  NetROM L3 forwarding rather than L2 digipeat). linbpq-flexnet is
  staying a leaf node — operators who need a transit router run
  one of the three real FlexNet routers: **(X)Net**, **PC/Flexnet**,
  or **RMNC/Flexnet**.
- **Route withdrawal on `via_session_idx` failover.** Was paired
  with transit advertising; without that, nothing to withdraw.
- **Periodic RTT=0 TX refresh marker.** Also tied to advertising;
  not needed as a leaf.
- **P2 #9 capacity resize (64 → 256 destinations).** The current
  64-slot table has been sufficient under live load. Listed as a
  quick-win instead.
- **Multi-day soak as a gating item.** Soak runs naturally
  in production usage; not a formal GA blocker.

### Code portability into `flexnetd`

The original plan was to factor the shared protocol surface
(CE type-6/7 build/parse, QSO allocator, probe table, L3RTT
counters, IIR filter) into a `flexnet_l3_proto.c` consumed by both
`linbpq-flexnet` and `flexnetd` (the Linux-daemon sibling project —
itself not a real FlexNet router; the three real routers are
(X)Net, PC/Flexnet, RMNC/Flexnet). This is set aside — not part of
v2.0 GA. If it ever happens it would be a sibling effort across
both repos, not a deliverable here.

---

_Document version: 2026-05-15 — v1.9.9 in production as
leaf-with-multiport; v2.0 GA item #1 (CE-UNKNOWN) shipped in v1.9.8;
case 0xcf double-memmove fix shipped in v1.9.9; only remaining GA
item is SSID-range internal application binding._
