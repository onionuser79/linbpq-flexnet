# linbpq-flexnet → v2.0 GA: Roadmap & Gap Analysis vs flexnetd v1.0.0

## Summary

**linbpq-flexnet v1.9.5** is the current production tip (2026-05-13). P1
protocol correctness and P2 M5 path discovery are closed end-to-end.
v1.9.1 added an on-disk path cache; v1.9.2 multi-FlexNet-neighbour
support; v1.9.3 AXIP routing + session-table hygiene; v1.9.4 transit-role
D-table re-advertisement; v1.9.5 the `C <flexnet-neighbour>` fixes
(self-referential digi-chain in `Cmd.c` + pid=CF fall-through to NetROM
L4 in `L2Code.c`).

The 2026-05-14 test rerun shows BPQ-13 outgoing to non-neighbour FlexNet
cloud destinations succeeding at **89 %** (Test 1, vs 13 % under v1.9.4)
and xnet → linbpq direct-neighbour connect at **100 %** (Test 3). Where
connects still fail — direct `C <flexnet-neighbour>` from -13, transit
through -13 originated at -4 — wire-trace evidence shows the drop is on
the xnet peer, not on linbpq. linbpq emits well-formed CREQs which xnet
L2-acks but does not surface to its NetROM L4 (mirror of the v1.9.5 fix,
but on the xnet side).

**Remaining work for v2.0 GA**: route withdrawal on `via_session_idx`
failover, periodic RTT=0 TX refresh marker, P2 #9 capacity-side resize
(64 → 256 dests), multi-day soak of v1.9.5, README rewrite, integration
tag.

---

## Gaps by Category

### P1 — Protocol correctness gaps

| # | Feature | flexnetd v1.0 | linbpq-flexnet status |
|---|---------|---------------|------------------------|
| 1 | **L3RTT c1-c4 counters** | Full tick-based counters, 10 ms granularity | ✅ v1.3.0 |
| 2 | **L3RTT c3=0, c4=0 = link-down** | Enforced | ✅ v1.3.0 |
| 2b | **L3 INFO envelope on replies** | Implicit | ✅ v1.3.3 — `flexl3_build_info` wrap, `dest=peer / ttl=mirror / IN/ID echo` |
| 3 | **Keepalive interval 180 s** | PROTOCOL_SPEC | ✅ v1.3.6 |
| 4 | **Proactive KA threshold 300 s** | Adaptive | ✅ v1.3.7 |
| 5 | **Link time IIR filter** | Smoothed | ✅ v1.3.4 |
| 6 | **dtable_merge RTT=0 skip** | v0.7.5 fix | ✅ v1.3.5 |

All P1 items closed.

### P2 — M5 path-discovery protocol

| # | Feature | flexnetd v1.0 | linbpq-flexnet status |
|---|---------|---------------|------------------------|
| 7 | **CE type-6 path request** | HOP_BYTE + QSO + origin + target | ✅ v1.9 (wire format corrected to include next-hop neighbour) |
| 8 | **CE type-7 path reply** | Accumulated callsign list | ✅ v1.9 |
| 9 | **Path cache** | 256 entries, 300 s TTL | TTL side ✅ v1.9 (14 400 s = 4 h covers a round-robin cycle). Capacity-side resize 64 → 256 still pending for v2.0 GA. |
| 10 | **Background path probing** | Round-robin, 30 s timeout per target | ✅ v1.9 — `FlexNet_Timer` fires one type-6 PATH_REQ every 60 s |

### P4 — Node identity preservation

| # | Feature | flexnetd v1.0 | linbpq-flexnet status |
|---|---------|---------------|------------------------|
| 18 | **Outbound node-in-digi-chain** | `AX25_IAMDIGI` socket option + H-bit on first digi | ✅ v1.2 — two-digi `MYCALL* NEIGHBOR` in `Cmd.c` + `L2Code.c` RX fix. In v1.9.5 the chain is suppressed (no-digi plain SABM) when target == neighbour, to avoid the self-referential frame that was breaking direct-neighbour connects. |

---

## Release timeline (shipped)

| Tag | Date | What |
|-----|------|------|
| **v1.2** | 2026-04-22 | Node identity in outbound digi chain |
| **v1.3.0–1.3.7** | 2026-04 → 2026-05 | All six P1 protocol-correctness items |
| **v1.4.0** | 2026-05-12 | First CE type-6/7 cut + background probing (rolled into v1.9) |
| **v1.9 pre-GA** | 2026-05-12 | M5 closed end-to-end, wire-format corrected, path cache TTL 4 h |
| **v1.9.1** | 2026-05-12 | On-disk path cache (`flexnet_path_cache.dat`) |
| **v1.9.2** | 2026-05-12 | Multi-FlexNet-neighbour cost-based routing |
| **v1.9.3** | 2026-05-13 | AXIP byte-6 normalisation + session-table hygiene |
| **v1.9.4** | 2026-05-13 | Transit-role D-table re-advertisement (split-horizon, cost-adjust, 300 s cycle) |
| **v1.9.5** | 2026-05-13 | `C <flexnet-neighbour>` fix: Cmd.c no-digi when target==neighbour + L2Code.c `case 0xcf` returns 0 when not L3RTT, so NetROM L4 receives CACK/INFO |
| (cosmetic) | 2026-05-14 | 3-column D output (xnet-style 24-char cells × 3) + `CE-UNKNOWN` log entry in `/tmp/flexnet_axudp.log` |

---

## v1.9.5 test results (2026-05-14 rerun)

Detailed numbers and target lists in `project_linbpq_v1_9_5_test_results.md`
(memory). Summary:

| Test | Origin → targets | v1.9.4 | v1.9.5 |
|---|---|---|---|
| 1 | BPQ-13 → 18 low-cost FlexNet cloud dests | 2 / 15 = 13 % | **17 / 19 = 89 %** |
| 2 | BPQ-4 → 18 transit-via-IW2OHX-13 dests | 1 / 13 = 8 % | 1 / 14 = 7 % (xnet-side, not linbpq) |
| 3 | BPQ-14 → IW2OHX-13 (1 target) | — | **21 / 21 = 100 %, sub-second** |

Where v1.9.5 did not change Test 2's numbers, wire-trace analysis on
2026-05-14 showed that xnet -4 fails to send CREQs over the wire for the
failed targets — linbpq's transit role is never exercised. The 7 % is an
xnet -4 route-selection behaviour, not a linbpq transit bug. Similarly,
`C IW2OHX-14` from -13 emits a well-formed CREQ on the wire that xnet
L2-acks but does not L4-reply to — symmetric to the bug v1.9.5 fixed in
linbpq, but on xnet's side.

---

## v1.9.7 (2026-05-14) — v1.9.4 transit-role re-advertisement REVERTED

After repeated transit-role failures during live testing (Test 2's
~7 % success rate could not be improved past xnet-side route-selection,
and follow-up v1.9.6 work to extend the AX.25 digi chain on transit
broke AX.25 V2 reciprocity at the originating xnet), the
distance-vector transit re-advertisement was **rolled back** to the
v1.9.3 leaf-with-multiport state.

linbpq now once again advertises only `MYCALL` to each FlexNet peer.
Two FlexNet neighbours behind linbpq remain mutually invisible at the
D-table level (each sees IW2OHX-13 as a leaf). This is the documented,
intended state until a re-designed transit-role mechanism that
preserves AX.25 V2 reciprocity is available.

v1.9.5 (the `C <flexnet-neighbour>` fixes — Cmd.c no-digi when target
== neighbour + L2Code.c pid=CF fall-through to NetROM L4) is **kept**:
those are independent of the transit work and remain valuable.

## v2.0 GA — outstanding work

1. **(re-imagined) Transit-role D-table re-advertisement** — re-design
   needed. The v1.9.4 mechanism extended the digi chain (or implied a
   peer extension downstream) which broke AX.25 V2 SABM/UA reciprocity
   at the originating xnet (3-digi UA didn't match the 2-digi SABM the
   originator sent). Future approach: either (a) restrict
   re-advertisement to peers that share an L3 NetROM transit layer with
   us — falling back to leaf for L2-only-SABM users; or (b) use a
   different routing mechanism entirely (NetROM-style L3 forwarding
   rather than L2 digipeat). Needs investigation before v2.0 GA.
2. **Route withdrawal on `via_session_idx` failover** — deferred with
   transit re-advertisement; revisit when transit is re-designed.
3. **Periodic RTT=0 refresh marker on TX side** — likewise.
4. **P2 #9 capacity side** — lift `FLEXNET_MAX_DESTS` from 64 to 256
   (or auto-grow), bump `path_hops[]` per-entry storage to match. TTL
   side already shipped in v1.9. Independent of transit work.
5. **Multi-day soak** of v1.9.7 — confirm no leaks, no probe-table
   starvation, no cache thrash, no session-table accumulation.
6. **Verification against flexnetd v1.0** — full feature-parity
   walk-through; document any intentional divergences (now including
   "linbpq is a leaf, not a transit").
7. **README rewrite** as the v2.0 GA announcement.
8. **Integration tag** for side-by-side deployment with flexnetd.

---

## v2.x — deferred past GA

The following are deliberately deferred past GA so they don't bloat the
v2.0 scope.

1. ~~**On-disk path cache**~~ — ✅ DONE in **v1.9.1**.
2. **Full portability into flexnetd** — extract the shared protocol
   surface (CE type-6/7 build/parse, QSO allocator, probe table) into a
   `flexnet_path_proto.c` consumed by both repos. Per-repo adapters
   cover the differing config / dest-entry / send paths.
3. ~~**Multiple FlexNet neighbours per port / across ports**~~ — ✅
   DONE in **v1.9.2**.
4. **SSID-range "application" mapping** — each SSID 0–15 on the node
   callsign maps to an "application" inside the FlexNet stack (BBS,
   chat, mail gateway, etc.). v2.x exposes this as a configurable bind
   table so users can reach a specific service by addressing the right
   SSID rather than going through the BPQ command parser.
5. **`CE_FRAME_STATUS_1x` parser entry** — the 3-byte `"12\r"` frame
   from xnet (sibling of the known `"10\r"` STATUS_10, `"3+\r"`
   STATUS_POS, `"3-\r"` STATUS_NEG family) currently falls to the
   `CE-UNKNOWN` default and is silently dropped. Benign in practice;
   add a parser entry + no-op handler once its semantic is documented.
6. **xnet upstream tracking** — once xnet ships its mirror of the
   v1.9.5 `case 0xcf` fall-through fix, `C <flexnet-neighbour>` from
   linbpq toward xnet peers will start working end-to-end. Until then
   the workaround is to use a non-FlexNet intermediate route (e.g.
   reach IW2OHX-14 via the NetROM peer IW2OHX-1 instead of the direct
   FlexNet path).

---

## Shared Code Strategy

The original plan was to factor portable algorithms out of flexnetd into
a shared module reused by both projects. **Reality check (2026-05-10):**

- `flexnet_l3.c/h` in this repo is currently **dead code** — compiled to
  `flexnet_l3.o` but not linked from `FlexNetCode.c`. flexnetd has no
  corresponding file; it inlines L3 work in `cf_proto.c`, `ce_proto.c`,
  and `poll_cycle.c`.
- No build-time sync exists between the two repos.

### Per-item shareability (from v1.3 pre-implementation investigation)

| # | Feature | Verdict | Note |
|---|---------|---------|------|
| 1 | L3RTT c1-c4 counters       | GOOD    | `cf_build_l3rtt()` is pure string formatting; tick math is portable |
| 2 | c3=0/c4=0 link-down guard  | GOOD    | Trivial conditional |
| 3 | Keepalive 180 s            | PARTIAL | Constant is portable; config plumbing is per-repo |
| 4 | Proactive KA 300 s adaptive| POOR    | flexnetd's threshold is tuned against (X)NET's 189 s — xnet-specific |
| 5 | IIR filter on link_time    | GOOD    | `(3·old + new) / 4` on uint32_t |
| 6 | dtable_merge RTT=0 skip    | GOOD    | Single-line predicate on the dest-entry struct |

### Decision: implement in-place, extract shared module after stabilization

All P1 + P2 items above implemented directly in `FlexNetCode.c` and
helpers. After multi-day soak of v1.9.5 confirms stability, items #1,
#2, #5, #6 (the GOOD ones) become candidates for extraction into a
shared `flexnet_l3_proto.c` reused by both repos. Items #3 and #4 stay
per-repo: #3's plumbing differs between projects, #4's threshold is
xnet-specific. Tracked as v2.x item #2 above.

---

## Reference

- **flexnetd v1.0.0**: https://github.com/onionuser79/flexnetd
- **linbpq-flexnet v1.9.5** (current production): https://github.com/onionuser79/linbpq-flexnet
- **PROTOCOL_SPEC.md** (flexnetd repo): canonical FlexNet protocol reference
- **Test methodology + results**: see memory file
  `project_linbpq_v1_9_5_test_results.md` for target lists, driver
  configuration, and per-test pass/fail numbers.
- **Investigation narrative** (wire traces, hypotheses, fixes,
  asymmetric-bug attribution): see memory file
  `project_linbpq_v1_9_4_stuck_after_disconnect.md`.

---

_Document version: 2026-05-14 (v1.9.5 + cosmetic refinements live; v2.0 GA = items 1–7 above)_
_Author: IW2OHX_
