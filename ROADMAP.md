# linbpq-flexnet → v2.0 GA: Roadmap & Gap Analysis vs flexnetd v1.0.0

## Summary

**linbpq-flexnet v1.4.0** is the current release tip (2026-05-12).
P2 M5 path-discovery work begins: items **#7 (CE type-6 PATH_REQ)
and #8 (CE type-7 PATH_REP)** are now closed. Bidirectional
implementation — we respond to incoming PATH_REQ when target = us
(matches flexnetd v1.0 target-role exactly), AND we originate
PATH_REQ from local BPQ `D` commands in parallel with the legacy
L3RTT-traceroute (first reply wins).

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
| 7 | **CE type-6 path request** | Full implementation: HOP_BYTE + QSO + origin + target | ✅ DONE in **v1.4.0** — `flex_parse_path_frame` + `flex_build_path_req` + `flex_handle_path_req` (target-role) + `flex_send_path_req` (originator). Wire format decoded from real-peer captures, cross-checked against flexnetd/ce_proto.c:580. See `V1.4_DESIGN.md`. | Path discovery enabled |
| 8 | **CE type-7 path reply** | Accumulated callsign list, 80-hop TTL cap | ✅ DONE in **v1.4.0** — `flex_build_path_rep` + `flex_handle_path_rep`, populates FlexNetDests[].path_hops[] via QSO-matched pending-probe table | Reply builder and parser in place |
| 9 | **Path cache** | 256 entries, 300s TTL | `path_hops[]` in-memory only (16 hops, 120s TTL) | Enlarge + make robust |
| 10 | **Background path probing** | Round-robin with 30s timeout per target | ✅ DONE in **v1.4.0** — `FlexNet_Timer` fires one type-6 PATH_REQ every 60s round-robin via `g_path_probe_idx`; pending-probe table (8 slots) tracks QSO→dest. **NOTE:** disasm of xnet (linuxnet x86 + xnet_arm7 ARM) confirms it does NOT implement type-6 RX (`cmpb $0x36` absent in both binaries). Probes go out, no replies arrive. Item ships as future-proofing for a peer with type-6 RX. | Infrastructure ready; needs a responding peer to bear fruit |

**Local-walk fallback for D command (v1.4.0):** since path_hops[] stays empty
in practice, `flex_show_dest_detail` synthesises `*** route: <MYCALL>
<via_callsign> <target>` from the local D-table. This gives visible
partial paths (2–3 callsigns) immediately. When a peer with type-6 RX
appears, the cached-path branch takes over automatically.

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

### v1.4 — Path protocol (M5 parity)

- CE type-6/7 path request/reply (P2 #7, #8)
- Enlarged + robust path cache (P2 #9)
- Background path probing (P2 #10)

### v2.0 GA — Final polish

- README rewrite as v2.0 GA announcement
- Full feature parity verified against flexnetd v1.0 where applicable
- Integration tag for side-by-side deployment with flexnetd

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
- **linbpq-flexnet v1.4.0** (current): https://github.com/onionuser79/linbpq-flexnet
- **PROTOCOL_SPEC.md** (flexnetd repo): canonical FlexNet protocol reference

---

_Document version: 2026-05-12 (v1.4.0 ships #7+#8 — M5 path discovery begun)_
_Author: IW2OHX_
