# linbpq-flexnet — Roadmap

## Current state: v2.1.27 in production

linbpq-flexnet is a **leaf node** participating in a FlexNet mesh
alongside its existing NET/ROM stack. v2.0.0 was the first GA tag;
the v2.1.x line adds **PC/Flexnet compatibility**, verified end-to-end
against IW2OHX-12 (PC/Flexnet V4.0).

**Production node IW2OHX-13 and test bed IR2UFV both run v2.1.27.**
The PC/Flexnet compatibility stack is now complete across five
distinct symptom classes:

- **Link-cost saturation at 4095** (v2.1.13) — rate-limited
  outbound type-1 link-time replies so they land inside
  PC/Flexnet's expected-reply window, avoiding the negative-delta
  wrap that pinned every sample at the 12-bit RTT cap.
- **~90-min spurious session reconnects from L2STATE blips**
  (v2.1.14) — added hysteresis to the session reaper so a single
  transient `L2STATE != 5` blip during routine AX.25 state
  transitions no longer destroys a live FlexNet session slot.
- **Periodic session re-handshakes from spurious `FlexNetLink`
  clears** (v2.1.15) — the proactive CE-init scan was re-handshaking
  to peers whenever some BPQ-internal L2 maintenance path cleared
  `LINK->FlexNetLink` without actually closing the L2 link. That
  re-INIT made PC/Flexnet reseed its link-cost ring with the
  `600 …` outliers, re-introducing the cost spike v2.1.13 had
  already solved. Now: if our session for the LINK is already
  established (`got_peer_init == TRUE`), we just re-promote
  `LINK->FlexNetLink` without disturbing the peer.
- **Periodic session-reaps from BPQ recycling the LINKTABLE slot**
  (v2.1.16) — even after v2.1.15, IR2UFV still saw a fresh
  `session started` event every ~90 min on the IW2OHX-12 link
  (new-slot path, not "session reconnected"). Root cause: BPQ's
  L2-idle handling occasionally runs `CLEAROUTLINK` on a LINKTABLE
  slot we still reference, zeroing the entire struct in place. Our
  stale `sess->LINK` now points at memset bytes (LINKCALL[0]==0,
  L2STATE==0), which is *persistent* bad state — the v2.1.14
  reap-hysteresis 3-strike counter trips on it, the slot is reaped,
  and the next inbound CE frame on the BPQ-allocated *new* LINK
  triggers a fresh INIT+KA handshake. Now: the reaper first looks
  for a new LINK matching the stashed `peer_callsign` on the same
  port, and if found migrates `sess->LINK` to it without
  re-INITing. `peer_callsign` is captured in every InitSession
  path so the migration survives slot recycling.
- **Residual reseed when the migration scan loses the BPQ race**
  (v2.1.17) — the v2.1.16 migration only catches recycles whose
  *new* LINK has already reached `L2STATE == 5` at the moment the
  reaper ticks. When the new LINK is still mid-SABM (or hasn't
  yet been allocated) the migration finds nothing, the session
  gets reaped, and the next inbound CE frame triggers the
  new-slot path — which used to unconditionally emit a fresh
  INIT. Solution: persistent per-peer (callsign, port) INIT
  cooldown. We send INIT to a peer at most once per
  `FLEXNET_INIT_TX_INTERVAL` (1 hour); the cooldown survives
  session destruction. PC/Flexnet only reseeds its link-cost
  ring on received INIT, so no INIT = no reseed. When the peer
  sends *us* a fresh INIT, we clear our cooldown so the next
  refresh reciprocates.

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

- v2.1.15 — **closes a residual session-reset path that v2.1.14 did
  not cover.** After v2.1.14 deploy, IR2UFV ↔ IW2OHX-12 still saw
  one `session reconnected (same LINK, re-sent init + keepalive)`
  event per ≈ 80 min — wire still showed no AX.25 U-frames. Same
  symptom on PC/Flexnet's side (cost spike back to mid-hundreds,
  ring reseeded with `600 …`), but the trigger was not the reaper.

  Root cause: the proactive CE-init scan in `FlexNet_Timer`
  (`FlexNetCode.c:1755`) iterates connected L2 links and calls
  `FlexNet_InitSession` whenever it finds `L2STATE == 5 &&
  !LINK->FlexNetLink`. Several BPQ-internal L2 maintenance paths
  (e.g. `CLEAROUTLINK` followed by silent slot reuse, internal
  state-machine resets) clear `LINK->FlexNetLink` to FALSE without
  actually closing the L2 link or sending DISC. The proactive
  scan reads that as "this link has no FlexNet session yet" and
  re-runs the INIT handshake. `FlexNet_InitSession`'s same-LINK-
  match branch then resets `sent_routes`, `got_peer_init`,
  `keepalive_count`, and `session_start`, and re-sends INIT + KA
  to the peer — which PC/Flexnet reads as "new peer" and reseeds
  its link-cost ring with the familiar `600 …` outlier pattern.

  Fix: split the same-LINK-match branch into "already established"
  vs "still in handshake". An established session (one where we
  have already received the peer's INIT — `got_peer_init == TRUE`)
  is now treated as authoritative: we re-promote
  `LINK->FlexNetLink = TRUE` and return silently, without
  re-sending INIT/KA or resetting session state. The original
  re-handshake flow is preserved for the mid-handshake case
  (`got_peer_init == FALSE`).

  This pairs with the v2.1.14 reaper hysteresis to provide
  defence-in-depth: even if some BPQ-internal path clears
  `FlexNetLink` mid-life, the peer's link-cost ring is not
  disturbed.

- v2.1.16 — **closes the residual `session started` (new-slot)
  cycle that survived v2.1.14 + v2.1.15.** First observation of
  v2.1.15 on IR2UFV showed PC/Flexnet's IR2UFV entry being
  recreated again ~21 min into the run; console showed
  `FlexNet: session started on port 2 with IW2OHX-12 (sent init
  max_ssid=8 + keepalive)` rather than `session reconnected`. The
  v2.1.14 reaper had fired and the next inbound CE frame on the
  *fresh* LINKTABLE slot ran the new-slot branch — which always
  sends INIT/KA, so PC/Flexnet still reseeded its ring.

  Root cause is BPQ-internal: `CLEAROUTLINK` (L2Code.c:4117) is
  called from several L2 maintenance paths (idle-timer N2-retry
  exhaustion, FRMR, DISC retry, …) and `memset`s the entire
  LINKTABLE struct in place. The slot can then be re-allocated by
  BPQ to the same peer when the next AXIP frame arrives. Our
  `sess->LINK` pointer is unchanged but now points at zeros
  (`LINKCALL[0]==0`, `L2STATE==0`) — *persistent* bad state, so
  v2.1.14's 3-strike hysteresis trips fast. The slot is reaped
  and the BPQ-allocated NEW LINKTABLE entry then hits the
  new-slot branch on the next CE frame.

  Fix: added `peer_callsign[7]` to `FLEXNET_SESSION` (set in
  every `FlexNet_InitSession` branch). Before the reaper destroys
  a bad-state session it scans `LINKS[0..MAXLINKS]` for an
  L2STATE==5 entry whose port and 7-byte LINKCALL match the
  stashed `peer_callsign`. If found, the session migrates to the
  new LINK pointer: `sess->LINK = new_LINK`,
  `new_LINK->FlexNetLink = TRUE`, `reap_strikes = 0`. The session
  stays `got_peer_init == TRUE` and `sent_routes == TRUE` — no
  INIT/KA is sent to the peer, so the peer's link-cost ring is
  untouched.

- v2.1.17 — **closes the residual reseed that survived v2.1.16
  when the migration scan loses the BPQ race.** v2.1.16 deploy
  showed PC/Flexnet's IR2UFV entry was still rebuilt ~92 min in,
  with the familiar `600 600 4095` seed pattern; console said
  `FlexNet: session started on port 2 with IW2OHX-12 (sent init
  max_ssid=8 + keepalive)`. The v2.1.16 reaper-time migration
  requires the *new* LINK to already be at `L2STATE == 5` at the
  exact reaper tick — when BPQ's L2-link maintenance and our
  reaper tick race the wrong way, the new LINK is still
  initialising and the migration scan finds nothing. The session
  is reaped, the next inbound CE frame from the now-L2STATE-5
  new LINK hits `ProcessCE` → `flex_find_session` returns NULL →
  `FlexNet_InitSession` new-slot path, which used to
  unconditionally fire INIT + KA.

  Fix: persistent per-peer INIT cooldown. A new static table
  `g_init_history[FLEXNET_INIT_HISTORY_SIZE]` (16 entries,
  ample for any realistic peer set) records the last outbound
  INIT timestamp per (callsign, port). The three
  `FlexNet_InitSession` branches now consult
  `flex_init_recently_sent()` before emitting INIT — if we've
  INIT'd this peer within `FLEXNET_INIT_TX_INTERVAL` (3600 s =
  1 hour) the INIT (and the kick-start KA after it) is
  suppressed; the console message reflects the suppression.
  `CE_FRAME_INIT` in `ProcessCE` calls
  `flex_clear_init_history()` so a peer that resets its own
  state and signals so with a fresh INIT will get our INIT
  back on the next refresh.

  The table lives outside `FlexNetSessions[]`, so the cooldown
  survives any number of reaper/recreate cycles. v2.1.13's
  rate-limited LT cycle continues to work regardless of
  session lifecycle — the link-cost ring on PC/Flexnet's side
  is now durably owned by linbpq-flexnet for as long as
  PC/Flexnet keeps the peer entry.

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

## v2.1 — open items (PCF L2-cycle residual)

The PC/Flexnet IW2OHX-12 link from IR2UFV still cycles
**approximately every 3 hours** even with v2.1.24's per-peer KA
cadence. The cycle is **cosmetic for routing** — the L2 link
itself stays up across the event (BPQ `L` shows `S=5`
throughout), and v2.1.13's LT rate-limit re-converges the cost
ring to `2/2` within ~5 minutes after each reseed. `max_ssid=0-8`
also stays correct (retained from the original handshake INIT).
Decision (2026-05-29 with operator): **accept as the v2.1.24
floor**, ship as production-stable, revisit when there is fresh
information about BPQ's L2 LINKTABLE behaviour for AXIP peers.

Empirical timeline of the iteration:

| Version | Effective cycle | Trigger characterised |
|---------|-----------------|------------------------|
| v2.1.13 + transit ON  | ~ 90 min  | PCF DM-cycle on transit advert content |
| v2.1.23 + transit OFF | ~ 2.5 h   | PCF AXIP-side idle behaviour |
| v2.1.24 + 30 s KA     | ~ 3 h     | BPQ-side LINKTABLE recycle on AXIP port |

The remaining trigger is a **BPQ-internal LINKTABLE recycle**
specifically for the AXIP port to IW2OHX-12 (`192.168.1.201:10075`).
When BPQ recycles its LINKTABLE entry under us, the next CE frame
arrives on the freshly-allocated slot, our session-table lookup
misses, the new-slot branch of `FlexNet_InitSession` allocates a
fresh slot and emits INIT — and PC/Flexnet reseeds its link-cost
ring on every received INIT. The v2.1.14 reaper hysteresis,
v2.1.15 proactive-scan guard, and v2.1.16 reaper-time
LINK-migration scan all close subsets of this race but don't
eliminate it.

Possible follow-on directions (none in flight):

1. **L2Code.c LINKTABLE-recycle audit.** The 13 `CLEAROUTLINK`
   call sites in `L2Code.c` cover N2-retry exhaustion (XID/SABM/
   DISC), FRMR, L2KILLTIME idle, etc. Identify which path fires
   for our AXIP IW2OHX-12 link at the 3 h interval and either
   suppress it or hook into it cleanly enough that we can migrate
   our session without sending a fresh INIT.
2. **flxnod32.dll RE deeper.** PCF V4's L2 timeout state machine
   is `fcn.10002aa0` (≈ 6 KB) with a 21-case switch dispatch
   table at `0x100043a4`. The "infobox timeout: %d minutes"
   threshold lives at `[0x10020f4c]`. Reverse-engineer the
   per-link counter (`[esi+4]` in the disasm) and identify
   whether PCF exposes any sysop command (none visible in the
   `flxnod32.dll` strings we dumped) or PE config to extend the
   threshold for AXIP peers.
3. **Match xnet's per-peer activity pattern more closely.**
   v2.1.24 raised our KA cadence to 30 s for PCF peers; xnet
   peers also emit periodic STATUS+ route records every ~21 s.
   Sending similar status frames to PCF would risk the
   "PCF DMs on unsolicited record" behaviour flexnetd v0.7.8
   documented — needs careful timing per PCF's token state.

For deeper context, the full investigation captures + decoded
wire traces + r2 RE notes are in
[[project_pcf_axip_disc_cycle]].

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

_Document version: 2026-05-28 — v2.1.27 in production (both IW2OHX-13
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
- _v2.1.15 — proactive-CE-init guard. An established session
  (`got_peer_init == TRUE`) is no longer re-handshaked when some
  BPQ-internal path clears `LINK->FlexNetLink`; we just re-promote
  the flag. Closes the residual ~80-min session-reconnect path
  that survived v2.1.14._
- _v2.1.16 — LINK-migration second chance. Before the reaper
  destroys a bad-state session it scans for a fresh LINKTABLE
  slot with the same callsign on the same port (BPQ may have
  recycled the old slot via `CLEAROUTLINK`); if found, the
  session is migrated to the new LINK pointer without re-INITing
  the peer. Closes the residual `session started` (new-slot)
  cycle that survived v2.1.14+v2.1.15. `peer_callsign` field
  added to `FLEXNET_SESSION`._
- _v2.1.18 — AX.25-aware callsign equality (mask SSID byte to bits
  4..1). Insufficient: still failed on IR2UFV ↔ IW2OHX-12 with a
  second session-start sending INIT. Superseded by v2.1.19._
- _v2.1.27 — drop non-CE/CF PIDs on FlexNet-flagged links. Wire
  evidence 2026-05-30: PC/Flexnet sends a 7-byte PID=F0 frame on
  our FlexNet link, BPQ's L4/sysop layer interprets it as a user
  connect and echoes a 60-byte banner back; PCF receives the
  PID=F0 reply and DISC's the link 22 ms later. This was PCF's
  double-DISC pattern that v2.1.25/v2.1.26 couldn't catch via
  SABM-accept adoption — the second DISC fired regardless. Now:
  on a FlexNet-flagged LINK, non-CE/CF PIDs are dropped at L2
  level (after the L2 RR ack) so the banner is never sent and
  PCF doesn't see the wrong-PID trigger. Non-FlexNet links keep
  the original AX.25 V2.0 behaviour._
- _v2.1.26 — fix v2.1.25 skip-self bug. The skip-self check
  (`sess->LINK == new_link continue`) missed the case where BPQ
  reuses the same LINKTABLE memory slot after CLEAROUTLINK. Our
  session's `.LINK` pointer was unchanged across the recycle
  (slot reused), the check fired, no adoption, fresh INIT was
  sent. Removed the skip; adoption now handles both fresh-slot
  and reused-slot cases. Console message distinguishes the two
  for forensics._
- _v2.1.25 — adopt existing session on SABM-accept for PCF
  L2-cycle pattern. When PC/Flexnet runs its periodic DISC/SABM
  cycle, BPQ's CLEAROUTLINK + fresh-LINK allocation no longer
  triggers a fresh CE-INIT and PC/Flexnet ring reseed. The
  SABM-accept hook now calls `FlexNet_TryAdoptSession` first; if
  an active session for the peer's callsign exists, the LINK
  pointer is migrated in place (preserving got_peer_init /
  sent_routes / peer_max_ssid / peer_ka_term)._
- _v2.1.24 — per-peer-type proactive KA cadence. PC/Flexnet
  (peer_ka_term=='\r') gets KA every 30 s; (X)Net peers stay on
  300 s. Wire-capture evidence on iw2ohx-bpq 2026-05-29 showed PCF
  exchanges KAs with IW2OHX-4 every 16-32 s but with us every ~5 min
  — PCF cycles AXIP peers that go too quiet. Mimicking xnet's
  ~21 s cadence should keep PCF satisfied._
- _v2.1.23 — REVERT the v2.1.17→v2.1.22 INIT-cooldown stack.
  Wire-trace evidence (IR2UFV ↔ IW2OHX-12, 2026-05-28) showed
  PC/Flexnet intentionally `DISC+`/new-`SABM+` cycles the L2 link
  after every token-handover round and expects a full
  INIT→INIT→RTT→routes handshake on each new L2 session.
  Suppressing our INIT made PCF rebuild the peer entry with
  default `max_ssid=15`. v2.1.23 reverts: every InitSession path
  unconditionally emits INIT+KA on a fresh session, exactly like
  v2.1.16. The periodic `600 4095` cost-ring reseed is accepted
  as PCF's normal protocol behaviour — v2.1.13's LT rate-limit
  re-converges the ring to `2/2` within ~5 min after each reseed.
  All v2.1.14 (reaper hysteresis), v2.1.15 (proactive-scan guard
  for same LINK), and v2.1.16 (LINK-migration) fixes remain in
  place to handle the BPQ-internal LINK-recycle paths that don't
  involve a PCF DISC/SABM cycle._
- _v2.1.21 — drop the over-eager cooldown-clear on peer-INIT
  receive. v2.1.17's `flex_clear_init_history()` fired on every
  CE-INIT from the peer — including the routine handshake reply
  to our own outbound INIT, which immediately wiped the cooldown
  we'd just recorded. Diagnostic build v2.1.20 caught it on the
  wire (next session-recycle's cooldown lookup found a matching
  entry but with `last_tx == 0` → age ≈ 56 years → cooldown read
  as expired → INIT sent → PCF reseeded). v2.1.21 removes the
  clear. A peer that truly restarts no longer triggers an
  immediate re-INIT from us; the link survives on KAs alone until
  the natural `FLEXNET_INIT_TX_INTERVAL` cooldown expires._
- _v2.1.19 — callsign cooldown lookup via `ConvFromAX25`-normalized
  string. The 7-byte AX.25 representation of `LINKCALL` varies
  across BPQ code paths (L2Code.c:1059 masks byte 6 to 0x1E;
  L2Code.c:4823 masks to 0xFE; L2Code.c:2033 doesn't mask). The
  cooldown table now stores the human-readable callsign (e.g.
  `"IW2OHX-12"`) and compares via `strcmp`, bypassing every
  byte-level inconsistency. Same normalization applied to v2.1.16's
  reaper-time LINK-migration scan._
- _v2.1.17 — persistent per-peer INIT cooldown. Outbound CE-INIT
  is now rate-limited to once per (callsign, port) per
  `FLEXNET_INIT_TX_INTERVAL` (3600 s). Suppresses the reseed that
  occurs when v2.1.16's migration scan loses the BPQ race and a
  session is recreated via the new-slot path. The cooldown is
  cleared when the peer itself sends us a fresh CE-INIT (peer
  state was reset → we should reciprocate). History table lives
  outside `FlexNetSessions[]` so it survives reaper/recreate
  cycles._
