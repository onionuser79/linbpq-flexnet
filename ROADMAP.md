# linbpq-flexnet → v2.0 GA: Roadmap & Gap Analysis vs flexnetd v1.0.0

## Summary

**linbpq-flexnet v1.1.0** is stable for the single-neighbor case but is
**protocol-wise behind flexnetd v1.0.0** in two areas: L3RTT/timing
correctness and path-discovery protocol. Closing these gaps will bring
linbpq-flexnet to v2.0 GA parity.

---

## Gaps by Category

### P1 — Protocol correctness gaps

| # | Feature | flexnetd v1.0 | linbpq-flexnet v1.1 | Impact |
|---|---------|---------------|---------------------|--------|
| 1 | **L3RTT c1-c4 counters** | Full tick-based counters, 10ms granularity, proper c3/c4 semantics | Text echo only — no counters, no timing fields | Neighbor cannot compute RTT from our replies |
| 2 | **L3RTT c3=0, c4=0 = link-down** | Enforced | Not implemented | Peer cannot detect our link state transitions |
| 3 | **Keepalive interval** | 180s (per PROTOCOL_SPEC) | 21s | Over-transmits, wastes bandwidth |
| 4 | **Proactive KA threshold** | 300s adaptive (v0.7.9) | Fixed 21s | Doesn't coexist cleanly with (X)NET's 189s native KA cadence |
| 5 | **Link time IIR filter** | Smoothed from actual measurements | Hardcoded `our_link_time = 2` (200ms) | Link time never reflects real conditions |
| 6 | **dtable_merge RTT=0 skip** | v0.7.5 fix: skip RTT=0 merges | Not implemented | Real RTTs overwritten by protocol refresh markers |

### P2 — Missing features (M5 path protocol)

| # | Feature | flexnetd v1.0 | linbpq-flexnet v1.1 | Notes |
|---|---------|---------------|---------------------|-------|
| 7 | **CE type-6 path request** | Full implementation: HOP_BYTE + QSO + origin + target | Parsed as `DEST_BCAST`, no handler | Needed for traceroute-style path discovery |
| 8 | **CE type-7 path reply** | Accumulated callsign list, 80-hop TTL cap | Not implemented | Reply builder missing |
| 9 | **Path cache** | 256 entries, 300s TTL | `path_hops[]` in-memory only (16 hops, 120s TTL) | Enlarge + make robust |
| 10 | **Background path probing** | Round-robin with 30s timeout per target | Only on-demand from D command | Pre-populates cache for common destinations |

### P4 — Node identity preservation (the v1.2 target)

| # | Feature | flexnetd v1.0 | linbpq-flexnet v1.1 |
|---|---------|---------------|---------------------|
| 18 | **Outbound node-in-digi-chain** | `AX25_IAMDIGI` socket option + H-bit on first digi (URONode patch) | Not working — remote sees IW2OHX-14, not IW2OHX-13 |

**Key difference**: flexnetd uses **Linux kernel AX.25 sockets** where
`AX25_IAMDIGI` does the trick. LinBPQ has its **own internal L2 stack** —
different mechanism needed. The bpqaxip TX injection approach we tried
broke the management link. Needs a different approach (likely in
L2Code.c `SETUPADDRESSES` or the AX.25 frame builder, with LinBPQ-specific
H-bit handling).

---

## Recommended Roadmap for linbpq-flexnet v2.0 GA

### v1.2 — Identity fix

- Node identity in outbound digi chain (P4 #18) — the hardest remaining problem

### v1.3 — Protocol correctness

- L3RTT c1-c4 counters (P1 #1, #2)
- Keepalive interval 180s (P1 #3)
- Link time IIR filter (P1 #5)
- `dtable_merge` RTT=0 skip (P1 #6)
- Proactive KA adaptive threshold (P1 #4)

### v1.4 — Path protocol (M5 parity)

- CE type-6/7 path request/reply (P2 #7, #8)
- Enlarged + robust path cache (P2 #9)
- Background path probing (P2 #10)

### v2.0 GA — Final polish

- README rewrite as v2.0 GA announcement
- Full feature parity verified against flexnetd v1.0 where applicable
- Integration tag for side-by-side deployment with flexnetd

---

## Shared Reuse Opportunity

Much of P1 (L3RTT counters, IIR filter, RTT=0 skip) is **algorithmic —
no LinBPQ-specific code**. These bits could be factored out of flexnetd
into shared portable C files (like `flexnet_l3.c/h` already does for
the L3 frame module) and reused in both projects. This keeps protocol
behavior in sync between flexnetd and linbpq-flexnet going forward.

---

## Reference

- **flexnetd v1.0.0**: https://github.com/onionuser79/flexnetd
- **linbpq-flexnet v1.1.0** (current): https://github.com/onionuser79/linbpq-flexnet
- **PROTOCOL_SPEC.md** (flexnetd repo): canonical FlexNet protocol reference

---

_Document version: 2026-04-21_
_Author: IW2OHX_
