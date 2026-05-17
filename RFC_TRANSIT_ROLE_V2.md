# RFC: Transit-Role v2 for linbpq-flexnet

**Status:** Accepted 2026-05-17 — implementation in progress
**Target version:** linbpq-flexnet v2.2.0
**Author:** IW2OHX (Marco) + Claude
**Date:** 2026-05-17
**Supersedes:** v1.9.4 transit-role design (reverted in v1.9.7)
**Companion:** `research/transit_v2/TRANSIT_BEHAVIOUR_REPORT.md` — wire study that informs every decision below

This RFC specifies how linbpq-flexnet becomes a **transit-capable
FlexNet node** — i.e. a node through which other peers can reach
destinations that we don't own but reach via one of our downstream
FlexNet neighbours. It is informed by 60+60+25 minutes of live wire
captures of `(X)NET V1.39` doing exactly that, plus the post-mortem
on why the earlier v1.9.4 attempt had to be reverted.

The headline conclusion from the research: **the v1.9.4 design was the
wrong abstraction.** Multi-hop transit in FlexNet is implemented at
**NetROM L4 over PID=CF**, not at AX.25 L2 with digi-chain extension.
v1.9.4 broke AX.25 V2 reciprocity precisely because it tried to extend
the L2 digi chain on transit forwarding — a thing xnet does not do.

---

## 1. Background

linbpq-flexnet v2.1.x is a **leaf** in the FlexNet mesh: it
participates in a FlexNet cloud as an endpoint, but it does not relay
other peers' destinations into other peers' D-tables. The result is
that a remote FlexNet station can connect *to* our node, but cannot
connect *through* our node to a station reachable only via one of our
other FlexNet links.

For the production topology (`IW2OHX-13` ↔ `IW2OHX-14` (xnet) ↔
`IW2OHX-4` (xnet) ↔ `IW2OHX-12` (PCF)), this isn't a hard problem
because xnet itself is the transit. But when we add a second
linbpq-flexnet node like `IR2UFV`, or when an operator wants to use
linbpq-flexnet as a hub between two disjoint FlexNet segments,
transit-role becomes essential.

The reverted v1.9.4 mechanism approached this by extending the AX.25
digi chain at L2 — emitting forwarded SABMs with our callsign added
to the chain. This worked one-way (the SABM reached the target) but
broke on the return path: AX.25 V2 §6.1 reciprocity requires the
returning UA / I / RR frames to carry a mirrored digi chain. The
extension we added did not match the LINK state at the originator,
so the originator's UA-match failed and the user saw no connection.
v1.9.4 was reverted in v1.9.7.

The wire-study research (`research/transit_v2/`) establishes
definitively that **xnet does NOT use L2 digi-chain extension for
multi-hop transit.** It uses **NetROM L4 CREQ over PID=CF**, with L2
remaining a clean direct peer-to-peer hop. This RFC therefore drops
the L2 approach entirely.

---

## 2. Goals

In scope for v2.2.0:

- **G1 — Route re-advertisement.** Re-advertise destinations learned
  from one FlexNet neighbour into the routing tables of our other
  FlexNet neighbours, so that we appear as a transit-capable node
  with cost `learned_cost + link_cost_to_source_neighbour`.
- **G2 — CREQ acceptance.** When a peer initiates a NetROM L4 CREQ
  (PID=CF) with a destination callsign reachable only via one of our
  downstream FlexNet links, accept the CREQ and forward it to the
  appropriate next-hop neighbour.
- **G3 — Full-session L4 forwarding.** Forward `IACK`, `INFO`, `DREQ`,
  `DACK`, and any other L4 control frames bidirectionally for an
  in-flight transit session for its full duration.
- **G4 — Poison-reverse on neighbour loss.** When a downstream
  neighbour goes away, immediately advertise its dependent
  destinations with `RTT=60000` to all other neighbours.
- **G5 — Split-horizon.** Never re-advertise a destination back to
  the neighbour we learned it from.
- **G6 — Wire compatibility.** Every emitted frame must be byte-for-
  byte indistinguishable from what (X)NET V1.39 emits in the same
  topological role. We have a Phase 2 capture as the gold-standard
  reference.

---

## 3. Non-Goals

These are explicitly **out of scope** for v2.2.0. Some are deferred,
some are forbidden:

- **NG1 — No AX.25 L2 digi-chain extension.** This was the v1.9.4
  failure mode. Never. The L2 frame carrying a transit-forwarded L4
  packet must have src = us, dst = next-hop-neighbour, no digi list.
- **NG2 — No new protocol semantics.** Stay wire-compatible with
  xnet. No new CE frame types, no new CF frame opcodes, no
  experimental extensions.
- **NG3 — No session-level multipath.** When a destination has two
  candidate next-hop neighbours, pick the cheapest and use only that
  one for the duration of the session. Failover within an in-flight
  session is deferred.
- **NG4 — No type-6/7 (path probe / path reply) forwarding work.**
  We already participate in path probes as an endpoint; we are not
  in v2.2 going to forward path probes through transit. Defer to a
  later RFC.
- **NG5 — No `?` indirect-measurement marker.** Phase 2 captured 50
  records from xnet; zero used the `?` prefix even on transit
  re-advertisements. We follow xnet — no `?`.
- **NG6 — No transit for the AX.25-V2 1-hop case.** That case
  already works correctly (skill §3.4, v1.2 + v2.1.8). This RFC
  does not change it.

---

## 4. The Existing Two Mechanisms — Unchanged

linbpq-flexnet already implements two of the three transit cases:

### 4.1 — 1-hop AX.25 V2 (CMDC00 FlexNet branch in `Cmd.c`)

When a user issues `C <call>` where `<call>` is reachable via a
direct FlexNet AXIP neighbour, `CMDC00` emits an AX.25 V2 SABM with
the two-digi chain `MYCALL* NEIGHBOUR` (v1.2). The downstream
neighbour digipeats it onward in standard AX.25. Reciprocity is
preserved by the existing L2-RX-DIGI trap in `L2Code.c` (skill §3.4
item 2). **No change.** This RFC does not touch `Cmd.c` or `L2Code.c`.

### 4.2 — Multi-hop NetROM L4 CREQ outbound (existing BPQ behaviour)

When a user issues `C <call>` where `<call>` is multi-hop (not
reachable via a direct neighbour, requires CREQ at L4), BPQ's
existing NetROM L4 stack constructs the CREQ and emits it as a
PID=CF I-frame on the chosen outbound FlexNet link. **No change.**

### 4.3 — NEW: Multi-hop CREQ inbound transit (this RFC)

When **we receive** a PID=CF I-frame on a FlexNet link, decode its
NetROM L4 envelope, and the destination callsign is reachable only
via one of our other FlexNet links: this is the transit case. v2.2
handles it for the first time.

---

## 5. Specification — Route Re-Advertisement (G1)

### 5.1 Data Model

Add a per-neighbour "learned-from-them" table to `FlexNetCode.c`:

```c
struct FLEXNET_LEARNED_ROUTE {
    char         dest_call[FLEXNET_MAX_CALLSIGN];
    int          ssid_lo;
    int          ssid_hi;
    int          rtt_at_neighbour;     /* the RTT the neighbour told us */
    time_t       last_heard;           /* for staleness / poison-reverse */
    int          source_session_idx;   /* which FlexNet session learned it */
};

struct FLEXNET_SESSION {
    ... existing fields ...
    struct FLEXNET_LEARNED_ROUTE learned[FLEXNET_MAX_LEARNED_PER_NEIGHBOUR];
    int learned_count;
    int learned_dirty;                 /* set when re-advertise needs to fire */
};
```

`FLEXNET_MAX_LEARNED_PER_NEIGHBOUR = 256` (conservative). With 8
sessions × 256 routes that's 2048 routes total — order of magnitude
larger than typical small-mesh deployments and still fits in tens of
kilobytes.

### 5.2 Update Path

In `FlexNet_ProcessCE`, the `CE_FRAME_COMPACT` (type 3) handler
already merges incoming routes into the global `FlexNetDests[]`. We
add: also record each parsed record into the source-session's
`learned[]` table. If the same dest already exists in the table,
update its RTT and `last_heard`.

Additionally, if the dest's RTT changed materially (e.g. > 20 %
difference or sign change between 60000-poison and active), set
`session->learned_dirty = TRUE` so the periodic emitter knows there's
something to advertise.

### 5.3 Advertisement Algorithm

A new timer-driven function `flex_periodic_advertise()` runs in
`FlexNet_Timer`. For each peer session:

```pseudocode
function flex_periodic_advertise(session):
    if now - session.last_advert < FLEXNET_ADVERT_INTERVAL:
        return
    session.last_advert = now

    candidates = []
    for sess2 in FlexNetSessions:
        if sess2 == session: continue            # split-horizon
        if not sess2.active: continue
        for route in sess2.learned:
            if route.rtt_at_neighbour >= FLEXNET_RTT_INFINITY:
                # poison-reverse — also advertise as poison
                advertise_rtt = 60000
            else:
                # additive RTT
                advertise_rtt = route.rtt_at_neighbour + link_rtt_to(sess2)
            candidates.append((route.dest_call, route.ssid_lo,
                               route.ssid_hi, advertise_rtt))

    # also include OUR OWN advertised destinations
    # (the existing flex_send_own_routes does this)
    candidates.append((MYCALL_BASE, our_ssid_lo, our_ssid_hi, OUR_RTT))

    # Phase 2 confirms xnet sends single-record CE frames toward PCF
    # and small batches (3-5 records) toward xnet peers. Match the
    # frame-size shape:
    if peer_is_xnet(session):
        flex_emit_compact_batch(session, candidates, max_records_per_frame=5)
    else:
        flex_emit_compact_singletons(session, candidates)
```

Constants:

```c
#define FLEXNET_ADVERT_INTERVAL          120     /* xnet's observed cadence */
#define FLEXNET_RTT_INFINITY             60000   /* per skill §1.6 */
#define FLEXNET_MAX_RECORDS_PER_BATCH    5
#define FLEXNET_READVERT_CHANGE_THRESHOLD 20    /* percent */
```

### 5.4 Wire Format Output

Per skill §1.6 and Phase 2 capture confirmation, emit one or more
`PID=CE` I-frames whose payload is:

```
'3' [CALL(6) SSID_LO_CHAR(1) SSID_HI_CHAR(1) RTT_DECIMAL ' ']+ '\r'
```

For a single-record advertisement (the PCF-style trickle):
```
hex: 33 49 51 32 4C 42 20 30 35 34 30 20 0D
ascii: 3 I Q 2 L B (space) 0 5 4 0 (space) (CR)
       ^ type
         ^^^^^^^ 6-char callsign space-padded
                ^ ssid_lo char ('0' = SSID 0)
                  ^ ssid_hi char ('5' = SSID 5)
                    ^^ RTT decimal "40"
                       ^ separator
                         ^ end of payload (CR)
```

For a multi-record batch (the xnet-peer style), concatenate records
separated by spaces with a single `\r` terminator and an optional
trailing `-` *before* the `\r` to mark batch-withdrawal (all records
in this batch are RTT=60000 poison).

**No `?` indirect prefix.** Ever. Phase 2 confirmed xnet does not
use it on transit re-advertisements; we don't either.

### 5.5 Token Handshake

Phase 1 + Phase 2 observed `3+` (REQUEST) and `3-` (END-OF-BATCH)
tokens flying in both directions per skill §1.6. We must honour the
peer state machine:

- **On RX `3+`**: emit our full current route view + trailing `3-`.
- **On RX `3-`**: peer has finished sending us a batch — flush
  parsed records into the learned[] table and merge into D-table.
- **On periodic timer**: emit single records or small batches per
  §5.3. Optionally include a `3+` REQUEST when we expect peer's
  table has churned (initial post-INIT, or after long silence).
  Per skill §1.6 there's a 360-s state-4 timeout if we send `3+`
  and don't get `3-` back; honour it.

Phase 1 showed xnet sends `3+`/`3-` heavily on xnet ↔ xnet links
(50 + 31 in 60 min) and lightly on xnet ↔ PCF links (14 + 5).
linbpq-flexnet should emit `3-` after every batch (whether single-
record or multi-record). `3+` REQUEST is emitted on session
establishment (CE INIT) and again roughly every 360 s thereafter.

---

## 6. Specification — CREQ Forwarding (G2 + G3)

### 6.1 Inbound CREQ Decode

A PID=CF I-frame may carry one of:
- An L3RTT probe / reply (payload starts with `L3RTT:`)
- A NetROM L4 frame (binary, per skill §2.3)

The dispatcher in `L2Code.c case 0xCF` already runs FlexNet's
`FlexNet_ProcessCF` first; if it returns 0 (not L3RTT, not our
business at L4), the frame falls through to BPQ's existing NetROM L4
handler. **This RFC inserts the transit decision INSIDE the existing
L4 handler**, not in `L2Code.c`.

Concretely: BPQ's L4 frame processor parses the binary NetROM L4
header (skill §2.3). When the opcode is CREQ and the destination
callsign is not local:

```pseudocode
function l4_handle_creq_inbound(frame, link_in):
    nrenv = parse_netrom_l4_envelope(frame.payload)
    dest = nrenv.dst_call

    if is_local_callsign(dest):
        # existing behaviour — deliver to bound app
        return deliver_to_local_app(frame, nrenv)

    # transit case
    sess = flex_find_route_for(dest)
    if sess is None or sess == link_in.session:
        # no route OR would loop back to source — refuse
        return emit_dm_to(link_in, nrenv.src_call)

    # allocate transit-session bookkeeping
    txn = flex_transit_session_alloc(link_in, sess, nrenv)
    if txn is None:
        return emit_dm_to(link_in, nrenv.src_call)

    # forward CREQ on the chosen out-link
    forward_envelope = clone(nrenv)
    forward_envelope.circuit_index = txn.out_circuit_index
    forward_envelope.circuit_id    = txn.out_circuit_id
    flex_emit_cf_iframe(sess.LINK, serialize(forward_envelope))
```

### 6.2 Transit Session Bookkeeping

```c
struct FLEXNET_TRANSIT_SESSION {
    BOOL  active;
    int   in_session_idx;       /* FlexNet session we received CREQ on */
    int   in_circuit_index;     /* IN as seen on the inbound */
    int   in_circuit_id;        /* ID  as seen on the inbound */
    int   out_session_idx;      /* FlexNet session we forwarded onto */
    int   out_circuit_index;    /* IN we assigned on the outbound */
    int   out_circuit_id;       /* ID we assigned on the outbound */
    char  origin_user[7];       /* preserved verbatim */
    char  origin_node[7];       /* preserved verbatim */
    char  dest_call[7];         /* the L4 destination */
    time_t last_activity;
    int   tx_seq_in, rx_seq_in;     /* current S(n)/R(n) on in-link */
    int   tx_seq_out, rx_seq_out;   /* current S(n)/R(n) on out-link */
};

#define FLEXNET_MAX_TRANSIT_SESSIONS  32
struct FLEXNET_TRANSIT_SESSION FlexNetTransitSessions[FLEXNET_MAX_TRANSIT_SESSIONS];
```

Allocation policy: linear scan for first `!active`. If full, refuse
the inbound CREQ (emit DM to the requesting peer). Idle reaping:
sessions with `last_activity > 600 s` are released regardless of
state.

### 6.3 CACK / IACK / INFO / DREQ / DACK Forwarding

The same `FlexNet_ProcessCF` (after the L4 decode runs) handles
in-flight frame forwarding:

```pseudocode
function l4_handle_inflight(frame, link_in):
    nrenv = parse_netrom_l4_envelope(frame.payload)
    txn = flex_transit_session_find(link_in, nrenv.circuit_index, nrenv.circuit_id)
    if txn is None: return forward_to_local_l4()

    # determine direction
    if link_in.session_idx == txn.in_session_idx:
        out_sess = FlexNetSessions[txn.out_session_idx]
        forward_idx = txn.out_circuit_index
        forward_id  = txn.out_circuit_id
    else:
        out_sess = FlexNetSessions[txn.in_session_idx]
        forward_idx = txn.in_circuit_index
        forward_id  = txn.in_circuit_id

    forward_env = clone(nrenv)
    forward_env.circuit_index = forward_idx
    forward_env.circuit_id    = forward_id
    # sequence numbers — see §6.4 below
    flex_emit_cf_iframe(out_sess.LINK, serialize(forward_env))

    if nrenv.opcode == DACK:
        flex_transit_session_release(txn)
    else:
        txn.last_activity = now()
```

### 6.4 Sequence-Number Translation

NetROM L4 carries `S(n)` (TX sequence) and `R(n)` (RX sequence) for
flow control. These are circuit-local — they refer to the window on
the link the frame is travelling. When we forward to the other link
we need to:

- Forward the `S(n)` from the inbound side as-is (it's the L4-end-to-
  end window, not L2). NetROM L4 windows are per-circuit, and both
  endpoints (originator and final destination) maintain them — we
  are a transit and don't translate, we just relay.
- Same for `R(n)`.

**Open question:** do we need to TRACK and TRANSLATE windows at all,
or is the L4 layer fully end-to-end? Phase 3 capture showed
sequence numbers in CREQ/IACK/I as `S(0) R(4)` etc. — the values
flow through unchanged at xnet transit. So: **forward as-is, no
translation.**

### 6.5 Circuit Index/ID Translation

The L4 circuit-index (IN) and circuit-ID (ID) ARE link-local — they
identify the circuit on the L2 link they ride on. When we forward
across, we allocate a NEW (IN, ID) pair valid on the out-link and
remember the mapping in the transit-session bookkeeping.

Allocation: each FlexNet session maintains its own pool of free
(IN, ID) pairs. On transit-session alloc, ask the out-link's pool
for a free pair. On transit-session release, return the pair to the
pool.

---

## 7. Specification — Split-Horizon (G5) and Poison-Reverse (G4)

### 7.1 Split-Horizon

Per §5.3, when assembling the candidates for advertisement to
neighbour X, iterate over OTHER neighbours' `learned[]` tables only
— never re-advertise a route back to the neighbour we learned it
from. This is a hard rule.

The OUR-OWN-DESTINATIONS branch (our own callsign, SSID range, RTT)
is unaffected — that's not learned; it's local.

### 7.2 Poison-Reverse on Neighbour Loss

When `FlexNet_HandleSessionDown(session)` fires (already exists),
add:

```pseudocode
function flex_session_down(sess):
    for route in sess.learned:
        # find any other session offering this dest at a finite cost
        backup = flex_find_other_session_with(route.dest_call, exclude=sess)
        if backup:
            # next periodic advert will pick up the cheaper finite path
            continue
        else:
            # nothing else knows the route — advertise poison
            flex_advertise_poison(route.dest_call, route.ssid_lo, route.ssid_hi)
    sess.learned_count = 0    # wipe the session's learned table
```

`flex_advertise_poison` emits a single-record compact frame with
`RTT=60000` to all OTHER active sessions immediately (don't wait for
the periodic timer — fast convergence). The peer side will see this
and update its D-table to mark our path unreachable.

Per skill §10, xnet does this AT `t=0.0 s` on link drop. We should
match: as soon as our `FlexNet_HandleSessionDown` fires, emit the
poison frames.

---

## 8. Code Changes by File

| File              | Change                                                                                                                 |
|-------------------|------------------------------------------------------------------------------------------------------------------------|
| `FlexNetCode.c`   | Add `learned[]` table per session; populate in `flex_recv_routes` parser; add `flex_periodic_advertise` timer hook; add `flex_advertise_poison`; add transit-session table; add CREQ-acceptance / forwarding hooks. Bulk of changes here. |
| `L4Code.c` (or wherever BPQ's L4 NetROM CREQ handler lives) | Add a hook for `is_local_callsign(dst) == false` case: call `flex_transit_creq_in(link, env)` and short-circuit return. |
| `L2Code.c`        | **No change.** AX.25 V2 stays untouched.                                                                                |
| `Cmd.c`           | **No change.** User-initiated `C` commands keep their current behaviour.                                                |
| `FlexNetCode.h` (if it exists) | Add `FLEXNET_LEARNED_ROUTE` and `FLEXNET_TRANSIT_SESSION` struct declarations.                            |
| `bpq32.cfg` (operator-facing) | New optional directive `FLEXNETTRANSIT YES` (default YES). Operator can disable transit-role and stay in v2.1 leaf mode. |
| `README.md`       | Document the new `FLEXNETTRANSIT` directive + the transit behaviour.                                                    |
| `ROADMAP.md`      | Promote v2.2 transit-role from "next" to "current".                                                                     |

---

## 9. Wire-Format Examples (Validated Against Phase 2/3 Captures)

### 9.1 Route Re-Advertisement — Single Record (PCF-Style Trickle)

```
linbpq-flexnet → PCF peer
I-frame on PID=CE, payload:
  hex   : 33 44 4B 30 57 55 45 30 3D 35 20 0D
  ascii : 3 D K 0 W U E 0 = 5 (sp) (CR)

  type 3 (compact records)
  callsign DK0WUE (6-char space-padded)
  ssid_lo '0' = 0
  ssid_hi '=' = 13
  RTT 5 (= 500 ms, learned 3 + link 2)
  separator + CR
```

This is byte-for-byte identical to a Phase 2 sample from xnet-14.

### 9.2 Route Re-Advertisement — Multi-Record Batch (xnet-Peer Style)

```
linbpq-flexnet → xnet peer
I-frame on PID=CE, payload (~ 42-44 bytes typical):
  type 3, then concatenated records separated by space:

  hex   : 33 44 4D 30 5A 4F 47 30 3F 31 32 30 20  ...next record...
  ascii : 3 D M 0 Z O G 0 ? 1 2 0 (sp)            ... next ...
                ↑^^^^^ DM0ZOG record: RTT 120, ssid 0-15

  Multiple records, single trailing CR. Optionally trailing '-' for
  batch-withdrawal.
```

### 9.3 Tokens

```
'3+\r' = REQUEST (please send me your routes)
'3-\r' = END-OF-BATCH (mine is done)
```

### 9.4 Poison-Reverse Record

```
linbpq-flexnet → peer (on neighbour loss)
I-frame on PID=CE, payload:
  type 3, record with RTT=60000:

  hex   : 33 49 52 35 53 20 30 3F 36 30 30 30 30 20 0D
  ascii : 3 I R 5 S (sp) 0 ? 6 0 0 0 0 (sp) (CR)
                          ssid 0-15, RTT 60000 = unreachable
```

### 9.5 Inbound CREQ Transit

The L2 view at our two FlexNet links is unchanged:

```
INBOUND  (in-link receives CREQ from far peer)
  L2: <us> ↔ <in-neighbour>      AX.25 I-frame, no digi, PID=CF
  L4: src=<originator>, dst=<target reachable downstream>, opcode=CREQ
      origin_user, origin_node embedded per skill §2.3

OUTBOUND (we forward to out-neighbour)
  L2: <us> ↔ <out-neighbour>     AX.25 I-frame, no digi, PID=CF
  L4: identical envelope to inbound except IN/ID rewritten to our
      out-link allocation
```

No L2 digi chain. No new L2 source/dest beyond the link's existing
peer pair. This is the key wire constraint that distinguishes us
from the v1.9.4 failure mode.

---

## 10. Test Plan

All testing on the IR2UFV instance first (standard workflow). The
test topology needs at least three FlexNet endpoints to demonstrate
transit:

```
                ┌─────────┐
                │ IR2UFV  │
                │ (test)  │
                └─┬─────┬─┘
        F flag    │     │    F flag
                  │     │
                  ▼     ▼
        ┌─────────┐    ┌─────────┐
        │IW2OHX-14│    │IW2OHX-12│
        │ (xnet)  │    │  (PCF)  │
        └────┬────┘    └─────────┘
             │
             ▼
        ┌─────────┐
        │IR3UHU-2 │ ← only reachable via IW2OHX-14
        └─────────┘
```

### 10.1 Tests

| ID | Description                                                                | Expected Result                                                                                                                            |
|----|----------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| T1 | IR2UFV initialises; D-table contains routes learned from both neighbours  | `D` on IR2UFV shows IR3UHU-2 (via -14), shows IW2OHX-12 (direct)                                                                            |
| T2 | IR2UFV advertises IR3UHU-2 to IW2OHX-12                                    | `D IR3UHU` on IW2OHX-12 (telnet) shows IR2UFV as a hop with `RTT = learned_RTT + IW2OHX-12_link_cost`                                       |
| T3 | A user telnets to IW2OHX-12 and issues `C IR3UHU-2`                        | CREQ arrives at IR2UFV's PID=CF dispatch on the IW2OHX-12 link → flex_transit_creq_in fires → CREQ forwarded onto IW2OHX-14 link → CACK back |
| T4 | User in T3 exchanges I-frames + DISCs cleanly                              | All IACK/INFO/DREQ/DACK forward through IR2UFV in both directions; session entry is reaped after DACK                                       |
| T5 | One IR2UFV neighbour link drops (e.g. IW2OHX-14 axudp connection breaks)  | IR2UFV emits poison-reverse (RTT=60000) for IR3UHU-2 to IW2OHX-12 within seconds; IW2OHX-12's D-table updates to "unreachable via IR2UFV"   |
| T6 | Concurrent transit sessions (10+ overlapping)                              | All complete cleanly; no IN/ID collision; transit-session table doesn't overflow                                                            |
| T7 | Split-horizon                                                              | IR2UFV does NOT advertise IR3UHU-2 *back* to IW2OHX-14 (would be a loop)                                                                    |
| T8 | Wire-byte comparison against xnet                                          | Capture IR2UFV's outbound CE compact frames toward IW2OHX-12; byte-compare against xnet-14's Phase 2 capture of the same shape              |
| T9 | Disable directive                                                          | `FLEXNETTRANSIT NO` in bpq32.cfg → IR2UFV stays in v2.1 leaf mode, no re-advertisement, no CREQ acceptance                                  |

### 10.2 Capture-Driven Validation

Re-use the Phase 2 / Phase 3 capture tooling (`xnet_monitor.py`,
`parse_hex.py`) to monitor IR2UFV's ports during T2–T8. The
acceptance bar is: **every emitted byte must match the wire shape
documented in `TRANSIT_BEHAVIOUR_REPORT.md`**.

---

## 11. Rollout Plan

1. **Implementation on IR2UFV.** Tag intermediate prototype as
   `v2.2.0-rc1` and deploy only to `/home/bpq-ufv/linbpq` on
   iw2ohx-gw. Production iw2ohx-13 stays on v2.1.9.
2. **Soak — at least 24 h.** Same workflow as v2.1.6 / v2.1.8 /
   v2.1.9. The transit-research script (`v2_1_7_soak_check.sh`)
   needs an addition that also checks IR2UFV is advertising at least
   one transit destination and that no FRMR / DM is reaching it.
3. **Live integration check.** From iw2ohx-13's BPQ console, issue
   `C IR3UHU-2 via IR2UFV-0` — or rely on routing if IR2UFV's
   advertised cost beats the direct path. Verify the session.
4. **Promote to production.** Standard deploy:
   `kill -9 -f /home/bpq/linbpq` → `sudo cp build/linbpq
   /home/bpq/linbpq` → `sudo setsid nohup …`.
5. **Tag v2.2.0 + GitHub release.** Update ROADMAP.md and
   QUICK_WINS.md "Shipped" section. Skill update (next section).

---

## 12. Skill / Documentation Updates Triggered by v2.2

After v2.2 ships:

- **flexnet-ax25-expert skill §3** — add a new sub-section on
  multi-hop transit explicitly distinguishing AX.25 V2 (1-hop) from
  NetROM L4 CREQ (multi-hop). The current skill text covers the
  1-hop case at length but only implicitly mentions the L4 case.
- **flexnet-ax25-expert skill §14** — add v2.2.0 to the version
  table.
- **README.md** — add `FLEXNETTRANSIT` directive documentation; add
  a top-level "Acting as transit node" section.
- **MEMORY.md** — update the linbpq-flexnet entry to note v2.2
  status.

---

## 13. Risks & Open Questions

### 13.1 Risks

- **R1 — Sequence-number / window misalignment.** §6.4 asserts "no
  translation, forward as-is". This is informed by xnet's observed
  behaviour but not exhaustively tested for cases where the in-link
  and out-link have different MTU / paclen. **Mitigation:** the L2
  paclen is the same (256) on all our AXIP links by config. If a
  third-party scenario ever has divergent paclen, defer it.
- **R2 — Transit-session table overflow.** 32 simultaneous transit
  sessions is generous for amateur use; if exceeded we reject new
  CREQs with DM. **Mitigation:** monitor; bump to 64 / 128 if
  warranted.
- **R3 — Loop formation.** If two linbpq-flexnet nodes mutually
  advertise routes they learned from each other, a loop forms.
  Split-horizon (§7.1) prevents the immediate-neighbour case but
  not multi-hop loops. **Mitigation:** the existing NetROM L3 TTL
  decrement protects at the L4 layer (default LT=19 in the
  envelope, decremented per transit hop, drops at 0). We rely on
  this.
- **R4 — Wire-byte deviation from xnet.** The capture-driven
  validation in T8 will catch this, but it requires getting the
  byte layout exactly right.

### 13.2 Open Questions

- **OQ1 — RTT=0 records observed in Phase 2** (e.g. DM0ZOG samples
  `[0, 49, 120, 727]` from xnet → PCF). What does RTT=0 mean? Is it
  a "newly learned, not yet measured" sentinel, distinct from
  60000 poison? Until clarified, treat RTT=0 as "skip — don't
  advertise" to be safe. Phase 4 (longer capture + active probe of
  RTT=0 destination) may clarify.
- **OQ2 — `'1n\r'` status family vs `'12\r'` specifically.** Phase 2
  saw only `"12\r"` from xnet. Are there other `1n` digits that
  could appear? linbpq-flexnet v1.9.8 has a generic classifier; no
  action needed for v2.2 unless a new digit appears in a future
  capture.
- **OQ3 — Multipath SABM behaviour observed on port 11 during
  Phase 3.** `C IW2OHX-12` triggered a SABM also on the IR3UHU-2
  link with a 3-digi chain. We don't replicate this; we forward
  single-path. Whether this hurts in some edge case is unknown.

---

## 14. Implementation Schedule

Rough estimate, assuming Marco-as-implementer with Claude assistance:

| Day  | Work                                                                                 |
|------|--------------------------------------------------------------------------------------|
| 1 AM | Data structures + `learned[]` table + parser integration                              |
| 1 PM | Periodic advertisement timer + `flex_periodic_advertise` + token state machine        |
| 2 AM | Transit-session table + CREQ acceptance hook in L4 handler                            |
| 2 PM | Inflight CF I-frame forwarding (IACK/INFO/DREQ/DACK)                                  |
| 3 AM | Poison-reverse on neighbour loss; FLEXNETTRANSIT directive parser                     |
| 3 PM | T1–T9 testing on IR2UFV; iterate on wire-byte mismatches                              |
| Day 4 | Soak; T8 wire-byte comparison; doc updates                                          |
| Day 5 | Promote to production iw2ohx-13; v2.2.0 tag; GitHub release; skill / memory updates |

Total: ~1 working week.

---

## 15. Decisions (Locked 2026-05-17)

1. **Q1 — Single 120 s cadence** for all peer families. ACCEPTED.
2. **Q2 — `FLEXNETTRANSIT YES`** by default. ACCEPTED.
3. **Q3 — Defer** factoring transit logic into the shared
   `flexnet_l3.c/h` module. ACCEPTED.

---

_RFC authored by Claude on 2026-05-17. Decisions accepted same day;
implementation reverted same day — see §16 below for lessons learned._

---

## 16. Lessons Learned — Step 2 First Attempt (2026-05-17)

Step 2 (Landings A/B/C) was implemented and deployed to IR2UFV on
2026-05-17. The implementation **proved the core mechanism works on
the wire** — xnet-14's D-table accepted IR2UFV's transit advertisement
of `IW2OHX-4` at the correct RTT (`learned + link_rtt = 1 + 2 = 3`)
with no `?` indirect marker. That's the design validation. However,
two bugs in the supporting state machine caused the IR2UFV ↔ PCF-12
link to thrash, and we reverted to v2.1.9. Production was never
touched.

### 16.1 What Went Right

- **Landing A (learned-table population)** worked transparently — the
  `flex_learned_add` hook inside `flex_dtable_merge` was invoked for
  every parsed compact record, populating per-session `learned[]`.
- **Landing B (emission with RTT arithmetic + split-horizon)** was
  byte-correct. xnet-14 accepted our transit re-advertisement and
  integrated `IW2OHX-4` into its D-table at the predicted cost.
  RFC §5 algorithm spec is right.
- **Landing C (120 s periodic timer)** fires from `FlexNet_Timer` as
  intended.

### 16.2 What Went Wrong

**Bug A — Flooding.** The initial implementation had no per-emission
cap. When `learned[]` reached ~200 entries (after fully receiving
xnet-14's D-table over ~30s of session lifetime), the next periodic
emission to PC/Flexnet-12 sent **326-403 records back-to-back**
(observed counts in console logs). PCF's L2 receive buffer saturated;
its smoothed RTT spiked to 4095 (the saturation sentinel xnet displays
in its L-table); the L2 session DISC'd; reconnect; flood again.
Vicious cycle. PCF eventually stayed stuck in INIT (refusing to
complete handshake) until we removed the offending binary.

**Bug B — Double-emission.** `last_advert` was only written in the
periodic-timer block inside `FlexNet_Timer`. The other emission paths
(KA-after-init and `3+` REQUEST) called `flex_send_own_routes`
without updating `last_advert`. Result: any emission was immediately
followed by a periodic-timer fire on the next `FlexNet_Timer` tick
(since `last_advert` was still epoch 0 → "now - last_advert > 120s"
trivially true). Visible in console as duplicate emissions back-to-
back ("+ 62 transit re-advertisements" appearing twice in a row,
then "+ 403" twice, etc.).

**Bug C — Suspected dedup mismatch.** Counts like `+ 403` ≈ 2 × 200
suggest `learned[]` was being populated with duplicate entries for
the same destination. Hypothesis: CE compact records and CF L3
D-table dumps BOTH call `flex_dtable_merge` → `flex_learned_add`,
and the SSID range encoding may differ between the two paths
(CE record uses ssid_lo/ssid_hi from compact format; CF L3 might
encode differently). Not yet confirmed — needs targeted instrumentation.

### 16.3 Fixes Applied Before Revert

A second build added: (i) per-emission cap of 8 records with a
rotating cursor across source-sessions; (ii) `last_advert` updated
inside `flex_send_own_routes` itself so all emission paths reset the
cadence. Built clean. Deployed. **But PC/Flexnet was already locked
in a bad state** from the earlier flooding (PCF refused to complete
INIT handshake even with the cap in place). Within several minutes
PCF still hadn't recovered. We chose to revert to v2.1.9 rather than
wait — production parity matters more than the test instance's
v2.2-rc state.

### 16.4 Required Changes to RFC §5 Before Step 2 Redo

1. **Make the per-emission cap part of the spec, not an
   afterthought.** Add `FLEXNET_MAX_RECORDS_PER_EMIT` to §1.12
   Protocol Constants. Default 8. Tunable by directive if needed.
2. **Add the rotating cursor mechanism.** Document it as a required
   element of §5.3 — each emission picks the next slice of learned
   destinations across all source-sessions, NOT all at once.
3. **Spell out the unified `last_advert` update.** §5.3 must state:
   `flex_send_own_routes` is the ONLY function that updates
   `last_advert`. Periodic timer, `3+` REQUEST, and KA-after-init
   all go through this single emitter, so the cadence is consistent.
4. **Add a slow-ramp safety knob.** First emission to a peer should
   carry at most 1-2 transit records. Each subsequent emission ramps
   to the cap. This protects strict peers (PCF) from being hammered
   while their L2 state is fresh.
5. **Resolve the CE/CF dedup question.** Either (a) call
   `flex_learned_add` only from the CE compact-record path (skipping
   the CF L3 D-table path), or (b) confirm both paths produce
   identical (call, ssid_lo, ssid_hi) tuples so the dedup logic
   in `flex_learned_add` catches them. Option (a) is simpler and
   probably correct — transit re-advertisement is a CE-layer
   concept.
6. **Add a kill-switch directive that DOESN'T require restart.**
   Currently `FLEXNETTRANSIT NO` is parsed once at init. Add a
   sysop console command (e.g. `FLEXNETTRANSIT OFF` at runtime)
   that flips `g_flexnet_transit_enabled` without restart. Saves
   restart-pain during testing.

### 16.5 What Was Learned About PC/Flexnet's Reception Behaviour

This is real protocol knowledge worth recording in the skill:

- **PCF tolerates back-to-back compact records at modest rate**, but
  ~50+ I-frames in <2 seconds saturates its receive path and breaks
  smoothed-RTT tracking.
- **Once PCF is in the "broken-RTT" state**, it does NOT recover
  cleanly even when the disrupting traffic stops. It needs a fresh
  L2 SABM (e.g. from a manual disconnect) to reset. Our v2.1.9
  doesn't proactively force this; PCF keeps the INIT/DISC cycle on
  its own cadence.
- **The 240-s polling cycle observed in Phase 1 captures was xnet ↔
  xnet behaviour.** xnet ↔ PCF route exchange happens on the trickle
  pattern (one record at a time, ~50 s avg gap; see Phase 1 §4.2).
  Our v2.2 implementation MUST match this — drip records, not batch.

### 16.6 Disposition

- **IR2UFV reverted to v2.1.9** at 2026-05-17 18:38 UTC.
- Production iw2ohx-13 **never modified** — remained on v2.1.9
  throughout Step 2 attempt.
- The v2.2-rc code (with cap + last_advert fix) **remains in the
  local repo** at HEAD — `g_flexnet_transit_enabled` defaults TRUE,
  but no binary with that code is currently deployed.
- Step 2 redo is parked. When resumed, start from §16.4 changes to
  the RFC, then re-implement against IR2UFV with conservative
  defaults (cap=4, slow ramp).
- The captured xnet-14 port-14 monitor (`p14_creqdebug_*_raw.txt` on
  iw2ohx-gw and locally in `research/transit_v2/`) is preserved for
  the eventual RFC §6 CREQ-forwarding work — it caught xnet's
  CREQ-to-IR2UFV which will be the test case for transit forwarding.

---

_RFC §16 lessons-learned authored by Claude on 2026-05-17 immediately
after the revert._

---

## 17. Lessons Learned — Step 2 Redo + Step 3 First Attempt (2026-05-17, later)

After the §16 lessons were captured, the cap=8 + unified `last_advert`
+ rotating cursor design was redeployed as v2.2.0-rc2. **The
implementation worked correctly at the wire-format level** —
xnet-14's `D < IR2UFV` briefly showed `IW2OHX 4-4 RTT=3` and
`IW2OHX 12-12 RTT=4` as transit destinations, byte-for-byte matching
what xnet itself emits.

Step 3 (RFC §6, CREQ forwarding) was then added as rc3 — a minimum-
viable hook in `FlexNet_ProcessCF` that decodes the L3 destination
from the CF I-frame envelope and forwards to the chosen FlexNet peer
when the dst is non-local and reachable via another active session.
The forward direction is symmetric with the return direction (CACK
back to originator), so one piece of code handles both.

The **CREQ hook code is correct in design** (verified by inspection
against Phase 3 wire captures and skill §2.3 NetROM L4 envelope spec)
**but was never proven on the wire** because we could not sustain a
test topology in which xnet would route a real CREQ through IR2UFV.
The reasons follow.

### 17.1 New Issue — Route Aging Out Faster than Our Cadence

After `ro fl del 1 iw2ohx-12` on xnet-14 (removing the direct FlexNet
route to PCF), xnet-14's D-table briefly showed `IW2OHX 12-12 RTT=4`
via IR2UFV (proof Landing B/C are correct and accepted). But within
a few minutes the entry vanished from xnet-14's `D < IR2UFV` output
and reappeared sporadically.

Mechanism: with `cap=8` + rotating cursor at the source-session
level, the budget of 8 records per emission cycle is spread across
the active source-sessions. Each cycle visits one starting session.
IW2OHX-12 lives in `FlexNetLearned[pcf_session_idx].routes[]` (added
at session init as the direct neighbour). When the cursor isn't on
the PCF session, IW2OHX-12 is NOT in the emission. With 4 sessions
(myself + 3 peers), each session is the starting cursor every 4
emissions = every 480 s. xnet's route-aging timeout appears to be
faster than that, so the route ages out before our next refresh.

**Result:** transit destinations get advertised intermittently and
the receiving xnet expires them between advertisements. The
advertisement is a slow trickle, but no single destination gets
refreshed often enough.

### 17.2 New Issue — PCF Degrades Even Under Cap=8

Across multiple cycles, the IR2UFV ↔ PCF-12 link kept degrading:
PCF's L-table showed `(2348/2) ... 600 4095` (RTT 2348 ticks, last
samples 600 and 4095 saturation), and the session uptime kept
resetting to ~1-2 minutes (frequent re-establishment). This
happened even though our outgoing rate was bounded at 8 records per
120 s = ~0.07 records/s — well below what should be sustainable.

Possible causes:
- PCF's L2 buffer is sensitive to RR/RNR pacing during bursts.
  Even cap=8 sends 8 I-frames back-to-back which may overrun PCF's
  initial state-machine window before RR catches up.
- Once PCF is in a degraded state, every subsequent emission keeps
  it broken — it can't drain its receive queue fast enough.
- Our v2.1.0 CTEXT-suppression hook may interact badly with PCF's
  state machine in the route-flood case.

What we observed in Phase 2 was: xnet's natural rate to PCF is **1
record per emission, every ~50 s** = 0.02 records/s — 3.5× gentler
than our cap=8 / 120 s. So xnet ↔ PCF rate is much slower than
what we're emitting, even with cap=8.

### 17.3 Real Design Issue — Tension Between Two Constraints

These two issues are **fundamentally in tension** under the current
cap+rotating-cursor design:

- **Constraint A:** rate to PCF must be ≤ 1 record / 50 s avg
  (matching xnet's natural rate, otherwise PCF degrades)
- **Constraint B:** every transit destination must be refreshed
  faster than xnet's route-aging timeout (otherwise xnet expires
  the route before we can use it)

With 200+ transit destinations and constraint A at 0.02 records/s,
a full cycle would take 200 × 50 s = 2.7 hours. xnet's aging is
clearly faster than that. So a "fair share" approach inevitably
fails.

### 17.4 Required Changes to RFC §5 Before Step 2 Re-Re-Attempt

1. **Per-peer cap, not uniform cap.** Different families need
   different cadences:
   - xnet peer: cap=8 every 120 s (current rc2 — works)
   - PCF peer: cap=1 every 50 s (Phase 2 observed rate)
   - linbpq-flexnet peer (future): cap=8 every 120 s (xnet-like)
   Detect by AXIP MAP flags or by INIT-time peer characteristics.

2. **Prioritise direct neighbours.** Direct neighbours (added via
   `flex_dtable_merge` at session init) are TINY in count (~3-5
   typically) and STABLE. Every emission to peer X should
   ALWAYS include all our direct neighbours (except X itself via
   split-horizon). This guarantees route freshness for the most
   important "transit shape" — multi-hop user connects across our
   downstream FlexNet links.

3. **Only emit transit advertisements in response to `3+` REQUEST**
   (xnet-style on-demand). Skill §1.6 + Phase 1 captures show
   xnet uses the `3+`/`3-` token protocol heavily for route
   exchange. Our v2.2 design pushed proactively at 120 s cadence,
   which is more aggressive than xnet itself. Switch to: emit
   own routes proactively at 120 s, but emit transit routes
   ONLY in response to `3+`. Peer asks, we answer.

4. **Implement poison-reverse on session loss.** When a downstream
   session DISCs, immediately emit RTT=60000 for routes that
   depended on it. Currently we keep stale learned[] entries that
   age out naturally — too slow.

5. **A per-peer rate limiter at the wire level**, not just per-
   emission. Even within a single emission, throttle outgoing CE
   I-frames to no more than 1 every ~5 s to PCF. xnet does this
   naturally because its emission generator is single-threaded;
   we need to enforce it explicitly.

### 17.5 Step 3 Status

The CREQ-forwarding hook is implemented in rc3 source code:
- Decodes L3 dst from offset 7 of CF payload (per skill §2.3)
- Checks dst != MYCALL base (local exemption)
- Looks up in FlexNetDests, skipping arrival session (split-horizon)
- If found and via active session: TTL-1, drop if expired, else
  `flex_send_frame(LINK, PID_CF, data, len)` to forward
- Logs via `Consoleprintf("FlexNet: transit-forwarding ...")` and
  `FlexNet_Log("CF-TRANSIT-FWD ...")`

**Never observed firing on the wire** because §17.1 and §17.2
prevented sustained route advertisement to xnet-14. When the route
is in xnet-14's D-table, no user happened to connect during the
window. When a user connected, the route had aged out and xnet
rejected the connect with "link failure".

### 17.6 Disposition

- IR2UFV reverted to v2.1.9 at 2026-05-17 21:29 UTC.
- Production iw2ohx-13 untouched throughout.
- The rc3 code (cap=8 + rotating cursor + unified last_advert +
  CREQ-forwarding hook) is at HEAD in local repo but undeployed.
- The xnet-14 route `ro fl del 1 iw2ohx-12` was issued during testing
  and may auto-restore as xnet-14 ↔ PCF exchanges routes again.
  Worth verifying.

### 17.7 Plan for Next Attempt (eventually)

Per §17.4, the v2.2 advertisement design needs a substantive rework
before retry — at minimum, items #1 (per-peer cap) and #2 (always-
include direct neighbours). The CREQ-forwarding hook in §17.5
stays. After that:

1. **Skip PCF for now.** Implement transit advertisement against
   xnet peers only. PCF compatibility is RFC §17.4 #1 work; defer
   to a v2.3.
2. **Test transit against another linbpq-flexnet node**, not xnet.
   We control both ends, so we can iterate the cap/cadence safely.
   Spin up a third linbpq-flexnet instance on iw2ohx-gw if needed.
3. **Once stable on linbpq ↔ linbpq, add xnet target.** xnet is
   tolerant of cap=8 (the rc2 verification showed this works
   against xnet-14 and xnet-4).
4. **PCF support last.** Treat it as a separate sub-project under
   §17.4 #1.

---

_RFC §17 lessons authored by Claude on 2026-05-17 after the rc2 +
rc3 revert. Step 2 redo is parked; needs more design before
re-implementation._
