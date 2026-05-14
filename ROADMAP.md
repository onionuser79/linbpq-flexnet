# linbpq-flexnet — Roadmap

## Current production: v1.9.7 (2026-05-14)

linbpq-flexnet is a **leaf node** participating in a FlexNet mesh
alongside its existing NET/ROM stack. v1.9.7 is the production tip:
commit `be6634a` on `main`, tag `v1.9.7`, deployed on iw2ohx-gw.

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

### 1. `CE-UNKNOWN` investigation + parser entry

When linbpq receives a pid=0xCE frame whose shape doesn't match any
of the known CE-frame types, the default branch logs a
`CE-UNKNOWN` line to `/tmp/flexnet_axudp.log` with a 32-byte hex +
ASCII preview of the payload (this logging was added on 2026-05-14
specifically to surface unclassified frames).

In the live trace, the only `CE-UNKNOWN` we have observed so far is
a 3-byte sibling of the known status family:

```
hex = [31 32 0D]   ascii = ["12\r"]
```

That puts it in the same family as the recognised `"10\r"`
(STATUS_10), `"3+\r"` (STATUS_POS), `"3-\r"` (STATUS_NEG). The
specific semantic of `"12\r"` (and any other `"1n\r"` variants) is
not yet documented.

The GA work:

1. Capture more CE frames over a multi-day window to inventory all
   `CE-UNKNOWN` shapes that arrive in practice (we may see `"11\r"`,
   `"13\r"`, etc., from different peers / session states).
2. Cross-reference against `xnet` source and the FlexNet protocol
   spec — or, lacking documentation, infer the semantic from
   correlated wire context (e.g. what session state triggers the
   frame, what peer sends it, what we observe immediately before
   and after).
3. Add a parser entry — likely `CE_FRAME_STATUS_1x` with a per-digit
   no-op or specific handler — and remove the `CE-UNKNOWN` default
   for the digits we now understand.
4. Keep the `CE-UNKNOWN` default in place for any genuinely unknown
   frames so future protocol surprises remain visible.

This work is **investigation-led**, not feature-led. The acceptance
criterion is "we know what `"12\r"` means and we handle it
intentionally".

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
  `xnet`, `PC/FlexNet`, or `flexnetd`.
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
`linbpq-flexnet` and `flexnetd`. This is set aside — not part of
v2.0 GA. If it ever happens, it would be a sibling effort across
both repos, not a deliverable here.

---

_Document version: 2026-05-14 — v1.9.7 in production as
leaf-with-multiport; v2.0 GA = CE-UNKNOWN + SSID-range._
