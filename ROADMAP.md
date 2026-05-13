# linbpq-flexnet → v2.0 GA: Roadmap & Gap Analysis vs flexnetd v1.0.0

## Summary

**linbpq-flexnet v1.9 (pre-GA)** is the current release tip (2026-05-12).
P2 M5 path-discovery work is **fully end-to-end**: items
**#7 (CE type-6 PATH_REQ), #8 (CE type-7 PATH_REP), and
#10 (background path probing)** are closed. The PATH_REQ wire format
names the immediate next-hop neighbour so peers forward and reply
with the full accumulated chain; replies populate the path cache
within the same second of the request. Item **#9** is partial — TTL
bumped to 4 h so a full round-robin cycle fits inside the cache
window, capacity resize to 256 entries still pending.

All P1 protocol-correctness items (#1, #2, #3, #4, #5, #6) shipped
in v1.3.x: real L3RTT counters with link-down guard, NetRom L3 INFO
envelope on replies, 3:1 IIR-smoothed link-time tracking (with wire
value decoupled and hardcoded to 2 per flexnetd convention), RTT=0
skip on `dtable_merge`, keepalive cadence at 180 s (PROTOCOL_SPEC),
and proactive KA threshold at 300 s (zero proactive on a healthy
xnet link). Node identity preservation (P4 #18) shipped in v1.2.

**Verification snapshot of `2:IW2OHX-13` in xnet's L (FlexNet
F-row): T=1** — converged to the documented healthy-peer value for
local AXUDP, matching `1:IW2OHX-12 F 1`, `3:IGATE F 1` and the
other long-uptime neighbours.

**Remaining gap vs flexnetd v1.0.0**: P2 M5 path-discovery protocol
(#7–#10). Closing these will bring linbpq-flexnet to v2.0 GA
parity.

**Correction on the v1.3.5 release notes:** the v1.3.5 commit's
claim that v1.3.5's RTT=0 skip closed a "Q-row caveat" from
v1.3.4 is invalidated. The Q-state row in xnet's `L*` for
`2:IW2OHX-13` is xnet's NetRom routing-table entry, not the
FlexNet link entry — outside the scope of any FlexNet protocol
change. xnet maintains parallel rows: a FlexNet F-row (with `T`
column showing our wire LT value) and a NetRom Q-row (with
`-/-` rtt). See `V1.3_DESIGN.md` item #3 Resolution → Correction
section. v1.3.6's wire-LT fix is the first commit that addresses
the actual FlexNet-side concern (the F-row T column), and it
converges xnet's display from 65 to 3 — matching every healthy
peer.

---

## Gaps by Category

### P1 — Protocol correctness gaps

| # | Feature | flexnetd v1.0 | linbpq-flexnet status | Impact |
|---|---------|---------------|------------------------|--------|
| 1 | **L3RTT c1-c4 counters** | Full tick-based counters, 10ms granularity, proper c3/c4 semantics | ✅ DONE in **v1.3.0** — raw `CLOCK_MONOTONIC` ticks, real c3/c4 in every reply | Neighbor can now compute RTT from our replies |
| 2 | **L3RTT c3=0, c4=0 = link-down** | Enforced | ✅ DONE in **v1.3.0** — `flex_count_reachable()` gate, zeros sent when no reachable dests | Peer detects our link state transitions |
| 2b | **L3 INFO envelope on replies** | Implicit (flexnetd builds correct L3 frames) | ✅ DONE in **v1.3.3** — `flexl3_build_info` wrap, `dest=peer / ttl=mirror / IN/ID echo`. See `V1.3_DESIGN.md` for three-iteration resolution (v1.3.1 → v1.3.2 → v1.3.3) | Without wrap, xnet parses payload but never binds reply to its pending-probe table |
| 3 | **Keepalive interval** | 180s (per PROTOCOL_SPEC) | ✅ DONE in **v1.3.6** — proactive timer at `FlexNetCode.c:1062` changed `>= 21` → `>= 180`. Verified: 5 KAs per 13-min steady window at exact 189 s cadence (xnet's native rhythm), 9× reduction from v1.3.5's ~45. | KA cadence matches spec |
| 4 | **Proactive KA threshold** | 300s adaptive (v0.7.9) | ✅ DONE in **v1.3.7** — `FlexNetCode.c:1066` constant `>= 180` → `>= 300`. 300 s > xnet's 189 s native cadence, so xnet's reactive KAs always pre-empt our timer. Verified: zero proactive KAs across 15-min capture, only reactive echoes at exact 189 s spacing. | Backup-only proactive, silent on healthy link |
| 5 | **Link time IIR filter** | Smoothed from actual measurements | ✅ DONE in **v1.3.4** — 3:1 IIR (`smoothed = (3·smoothed + sample) / 4`) fed by CE LT round-trip samples. Verified: wire LT values converged 56→43→33→26→20 across 5 bursts in 12.5 min capture. See `V1.3_DESIGN.md` for full resolution. | Link time now reflects measured RTT |
| 6 | **dtable_merge RTT=0 skip** | v0.7.5 fix: skip RTT=0 merges | ✅ DONE in **v1.3.5** — `incoming->rtt == 0 && !is_infinity` guard at top of `flex_dtable_merge`, preserves existing entry's `last_updated` if found, skips entirely otherwise. New `flex_find_dest` helper used by both paths. Verified: 7 entries skipped across 2 batches (97 total) in 15-min capture; closes v1.3.4 Q-row caveat as side effect. See `V1.3_DESIGN.md`. | Real RTTs preserved |

### P2 — Missing features (M5 path protocol)

| # | Feature | flexnetd v1.0 | linbpq-flexnet v1.1 | Notes |
|---|---------|---------------|---------------------|-------|
| 7 | **CE type-6 path request** | Full implementation: HOP_BYTE + QSO + origin + target | ✅ DONE in **v1.9** — `flex_parse_path_frame` + `flex_build_path_req` + `flex_handle_path_req` (target-role) + `flex_send_path_req` (originator). Wire format decoded from real-peer captures, cross-checked against flexnetd/ce_proto.c:580. See `V1.4_DESIGN.md`. | Path discovery enabled |
| 8 | **CE type-7 path reply** | Accumulated callsign list, 80-hop TTL cap | ✅ DONE in **v1.9** — `flex_build_path_rep` + `flex_handle_path_rep`, populates FlexNetDests[].path_hops[] via QSO-matched pending-probe table | Reply builder and parser in place |
| 9 | **Path cache** | 256 entries, 300s TTL | 16 hops/entry × 64 destinations, TTL bumped 120s → 14400s (4h) in v1.9 so a full round-robin cycle (~3h at 60s/probe × ~190 dests) fits inside the cache window. Capacity-side resize to 256 entries still pending. | Partial — TTL fix shipped, capacity resize pending |
| 10 | **Background path probing** | Round-robin with 30s timeout per target | ✅ DONE in **v1.9** — `FlexNet_Timer` fires one type-6 PATH_REQ every 60s round-robin via `g_path_probe_idx`; pending-probe table (8 slots) tracks QSO→dest. After the wire-format correction below, **real PATH_REP replies arrive within the same second** and populate `path_hops[]`. | Fully working end-to-end |

**Wire-format correction (v1.9):** initial implementation sent
`6 + HOP + QSO + <origin> ' ' <target>` and observed no replies. Dual-port
capture between IW2OHX-14 / IW2OHX-12 / IW2OHX-4 on 2026-05-12 showed
real peers send `6 + 0x21 + QSO + <origin> ' ' <next_hop> ' ' <target>`,
forwarding hop-by-hop and accumulating the chain. Fix shipped: PATH_REQ
now names the immediate next-hop neighbour; peers forward and the
target replies with the full chain. The local-walk D fallback remains
in place for destinations not yet probed by the round-robin.

### P4 — Node identity preservation ✅ DONE in v1.2.0

| # | Feature | flexnetd v1.0 | linbpq-flexnet v1.2 |
|---|---------|---------------|---------------------|
| 18 | **Outbound node-in-digi-chain** | `AX25_IAMDIGI` socket option + H-bit on first digi (URONode patch) | ✅ Working — two-digi `MYCALL* NEIGHBOR` in Cmd.c + L2Code.c RX fix |

**Solution (v1.2.0, 2026-04-22):** flexnetd uses Linux kernel `AX25_IAMDIGI`.
LinBPQ has its own internal L2 stack with no equivalent flag, so v1.2
implements the mechanism directly:

1. **Outbound (`Cmd.c`)** — SABM built with two-digi chain `MYCALL* NEIGHBOR`
   (H-bit set on MYCALL, clear on NEIGHBOR).
2. **Inbound (`L2Code.c`)** — when the remote replies with mirrored digi
   list `NEIGHBOR* MYCALL`, the standard digipeat logic would retransmit the
   frame. Intercepted: if an active LINK exists for `(ORIGIN, DEST, Port)`,
   mark our H-bit and consume locally.

**Verified:** IR5S `u` shows `IR5S>IW7EAS v IQ5KG-7 IW2OHX-13` — our node
appears as the last digi, not the upstream neighbor.

---

## Recommended Roadmap for linbpq-flexnet v2.0 GA

### v1.2 — Identity fix ✅ RELEASED (2026-04-22)

- Node identity in outbound digi chain (P4 #18) — **done**

### v1.3 — Protocol correctness

Implementation order (each item lands as a focused commit before moving on):

1. **L3RTT c1-c4 counters + link-down guard** (P1 #1, #2) — ✅ done in v1.3.0
2. **L3 INFO envelope on L3RTT replies** (P1 #2b) — ✅ done in v1.3.3
3. **Link-time IIR filter** (P1 #5) — ✅ done in v1.3.4 (wire-advertisement corrected in v1.3.6)
4. **`dtable_merge` RTT=0 skip** (P1 #6) — ✅ done in v1.3.5
5. **Keepalive 180s** (P1 #3) — ✅ done in v1.3.6 (wire-LT decoupling folded in)
6. **Proactive KA adaptive threshold** (P1 #4) — ✅ done in v1.3.7

All P1 items closed.

### v1.4 — Path protocol initial drop (superseded by v1.9)

Items below shipped in the v1.4.x train and were rolled forward into
the v1.9 pre-GA release after the wire-format fix:

- CE type-6/7 path request/reply (P2 #7, #8) — done
- Background path probing (P2 #10) — done
- Path cache TTL bump (P2 #9 partial) — done

### v1.9 — pre-GA ✅ RELEASED (2026-05-12)

Snapshot of the M5 path-discovery work after the dual-port-capture-
driven wire-format correction. CE type-6 PATH_REQ now names the
immediate next-hop neighbour (`<origin> <next_hop> <target>`) so
peers forward and reply with the full accumulated chain. `D <call>`
detail view renders the cached chain; D list view marks resolved
entries with `!` in the `Path` column. Path cache TTL is 14 400 s
(4 h) to cover one full round-robin cycle.

### v2.0 GA — Final polish

- Soak-test v1.9 across multi-day uptime: confirm no leaks, no probe
  starvation, no cache thrash.
- Capacity side of P2 #9: lift `FLEXNET_MAX_DESTS` from 64 to 256
  (or auto-grow) and bump `path_hops[]` per-entry storage to match.
- README rewrite as v2.0 GA announcement.
- Full feature parity verified against flexnetd v1.0 where applicable.
- Integration tag for side-by-side deployment with flexnetd.

### v2.x — Open items beyond GA

The following items are deliberately deferred past GA and tracked
here so they don't get lost.

#### v1.9.3 — AXIP routing + session-table hygiene ✅ RELEASED (2026-05-13)

Bug-fix release uncovered by the v1.9.2 multi-neighbour soak.
Three issues that combined to make outbound FlexNet connects fail
~98% of the time with two F-flagged MAPs:

1. `bpqaxip.c` AXIP-TX SSID-byte normalisation (`& 0x7e`) didn't
   restore the AX.25 reserved bits (`0x60`); the `memcmp` against
   `arp_table` (which has them set, via `convtoax25`) failed on
   byte 6 for every digi-resolved next-hop. The "first F-flagged"
   fallback then picked `arp[0]` blindly. Fixed by `axcall[6] =
   (axcall[6] & 0x7e) | 0x60;`.
2. `L2Code.c` auto-bootstrapped a FlexNet session on every
   incoming CE-PID frame regardless of source. Users relayed via
   URONode appeared in `FL`. Gated on
   `FlexNet_IsPeerFlexNetMapped` (the same helper added in
   v1.9.2 for the proactive init scan).
3. `FlexNet_InitSession` matched only by LINK pointer; an L2
   reconnect with a fresh LINK left an orphan session row.
   Added callsign-match path + a per-tick reaper in
   `FlexNet_Timer` that drops sessions whose LINK is gone /
   dead / has a blank LINKCALL, and demotes any destinations
   attributed to the reaped slot.

#### v1.9.4 (next focus) — transit-role D-table re-advertisement

With multi-neighbour and the v1.9.3 routing fixes both in place,
linbpq finally routes traffic through the correct neighbour, but
it still advertises only `MYCALL` to each FlexNet peer. A proper
distance-vector router re-broadcasts the destinations it can reach
**on behalf of** other neighbours so the cloud converges on full
reachability. **This is the next blocker** before v2.0 GA — until
it ships, two FlexNet peers sitting behind us see linbpq as a
leaf, and the other neighbour's destinations are not advertised
through us. Same five mechanics as before (split-horizon, cost
adjustment, periodic CE-COMPACT-BATCH send, withdraw on
via_session_idx flip, RTT=0 refresh marker on TX).

Required mechanics:

1. Per-neighbour outbound D-table built by iterating
   `FlexNetDests[]` and including every entry whose
   `via_session_idx` is **not** the target neighbour's session
   (split-horizon — avoid feeding routes back to the neighbour
   that taught them).
2. Cost adjustment: each re-advertised entry's `rtt` becomes
   `dest->rtt + sess->our_link_time` so peers see the real cost
   through us, not the cost the original neighbour reported.
3. Periodic CE-COMPACT-BATCH send: same cadence as `flexnetd`'s
   poll cycle (today the code sends only on init / on change).
4. Route withdrawal: when `via_session_idx` for a destination
   changes (failover) or the destination goes infinity, the
   previous chosen neighbour should receive a withdraw
   announcement (rtt=infinity) so the change propagates cleanly.
5. Cross-check against `flexnetd`'s `poll_cycle.c` /
   `dtable.c:50-75` — the RTT=0 refresh-marker pattern (already
   honoured on the receive side, see item #6 from v1.3.5) must
   also be emitted on the transmit side every N cycles.

Without this work, two FlexNet neighbours sitting behind a v1.9.2
linbpq remain mutually invisible: each one knows it can reach
IW2OHX-13 but doesn't learn that the other neighbour's
destinations are reachable through us.

1. ~~**On-disk path cache**~~ — ✅ DONE in **v1.9.1**.
   Persists `path_hops[]` + `path_updated` to
   `flexnet_path_cache.dat` (in `linbpq`'s CWD) every 5 min when at
   least one row has changed. Reloads on startup, skipping entries
   older than 5 h. Eliminates the ~3 h post-restart re-probe
   warm-up. Cached entries render in `D <call>` immediately after
   restart; round-robin re-probing refreshes them in the
   background.
2. **Full portability into flexnetd** — extract the shared protocol
   surface (CE type-6/7 build/parse, QSO allocator, probe table) into
   a module `flexnet_path_proto.c` consumed by both repos. Per-repo
   adapters cover the differing config / dest-entry / send paths.
3. ~~**Multiple FlexNet neighbours per port and across ports**~~ —
   ✅ DONE in **v1.9.2**. Sessions are keyed by L2 LINK pointer, not
   by BPQ port number, so multiple FlexNet neighbours can coexist on
   one port. Proactive CE-init scan in `FlexNet_Timer` bootstraps
   sessions on F-flagged MAP entries whose peer hasn't initiated.
   `flex_dtable_merge` records `via_session_idx` per destination on
   a lowest-RTT-wins basis; `flex_send_path_req` and
   `FlexNet_FindRoute` route through the chosen session. Routing is
   transparent (no Via column in `D` — only RTT and the `!`
   cached-path marker). `FL` shows per-session route counts.
4. **SSID-range "application" mapping** — each SSID 0–15 on the
   node callsign maps to an "application" inside the FlexNet stack
   (BBS, chat, mail gateway, etc.). v2.x exposes this as a
   configurable bind table so users can reach a specific service by
   addressing the right SSID rather than going through the BPQ
   command parser.

---

## Shared Code Strategy

The original plan was to factor portable algorithms out of flexnetd into a
shared module reused by both projects. **Reality check (2026-05-10):**

- `flexnet_l3.c/h` in this repo (the assumed precedent) is currently
  **dead code** — compiled to `flexnet_l3.o` but not linked from
  `FlexNetCode.c`. flexnetd has no corresponding file; it inlines L3 work
  in `cf_proto.c`, `ce_proto.c`, and `poll_cycle.c`.
- No build-time sync exists between the two repos.

### Per-item shareability (from v1.3 pre-implementation investigation)

| # | Feature | Verdict | Note |
|---|---------|---------|------|
| 1 | L3RTT c1-c4 counters       | GOOD    | `cf_build_l3rtt()` is pure string formatting; tick math is portable |
| 2 | c3=0/c4=0 link-down guard  | GOOD    | Trivial conditional |
| 3 | Keepalive 180s             | PARTIAL | Constant is portable; config plumbing is per-repo |
| 4 | Proactive KA 300s adaptive | POOR    | flexnetd's threshold is tuned against (X)NET's 189s — xnet-specific |
| 5 | IIR filter on link_time    | GOOD    | `(3*old + new)/4` on uint32_t |
| 6 | dtable_merge RTT=0 skip    | GOOD    | Single-line predicate on the dest-entry struct |

### Decision: implement v1.3 in-place, extract shared module after stabilization

v1.3 implements all six items directly in `FlexNetCode.c` and helpers. After
live testing on IW2OHX-13 confirms stability, items #1, #2, #5, #6 (the GOOD
ones) become candidates for extraction into a shared `flexnet_l3_proto.c`
reused by both repos. Items #3 and #4 stay per-repo: #3's plumbing differs
between projects, #4's threshold is xnet-specific.

**Why not extract upfront:** call sites differ structurally between the two
projects (different config systems, different dest-entry types, different
L3RTT reply builders). A shared module would need callback hooks or opaque
pointers, adding indirection for ~200 LoC of reuse before either side has
stabilized. Wait until both implementations exist and converge.

---

## Reference

- **flexnetd v1.0.0**: https://github.com/onionuser79/flexnetd
- **linbpq-flexnet v1.9** (current, pre-GA): https://github.com/onionuser79/linbpq-flexnet
- **PROTOCOL_SPEC.md** (flexnetd repo): canonical FlexNet protocol reference

---

_Document version: 2026-05-12 (v1.9 ships #7+#8+#10 fully working — M5 closed; v2.0 GA next)_
_Author: IW2OHX_
