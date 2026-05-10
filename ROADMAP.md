# linbpq-flexnet → v2.0 GA: Roadmap & Gap Analysis vs flexnetd v1.0.0

## Summary

**linbpq-flexnet v1.3.3** is the current production release (2026-05-10).
P1 items #1 and #2 are closed — real L3RTT counters with link-down guard,
wrapped in proper NetRom L3 INFO envelopes that xnet's reply-binder
recognises. Node identity preservation (P4 #18) shipped in v1.2.

**Remaining gap vs flexnetd v1.0.0**: P1 timing-quality items #3–#6
(keepalive cadence, IIR-filtered link time, dtable_merge RTT=0 skip)
and the P2 M5 path-discovery protocol (#7–#10). Closing these will
bring linbpq-flexnet to v2.0 GA parity.

---

## Gaps by Category

### P1 — Protocol correctness gaps

| # | Feature | flexnetd v1.0 | linbpq-flexnet status | Impact |
|---|---------|---------------|------------------------|--------|
| 1 | **L3RTT c1-c4 counters** | Full tick-based counters, 10ms granularity, proper c3/c4 semantics | ✅ DONE in **v1.3.0** — raw `CLOCK_MONOTONIC` ticks, real c3/c4 in every reply | Neighbor can now compute RTT from our replies |
| 2 | **L3RTT c3=0, c4=0 = link-down** | Enforced | ✅ DONE in **v1.3.0** — `flex_count_reachable()` gate, zeros sent when no reachable dests | Peer detects our link state transitions |
| 2b | **L3 INFO envelope on replies** | Implicit (flexnetd builds correct L3 frames) | ✅ DONE in **v1.3.3** — `flexl3_build_info` wrap, `dest=peer / ttl=mirror / IN/ID echo`. See `V1.3_DESIGN.md` for three-iteration resolution (v1.3.1 → v1.3.2 → v1.3.3) | Without wrap, xnet parses payload but never binds reply to its pending-probe table |
| 3 | **Keepalive interval** | 180s (per PROTOCOL_SPEC) | 21s | Over-transmits, wastes bandwidth |
| 4 | **Proactive KA threshold** | 300s adaptive (v0.7.9) | Fixed 21s | Doesn't coexist cleanly with (X)NET's 189s native KA cadence |
| 5 | **Link time IIR filter** | Smoothed from actual measurements | Hardcoded `our_link_time = 2` (200ms) — depends on #1 producing real ticks (now satisfied) | Link time never reflects real conditions |
| 6 | **dtable_merge RTT=0 skip** | v0.7.5 fix: skip RTT=0 merges | Not implemented | Real RTTs overwritten by protocol refresh markers |

### P2 — Missing features (M5 path protocol)

| # | Feature | flexnetd v1.0 | linbpq-flexnet v1.1 | Notes |
|---|---------|---------------|---------------------|-------|
| 7 | **CE type-6 path request** | Full implementation: HOP_BYTE + QSO + origin + target | Parsed as `DEST_BCAST`, no handler | Needed for traceroute-style path discovery |
| 8 | **CE type-7 path reply** | Accumulated callsign list, 80-hop TTL cap | Not implemented | Reply builder missing |
| 9 | **Path cache** | 256 entries, 300s TTL | `path_hops[]` in-memory only (16 hops, 120s TTL) | Enlarge + make robust |
| 10 | **Background path probing** | Round-robin with 30s timeout per target | Only on-demand from D command | Pre-populates cache for common destinations |

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

1. **L3RTT c1-c4 counters + link-down guard** (P1 #1, #2) — biggest item;
   unblocks real RTT measurements that the rest of v1.3 depends on
2. **Link-time IIR filter** (P1 #5) — needs #1 to produce measured RTTs first
3. **`dtable_merge` RTT=0 skip** (P1 #6) — prevents real RTTs being overwritten
   by protocol refresh markers
4. **Keepalive 180s** (P1 #3) — small constant change; verify against (X)NET's
   189s native cadence on the live IW2OHX-13 setup
5. **Proactive KA adaptive threshold** (P1 #4) — last; tuning is xnet-specific
   and likely diverges from flexnetd's exact algorithm

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
- **linbpq-flexnet v1.2.0** (current): https://github.com/onionuser79/linbpq-flexnet
- **PROTOCOL_SPEC.md** (flexnetd repo): canonical FlexNet protocol reference

---

_Document version: 2026-05-10 (v1.3 plan + shared-code reality check)_
_Author: IW2OHX_
