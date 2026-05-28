# linbpq-flexnet — Roadmap

## Current state: v2.1.14 in production

linbpq-flexnet is a **leaf node** participating in a FlexNet mesh
alongside its existing NET/ROM stack. v2.0.0 was the first GA tag;
the v2.1.x line adds **PC/Flexnet compatibility**, verified end-to-end
against IW2OHX-12 (PC/Flexnet V4.0).

**Production node IW2OHX-13 and test bed IR2UFV both run v2.1.14.**
The PC/Flexnet compatibility stack is now complete across two
distinct symptom classes:

- **Link-cost saturation at 4095** (v2.1.13) — rate-limited
  outbound type-1 link-time replies so they land inside
  PC/Flexnet's expected-reply window, avoiding the negative-delta
  wrap that pinned every sample at the 12-bit RTT cap.
- **~90-min spurious session reconnects** (v2.1.14) — added
  hysteresis to the session reaper so a single transient
  `L2STATE != 5` blip during routine AX.25 state transitions no
  longer destroys a live FlexNet session slot.

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
- v2.1.7 — proactive CE-init scan no longer auto-classifies user
  pass-through sessions as peer-to-peer FlexNet sessions. The scan
  matched on `LINKCALL` only, which also caught LINKs created when a
  telnet user issued `C <flexnet-peer>` (the user's session terminates
  at the peer's call too). The result was that linbpq pushed
  `pid=CE INIT` + `KA` frames into the user's L2 session, and
  PC/Flexnet stopped delivering reply data while still L2-ACKing.
  Two additional filters in the scan — `OURCALL` base must equal node
  `MYCALL` base, and `DIGIS[0]` must be zero — restrict auto-init to
  the direct AXIP peer tunnel.
- v2.1.8 — direct-neighbour `C <call>` now emits a **single-digi**
  chain `MYCALL*` (H-bit set) rather than the v1.9.5 zero-digi
  arrangement. Without any digi, the SABM arrived at the FlexNet
  peer as a bare user callsign that PC/Flexnet didn't recognise and
  DM'd:
  ```
  <R IW7EAS>IW2OHX-12 SABM+>
  <T IW2OHX-12>IW7EAS DM->
  ```
  With a single MYCALL digi the SABM becomes
  `<user> -> <peer> via <us>*` and PC/Flexnet accepts it. The reverse
  UA carries `MYCALL` as a pending digi; the existing L2-RX-DIGI
  handler matches the active LINK, marks the H-bit and delivers
  locally, so no L2 changes are needed.
- PC/Flexnet compatibility (v2.1.0 + v2.1.6 + v2.1.8). Three changes
  were needed in total, identified by direct comparison against live
  xnet-14 ↔ IW2OHX-12 wire captures used as the gold-standard
  reference:
  1. v2.1.0 — when a peer in the AXIP MAP table with the `F` flag
     (FlexNet-only, no NetROM) initiates the L2 SABM, the new hook
     in `L2Code.c` detects the F-flagged peer in the inbound
     SABM-accept path, marks the LINK as FlexNet, suppresses BPQ's
     default CTEXT banner (which PC/Flexnet rejects as non-protocol
     traffic), and drives `FlexNet_InitSession`. Without this the
     session DISC'd within seconds. The keepalive parser was also
     relaxed to accept the 201-byte PC/Flexnet shape (`'2'` + 200
     spaces) in addition to xnet's 241-byte form.
  2. v2.1.6 — `flex_send_own_routes` previously prefixed each
     outbound route advertisement with `"3+\r"`. Per spec §2.6,
     `"3+\r"` is a REQUEST sent to the peer ("please send me your
     routes"), not a marker that the sender is about to advertise.
     PC/Flexnet treated the spurious prefix as a malformed exchange
     and DISC'd the L2 session every few seconds. Removing the
     leading `"3+\r"` while keeping the trailing `"3-\r"` (the
     legitimate end-of-our-batch marker, per spec) restored the
     session and PC/Flexnet immediately began pushing its full
     compact route table — verified against IW2OHX-12, 19-entry
     compact batches arriving every ~5 sec, FL shows status
     `CONNECTED` with the peer's KAs counted.

- v2.1.9 — operator-UX additions to the `D` (destinations) command:
  `D /COST` / `D /CALL` / `D /AGE` sort modifiers, `D < <neighbour>`
  via-neighbour filter, and `D !` / `D ?` cached-path filters. No
  protocol or wire changes; pure local presentation.

- v2.1.10 — per-session keepalive shape mirror. The CE keepalive
  builder records the length and trailing byte of each accepted peer
  KA on the session struct and echoes a matching-shape frame on the
  next send. PC/Flexnet emits a `'2' + 200 spaces + CR` shape and was
  observed to silently discard our (X)Net-shape echo before this
  change. The mirror was later simplified in v2.1.13 once the actual
  saturation root-cause was identified — see below.

- v2.1.11 — three coupled changes that surfaced after IR2UFV was
  added as a second linbpq-flexnet test instance on the same gateway:
  1. Route emission no longer gates on the **first received peer KA**.
     PC/Flexnet does not reliably emit its own KAs after the initial
     SABM/UA on AXUDP-mapped peer sessions, so the gate at
     `case CE_FRAME_KEEPALIVE` never fired and our destination table
     never reached the peer's link table. Route emission now triggers
     on `case CE_FRAME_INIT` instead — INIT receipt is sufficient
     evidence that the FlexNet layer is up.
  2. Peer flavour is inferred from observed INIT length (PC/Flexnet
     V4 sends a 6-byte init, (X)Net sends 5 bytes); the keepalive
     shape and records-per-emit cap follow.
  3. Records-per-emit cap drops from 8 to 2 for PC/Flexnet peers (and
     1 for unknown flavour) — wire evidence showed PC/Flexnet
     issuing DISC the moment it received the 5th back-to-back
     I-frame in a 1-ms burst on its inbound queue.

- v2.1.12 — short-lived attempt that reactively answered PC/Flexnet
  keepalives with a CE_FRAME_STATUS_10 (`"10\r"`) pong instead of a
  KA echo, based on misreading the IW2OHX-14 ↔ IW2OHX-12 wire
  capture. The pong reply is what (X)Net IW2OHX-14 emits in response
  to PC/Flexnet's active probe; copying it from a peer that lives on
  a different code path didn't reproduce the live-peer measurement
  cycle. **Reverted by v2.1.13.** Documented here for posterity.

- v2.1.13 — **closes the PC/Flexnet link-cost saturation root cause.**
  Rate-limits outbound CE type-1 (link-time) replies based on peer
  flavour:

  - PC/Flexnet peers (KA terminator = CR): ≥ 320 s between sends.
  - (X)Net peers (KA terminator = space): ≥ 20 s between sends.
  - First LT during the session-start handshake passes unrestricted.

  PC/Flexnet computes an internal expected-reply timestamp after
  each CE link-time frame. With our advertised smoothed link-time
  value of `2` (the spec-recommended advertise, see `FLEXNET_WIRE_LT`),
  the expected next-reply lands roughly 19 seconds out; with a fully
  smoothed link the cap is 320 seconds. Replies arriving **before**
  that window underflow PC/Flexnet's RTT delta arithmetic and clamp
  the sample to its 12-bit saturation cap (= 4095), which is the
  value we'd observed pinning the IR2UFV link in PC/Flexnet's `L *`
  table across v2.1.0–v2.1.12. The rate limit moves every outbound
  LT into the valid window. The sibling flexnetd project uses the
  same strategy on its PC/Flexnet ports.

  Other v2.1.13 changes: dropped v2.1.10's per-session KA shape
  mirror in favour of the universal `'2' + 240 spaces` (no trailer)
  shape both (X)Net and PC/Flexnet accept; reverted v2.1.12's "PCF
  → `10\r` only" branch in `case CE_FRAME_KEEPALIVE` so we again
  echo the KA + send LT for **all** peer flavours, matching
  flexnetd's reference behaviour.

  Verified on the IR2UFV ↔ IW2OHX-12 link, 2026-05-27: cost in
  PC/Flexnet's `L *` table converged
  `4095/2 → 1566/2 → 941/2 → 315/2 → 258/2 → 2/2` over ≈ 85 minutes,
  with all 16 ring samples settling at `2 3` (i.e. 20–30 ms RTT,
  matching the (X)Net peer baseline). xnet IW2OHX-14's view of
  IR2UFV stayed at `F 2 2/2` throughout — no regression.

- v2.1.14 — **closes the ~90-min spurious session-reset cycle**
  that v2.1.13 still exhibited on the IR2UFV ↔ IW2OHX-12 link.

  Extensive monitoring on 2026-05-28 (3-hour wire capture + console
  trace) showed PC/Flexnet's `L *` entry for IR2UFV would rebuild
  from scratch (`600 …` seed pattern, cost climbing back to mid-
  thousands) approximately every 90 minutes — without a single
  AX.25 U-frame (SABM/DISC/UA/DM/FRMR) crossing the wire. The L2
  link was continuously up; only our internal FlexNet session
  slot was being cleared.

  Root cause: the session-reaper loop in `FlexNet_Timer`
  (`FlexNetCode.c:1696`) treated any single observation of
  `LINK->L2STATE != 5` as proof the link was gone, dropped the
  session slot in place, and let the next inbound CE frame auto-
  recreate it via `FlexNet_ProcessCE`. BPQ briefly takes
  `L2STATE` away from 5 during routine AX.25 internal state
  transitions (mod-128 negotiation, N(S) wrap, retry timing
  windows), and a single transient triggered the reap.

  The fresh session-start emitted INIT + KA to the peer, which
  PC/Flexnet reads as "new peer" and reseeds its link-cost ring
  with the `600 4095` outliers — driving the cost back up until
  the rate-limited LT cycle converged it again over the next
  hour. Cosmetically the link kept working, but PC/Flexnet's
  routing decisions and outbound cost advertisements about
  IR2UFV were inflated for half of every cycle.

  Fix: added a `reap_strikes` counter to `FLEXNET_SESSION`. The
  bad-state condition (`L2STATE != 5` OR `LINKCALL[0] == 0`)
  must now persist for `FLEXNET_REAP_STRIKES = 3` consecutive
  `FlexNet_Timer` ticks before the slot is destroyed. Any tick
  that observes a recovered state (L2STATE back to 5) resets the
  counter to zero. `LINK == NULL` still reaps immediately —
  that's not a transient.

  Verified on IR2UFV ↔ IW2OHX-12: zero `session reconnected`
  events and zero `reaping` messages in the 80+ min validation
  window after deploy, against the previous ≈ 90 min cadence
  observed pre-v2.1.14. The PC/Flexnet `L *` entry kept the
  same age counter the entire time, with the ring filling
  cleanly from `600 4095 2 2 …` through the standard convergence.

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

_Document version: 2026-05-28 — v2.1.14 in production (both IW2OHX-13
and IR2UFV). The PC/Flexnet compatibility stack across the v2.1.x
line:_

- _v2.1.0 — CTEXT suppression on F-flagged inbound SABM + 201-byte
  KA shape accepted on receive._
- _v2.1.6 — removed the spurious leading `"3+\r"` from outbound
  route batches._
- _v2.1.7 — proactive-init scan filtered to peer-to-peer FlexNet
  sessions only._
- _v2.1.8 — single-digi MYCALL on direct-neighbour `C <call>`._
- _v2.1.11 — route emission moved to CE_FRAME_INIT trigger;
  per-flavour records-per-emit cap._
- _v2.1.13 — outbound CE link-time replies rate-limited per peer
  flavour (≥ 320 s for PC/Flexnet, ≥ 20 s for (X)Net) to land in
  PC/Flexnet's expected-reply window. Closes the link-cost
  saturation-at-4095 symptom that survived v2.1.10–v2.1.12._
- _v2.1.14 — session-reaper hysteresis. A single transient
  `L2STATE != 5` observation no longer destroys a live FlexNet
  session slot; bad state must persist for 3 consecutive
  `FlexNet_Timer` ticks. Closes the ~90-min spurious session-reset
  cycle observed on IR2UFV ↔ IW2OHX-12 in v2.1.13._
