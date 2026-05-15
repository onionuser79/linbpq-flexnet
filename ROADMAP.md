# linbpq-flexnet — Roadmap

## Current production: v1.10.0 (2026-05-15)

linbpq-flexnet is a **leaf node** participating in a FlexNet mesh
alongside its existing NET/ROM stack. v1.10.0 ships **GA item #2 —
SSID-range internal application binding**: the operator can declare
`FLEXNETSSIDRANGE N-M` in `bpq32.cfg`, the node then advertises the
range on the FlexNet cloud, and incoming connects to MYCALL-N
(N in the range) are dispatched by BPQ's existing `APPLICATION`
mechanism to whichever app is bound to that SSID (BBS, CHAT, etc.).
v1.9.9's `case 0xcf` fix and v1.9.8's `CE_FRAME_STATUS_1N` classifier
remain.

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

Both GA items are now shipped — v1.9.8 closed item #1 and v1.10.0
closed item #2. The repo is feature-ready for the v2.0 tag.

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

### 2. SSID-range internal application binding — _shipped in v1.10.0_

Shipped on 2026-05-15 (v1.10.0). Operators declare an SSID range
in `bpq32.cfg` with one new directive:

```
FLEXNETSSIDRANGE 0-8
```

What v1.10.0 does:

1. A new `FLEXNETSSIDRANGE N-M` directive is parsed at FlexNet
   init time (lazy first-call from `FlexNet_InitSession`, since
   stock LinBPQ doesn't invoke FlexNet's own init hook).
2. The FlexNet CE INIT handshake declares `max_ssid = M` to peers
   (instead of the node's own SSID). Without this, xnet clamps
   incoming route adverts to the originator's declared max_ssid,
   which is why the range used to collapse to `(0-0)`.
3. The compact route record sent to peers encodes `ssid_lo = N,
   ssid_hi = M` so the cloud sees a single line, e.g.
   `IR2UFV  0-8  1`, instead of N separate per-SSID entries.
4. Inbound connects to MYCALL-N (N in the range) are dispatched
   by BPQ's existing `APPLICATION` mechanism — bound SSIDs (e.g.
   `APPLICATION 1,BBS,,IR2UFV-8,...`) reach their app, the node
   SSID reaches the command parser, and unbound intermediate
   SSIDs refuse cleanly. No new dispatch code was needed.

Verified live on iw2ohx-gw running a second IR2UFV instance
configured with `FLEXNETSSIDRANGE 0-8` and `APPLICATION 1,BBS,,
IR2UFV-8,UFVBBS,255`:

- `C IR2UFV-8` from xnet IW2OHX-4 (direct neighbour) → BBS.
- `C IR2UFV-8` from xnet IW2OHX-14 (direct neighbour) → BBS.
- `C IR2UFV-8` from production IW2OHX-13 (FlexNet path via -4) → BBS.
- `C IR2UFV-8` from IR2UFV's own BPQ console (local loopback) → BBS.

xnet's `D IR` shows `IR2UFV  0-8  cost=1` — the range encoding
works on the wire.

Future expansion: add another `APPLICATION 2,CHAT,...,IR2UFV-7,...`
line in `bpq32.cfg` and the cloud immediately reaches that app via
`C IR2UFV-7` (the SSID is already in the advertised range).

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

_Document version: 2026-05-15 — v1.10.0 in production as
leaf-with-multiport; v2.0 GA items #1 (CE-UNKNOWN, v1.9.8) and
#2 (SSID-range, v1.10.0) both shipped; case 0xcf double-memmove
fix in v1.9.9. The repo is feature-ready for the v2.0 tag._
