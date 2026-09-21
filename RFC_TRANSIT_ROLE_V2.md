# RFC: Transit-Role v2 for linbpq-flexnet

**Status:** §5 rewritten 2026-05-18 (event-driven). §6 unchanged from
acceptance 2026-05-17. Implementation reset to v2.2-rc4 (pending).
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

linbpq-flexnet v2.1.x **re-advertises nothing** in the FlexNet mesh: it
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
- **NG6 — No transit for the AX.25-V2 1-hop case.** ~~That case
  already works correctly (skill §3.4, v1.2 + v2.1.8). This RFC
  does not change it.~~ **FALSE PREMISE — corrected 2026-09-17, see
  §13.3.** §4.1 describes us as the *originator* of the digi chain.
  Nothing had ever put us in the *middle* of one, because no peer had
  a route through us — and the moment G1 gave them one, (X)Net began
  sending us `<peer>* IR2UFV` chains to repeat and we dropped them.
  A transit node must set `DIGIFLAG=1` on its FlexNet port. This is
  not an L2 digi-chain extension (NG1 still holds — we never *add*
  digis); it is honouring a chain a peer built with us already in it.

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

> **FALSE PREMISE — measured 2026-09-17. (X)Net does not send a CREQ
> for this case.** See §13.3 "v2.2.0 GA scope". For a destination
> multiple hops beyond us, (X)Net sends the *same* AX.25 two-digi chain
> it uses for the 1-hop case and expects us to route the frame onward
> at **layer 2**. Verified against a destination we had never answered a
> path query for, so nothing we said could have misled the peer: zero
> PID=CF frames, `CF-TRANSIT-FWD` stayed 0 across every attempt. §6's
> hook may still be correct for a **BPQ/linbpq** peer, which does use
> NetROM L4 — that remains untested. It is not what (X)Net does, and
> (X)Net is what we peer with.

When **we receive** a PID=CF I-frame on a FlexNet link, decode its
NetROM L4 envelope, and the destination callsign is reachable only
via one of our other FlexNet links: this is the transit case. v2.2
handles it for the first time.

---

## 5. Specification — Route Re-Advertisement (G1) — _Event-Driven Model_

> **Spec revision 2026-05-18.** This section was rewritten end-to-end
> after the rc1/rc2/rc3 cycle. The original cap+rotating-cursor design
> is preserved in §16 and §17 as historical reference. **Sections 16
> and 17 describe what was tried and why it failed; §5 below is the
> spec future implementations must follow.**

The model is **event-driven, not periodic-batched.** The wire-level
finding from Phase 1 + 2 that we under-emphasised the first time
through: xnet emits compact records as a near-1:1 mapping of *table
mutations* in its own state, not as a clock-driven sweep. Replicating
that mapping eliminates the two structural problems of v2.2-rc2:

- PCF degrades under sustained record bursts because we ran at a rate
  higher than xnet's natural ~1 record / 50 s to PCF.
- Routes age out in xnet's D-table because a rotating cursor at the
  source-session level cycles each session every ~480 s, slower than
  xnet's per-destination ageing window.

Both go away when emission is driven by *changes*, gated by a per-peer
token bucket, with an explicit "always-keep-fresh" carve-out for the
small stable set of direct neighbours and an explicit response path
for the `3+` REQUEST token.

### 5.1 Data Model

Two parallel arrays sized by `FLEXNET_MAX_SESSIONS`, both held outside
the `FLEXNET_SESSION` struct (which is owned by `asmstrucs.h`):

```c
/* What we've LEARNED from each peer, populated by the CE compact-
   record receive path. The source of truth for transit destinations. */
struct FLEXNET_LEARNED_ROUTE {
    char    dest_call[FLEXNET_MAX_CALLSIGN];
    int     ssid_lo, ssid_hi;
    int     rtt_at_neighbour;          /* RTT this peer reported */
    time_t  last_heard;
};

struct FLEXNET_LEARNED_STATE {
    struct FLEXNET_LEARNED_ROUTE routes[FLEXNET_MAX_LEARNED_PER_NEIGHBOUR];
    int     count;
};

extern struct FLEXNET_LEARNED_STATE FlexNetLearned[FLEXNET_MAX_SESSIONS];

/* What we've TOLD each peer per destination. The source of truth for
   "do we need to send an update?". Same key space as learned[] but
   indexed by the destination-peer (the one we're advertising TO),
   not the source-peer (the one we learned FROM). */
struct FLEXNET_ADVERTISED_ROUTE {
    char    dest_call[FLEXNET_MAX_CALLSIGN];
    int     ssid_lo, ssid_hi;
    int     last_advertised_rtt;       /* what we said last to this peer */
    time_t  last_advertised_at;
};

struct FLEXNET_ADVERTISED_STATE {
    struct FLEXNET_ADVERTISED_ROUTE advs[FLEXNET_MAX_ADVERTISED_PER_PEER];
    int     count;
    /* Token bucket */
    double  tokens;                    /* current credit, 0..bucket_size */
    time_t  last_tokens_refill;
};

extern struct FLEXNET_ADVERTISED_STATE FlexNetAdvertised[FLEXNET_MAX_SESSIONS];
```

Sizes (defaults; tunable per profiling):
```
FLEXNET_MAX_LEARNED_PER_NEIGHBOUR    = 256
FLEXNET_MAX_ADVERTISED_PER_PEER      = 256
```
~36 kB per session at the upper bound, ~290 kB for 8 sessions. Easily
fits in linbpq's static heap.

### 5.2 Learned-Table Population (Inbound)

Unchanged from the rc2 implementation: `flex_dtable_merge` calls
`flex_learned_add(sess_idx, route)` for every accepted compact
record (and for the direct-neighbour entry added by
`FlexNet_InitSession`). `flex_learned_add` is idempotent by
`(dest_call, ssid_lo, ssid_hi)`.

The CRITICAL behaviour is what `flex_learned_add` does when the RTT
*changed*:

```pseudocode
function flex_learned_add(sess_idx, route):
    entry = find_or_create(FlexNetLearned[sess_idx], route)
    if entry.rtt_at_neighbour != route.rtt:
        entry.rtt_at_neighbour = route.rtt
        # CHANGE EVENT — for each OTHER peer, decide whether to advertise
        for p in 0..FLEXNET_MAX_SESSIONS:
            if p == sess_idx: continue                    # split-horizon
            if not FlexNetSessions[p].active: continue
            flex_advertise_check(p, route.dest, route.ssid_lo, route.ssid_hi)
    entry.last_heard = now()
```

### 5.3 Decision Rule (`flex_advertise_check`)

Called whenever the *expected* RTT we'd advertise to peer P for
destination D has changed. The function:

1. Computes the new expected RTT:
   `expected = learned_rtt_at_source + link_rtt_to_source_peer`
   (or 60000 if learned == infinity → poison-reverse).
2. Looks up `FlexNetAdvertised[P].advs[D]`.
3. If `|expected - last_advertised_rtt|` < `FLEXNET_REFRESH_THRESHOLD`
   (default 10 % relative, 1 tick absolute floor), skip — no update
   needed, jitter is suppressed.
4. If significant change: **queue** a single CE compact record for
   peer P with the new RTT. The queue is drained by the per-peer token
   bucket — see §5.4. Update `last_advertised_rtt` *when the queue
   actually emits the frame*, not when we queue it (so back-to-back
   changes during a quiet-bucket period collapse into one wire frame).

The decision rule fires from four sites:
- (a) `flex_learned_add` when an inbound compact record changes RTT
- (b) `flex_link_time_sample` when our smoothed RTT to a peer changes
      (since `expected` depends on `link_rtt_to_source_peer`)
- (c) `FlexNet_HandleSessionDown` for poison-reverse — see §5.7
- (d) The "direct-neighbour keepalive" timer — see §5.5

### 5.4 Per-Peer Token-Bucket Rate Limiter

Each peer session has its own bucket:

```
FlexNetAdvertised[sess_idx].tokens     = remaining capacity
FlexNetAdvertised[sess_idx].last_tokens_refill = last refill timestamp
```

Refilled at peer-family-specific rates:

| Peer family             | Refill rate         | Bucket size | Source |
|-------------------------|---------------------|-------------|--------|
| **PC/Flexnet 3.3g**     | 1 token / 5 s       | 2 tokens    | Phase 2 trickle pattern × safety margin |
| **(X)NET V1.39 / linbpq-flexnet** | 1 token / 2 s | 4 tokens | Matches xnet ↔ xnet observed inter-frame gap |

Peer family is determined at session-init time:
- `FlexNetAdvertised[sess].peer_family = PCF` if the AXIP MAP entry
  carries `F` *only* (no `B`) — that's our existing PCF marker
- `XNET_LIKE` otherwise (xnet, linbpq-flexnet, RMNC).

The token bucket is checked just before `flex_send_frame`:
```pseudocode
function flex_advertise_drain(sess_idx):
    bucket = FlexNetAdvertised[sess_idx]
    # Refill: tokens += (now - last_refill) / refill_period
    bucket.tokens = min(bucket_size, bucket.tokens +
                       (now - bucket.last_tokens_refill) / refill_period)
    bucket.last_tokens_refill = now
    while bucket.tokens >= 1 and bucket.queue is not empty:
        entry = bucket.queue.pop_front()
        flex_send_frame(LINK, PID_CE, build_record(entry))
        update entry.last_advertised_rtt
        bucket.tokens -= 1
```

`flex_advertise_drain` is called from `FlexNet_Timer` on every tick
(per-second resolution is enough); it's also called immediately after
`flex_advertise_check` queues an entry to take advantage of accumulated
quiet-period credit for a fresh change.

### 5.5 Direct-Neighbour Keepalive

The set of direct FlexNet neighbours (the calls in our AXIP MAP with
the `F` flag, populated into `FlexNetLearned[sess_idx]` at
`FlexNet_InitSession` time) is special:

- **Tiny in count** — 3 to 5 typical, never more than ~10.
- **Stable** — their RTT changes only when our link to them
  fluctuates.
- **High-value** — they're the load-bearing transit-shape carriers.
  Every CREQ for "something reachable via one of our direct
  neighbours" needs them in xnet's table.

To avoid xnet expiring them between RTT changes, the periodic timer
(120 s; reuse `FLEXNET_ADVERT_INTERVAL`) walks the direct-neighbour
set and re-queues an advertisement to each *other* peer for each
direct neighbour, even when no change has happened. These ride the
same token bucket as change-driven events — they just keep showing up
on a regular cadence as a fallback.

Direct-neighbour entries are identified by a flag on the
`FLEXNET_LEARNED_ROUTE`:
```c
struct FLEXNET_LEARNED_ROUTE {
    ... existing fields ...
    BOOL is_direct_neighbour;       /* set at init, never cleared */
};
```

`FlexNet_InitSession` sets the flag on the record it adds via
`flex_learned_add` before any CE compact-record path can run.

### 5.6 `3+` REQUEST Handling

`3+` is the xnet-style "give me your full current view" REQUEST per
skill §1.6. When received from peer P:

1. Walk `FlexNetLearned[s]` for all s ≠ P (split-horizon).
2. For each entry, compute the expected RTT and queue it on P's
   bucket (using the same `flex_advertise_check` path so jitter
   suppression still applies — `3+` is not a "force-send-everything",
   it's "force-the-decision-rule-to-walk-everything").
3. Queue a trailing `3-` END-OF-BATCH token after the last record.
4. The token bucket drains records at the peer's natural rate; the
   `3-` is queued *after* all records, so it's emitted last.

This makes `3+` a heavy operation (potentially 200+ queued items at
the bucket rate) — but it's bounded by the bucket and triggered by
the peer's own decision, so we don't overwhelm anyone.

### 5.7 Poison-Reverse on Session Loss

When `FlexNet_HandleSessionDown(sess_y)` fires:

1. Walk `FlexNetLearned[sess_y]`. For each entry that *isn't* a direct
   neighbour, look for an alternate session offering the same dest.
   - If a viable alternate exists: do nothing here — the next change-
     event from that session will re-advertise the better path naturally.
   - If no alternate: queue an RTT=60000 (poison) advertisement to all
     OTHER peers via `flex_advertise_check`.
2. Clear `FlexNetLearned[sess_y]` after walking.
3. The poison advertisements drain through each peer's token bucket
   at the per-family rate. PCF gets poison at 5 s/record; xnet peers
   at 2 s/record.

Receiving peers see RTT=60000 and update their D-tables to reflect
us no longer being the path. Skill §10 confirms xnet does this at
`t=0.0 s` on link drop — our delay budget is the bucket drain time,
typically a few seconds.

### 5.8 Wire Format Output

Unchanged from the rc2 implementation; carried forward from the
historical §5.4 (still validated against Phase 2 captures):

```
'3' CALL(6) SSID_LO_CHAR(1) SSID_HI_CHAR(1) RTT_DECIMAL ' ' '\r'
```

One record per CE I-frame in the trickle direction (PCF). Multi-record
batches are NOT used by this spec — even on xnet peers, the
event-driven model produces single records most of the time. The only
multi-record case is the `3+` REQUEST response, and even there the
token-bucket drain spreads the records across multiple I-frames.

**No `?` indirect prefix.** Ever. (Phase 2 confirmed xnet does not use
it on transit re-advertisements.)

### 5.9 Constants (Summary)

```c
#define FLEXNET_MAX_LEARNED_PER_NEIGHBOUR  256
#define FLEXNET_MAX_ADVERTISED_PER_PEER    256
#define FLEXNET_REFRESH_THRESHOLD_PCT      10   /* relative jitter floor */
#define FLEXNET_REFRESH_THRESHOLD_ABS      1    /* absolute tick floor */
#define FLEXNET_ADVERT_INTERVAL            120  /* direct-neighbour keepalive */
#define FLEXNET_BUCKET_REFILL_PCF_S        5    /* 1 record / 5s to PCF */
#define FLEXNET_BUCKET_REFILL_XNET_S       2    /* 1 record / 2s to xnet */
#define FLEXNET_BUCKET_SIZE_PCF            2
#define FLEXNET_BUCKET_SIZE_XNET           4
#define FLEXNET_RTT_INFINITY               60000
```

### 5.10 Worked Example — A Single Change

Scenario: IR2UFV has 3 peers (xnet-14, xnet-4, PCF-12). Steady-state
for ~10 minutes. Now IR3UHU-2 (reachable through xnet-14) suddenly
becomes unreachable from xnet-14's view — xnet-14 advertises IR3UHU-2
with RTT=60000 to IR2UFV.

Step-by-step under the event-driven model:
1. IR2UFV's CE compact-record handler receives `IR3UHU2 0 ? 60000\r`
   on the xnet-14 session.
2. `flex_dtable_merge` updates `FlexNetDests` and calls
   `flex_learned_add(sess_for_-14, route)`.
3. The RTT changed (from ~6 to 60000) — `flex_advertise_check` fires
   for the other two peers (xnet-4 and PCF-12).
4. Each peer's `flex_advertise_check`:
   - xnet-4: queues `IR3UHU2 0 ? 60000\r` on xnet-4's bucket.
   - PCF-12: queues the same on PCF-12's bucket.
5. xnet-4's bucket (`XNET_LIKE`, 2 s refill, 4 token cap) is full;
   drains the 1 queued record immediately → record on the wire to -4.
6. PCF-12's bucket (`PCF`, 5 s refill, 2 token cap) is also full;
   drains the 1 queued record immediately → record on the wire to -12.
7. `FlexNetAdvertised[xnet-4].advs[IR3UHU-2].last_advertised_rtt =
   60000`; same for PCF-12.

Total wire emission: 2 CE compact frames in the same second, one per
other peer, both correctly carrying the poison RTT.

Now if xnet-14 sends an update 1 s later (suppose IR3UHU-2 comes back
at RTT=8): `flex_advertise_check` fires again, two more records get
queued. Buckets are nearly empty now — xnet-4 has 1 token (after the
refill from the second-old emission), PCF-12 has 0.2 tokens. xnet-4
drains immediately; PCF-12 waits ~4 s before draining. No flooding
even on a rapid back-and-forth.

---

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
| `bpq32.cfg` (operator-facing) | New optional directive `FLEXNETTRANSIT YES|NO` (**default NO** — §15 Q2 as superseded 2026-09-14). A node opts *into* transit; with no directive it keeps v2.1 behaviour and re-advertises nothing. |
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

**Amended 2026-06-05 (IR3UFV dropped) and 2026-06-07 (offline unit
tests dropped).** Testing uses **only IR2UFV plus its existing
direct FlexNet neighbours** (IW2OHX-14 + IW2OHX-4 xnet, IW2OHX-12
PCF). The previously-proposed offline unit-test harness was right-
sized away — IR2UFV is already a controlled test bed, natural xnet
+ PCF traffic injects enough state-machine events to exercise the
§5 logic, and `FlexNet_Info` log lines built into the
implementation make decisions observable in real time. The trade-
off is that re-tuning bucket parameters means re-soaking IR2UFV
(~30 min) rather than running a sub-second test binary; given how
rarely these parameters move, this is acceptable.

```
                                  ┌─────────┐
                                  │IW2OHX-13│   (production, separate)
                                  │ (prod)  │     transit DISABLED — own dests only
                                  └─────────┘
                                  
                            ┌────────────────┐
                            │     IR2UFV     │   test bed for v2.2
                            │  v2.2 transit  │   FLEXNETTRANSIT=YES
                            │    enabled     │
                            └────┬───┬───┬───┘
                                 │   │   │
                          F flag │   │   │ F flag
                                 ▼   ▼   ▼
                          ┌─────┐ ┌─────┐ ┌─────┐
                          │xnet │ │xnet │ │ PCF │
                          │ -14 │ │ -4  │ │ -12 │
                          └─────┘ └─────┘ └─────┘
```

IR2UFV plays both roles in the test: it is the **transit subject
under test** (its `FlexNetAdvertised[]` evolution drives the
outbound traffic we observe) and the **source of truth** for what
gets re-advertised between peer pairs.

### 10.1 Phase 1 — IR2UFV behavioural validation

Phase 1 was originally "linbpq ↔ linbpq only" against a controlled
IR3UFV peer. With IR3UFV dropped and the unit-test harness right-
sized away, Phase 1 is now a single behavioural-observation pass on
IR2UFV's real peer links, made trustworthy by embedded
`FlexNet_Info` log lines at each §5 decision point.

#### 10.1.a Observability built into the implementation

Before any test runs, the §5 implementation must emit four
diagnostic log lines (gated by `FLEXNET_PROD` so they vanish in
production builds — see v2.1.38 mechanism):

```c
FlexNet_Info("FlexNet: ADVERT-CHECK peer=%s dest=%s exp=%d "
             "last=%d delta=%d %s",
             peer_call, dest_call, expected_rtt,
             last_advertised_rtt, delta,
             fired ? "FIRED" : "SUPPRESSED");

FlexNet_Info("FlexNet: BUCKET peer=%s tokens=%.2f queue=%d "
             "emit=%d family=%s",
             peer_call, bucket.tokens, queued, emitted,
             is_pcf ? "PCF" : "xnet");

FlexNet_Info("FlexNet: POISON peer-down=%s dest=%s alt=%s",
             dead_peer, dest_call,
             alternate_peer ? alternate_peer : "(none, poisoning)");

FlexNet_Info("FlexNet: 3PLUS-WALK from=%s entries=%d queued=%d",
             requesting_peer, learned_count, queued_count);
```

These four lines are the substitute for unit-test assertions: every
state-machine transition emits a structured log line that an
operator (or future Claude) can grep, diff, and reason about.

#### 10.1.b Behavioural soak (30 min)

Deploy IR2UFV with `FLEXNETTRANSIT=YES` and the dev build
(`FLEXNET_DEBUG=1`, no `FLEXNET_PROD`). Run a 30-min eth0 capture
on UDP/10075 covering all three peer links. The acceptance tests:

| ID  | Description                                                                                       | Expected Result                                                                                                                            |
|-----|---------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| B1  | IR2UFV brings up sessions with each of its three peers, populates `FlexNetLearned[]`              | `FL` shows IW2OHX-14, IW2OHX-4, IW2OHX-12 as CONNECTED with `learned[]` populated by `FlexNet_InitSession`                                |
| B2  | Init-time self-advertisement emits 1 self-record per peer + 1 record per direct neighbour         | First minute of the eth0 pcap shows the expected `'3'`-prefixed compact records on each peer port                                          |
| B3  | Direct-neighbour 120 s keepalive cadence                                                          | Over 30 min: each peer port shows IR2UFV re-emitting a record per direct neighbour every 120 s ± token-bucket jitter                       |
| B4  | Wire-byte equivalence to xnet's own emissions                                                     | Diff IR2UFV's `mo -i`-equivalent capture against xnet-14's `mo -i` for the same compact-record shape; all bytes match per skill §1.6      |
| B5  | Token-bucket cadence is respected per-peer-family                                                 | IW2OHX-12 (PCF) port shows ≤ 1 record / 5 s under any burst; IW2OHX-14/-4 ports show ≤ 1 record / 2 s. Log lines `BUCKET … family=PCF/xnet` confirm the gating logic |
| B6  | Split-horizon visible on the wire                                                                 | A route learned from IW2OHX-14 never appears in the outbound stream addressed to IW2OHX-14 (verify by INFO byte inspection). Log lines `ADVERT-CHECK … SUPPRESSED` with peer = source show split-horizon firing |
| B7  | Jitter suppression is firing under natural traffic                                                | Grep log for `ADVERT-CHECK … SUPPRESSED` vs `… FIRED`; under natural xnet route drift the SUPPRESSED:FIRED ratio should be ≥ 3:1 (most small RTT wiggles are below the 10 %/1-tick threshold) |
| B8  | RTT=0 records skip-and-count (OQ1)                                                                | Grep log for any `ADVERT-CHECK` with `exp=0` followed by FIRED — should be zero (the skip rule applies before the check). Counter visible via `FL` |

#### 10.1.c Targeted-disruption tests

Three behaviours that natural traffic doesn't reliably exercise.
Run after B1-B8 are clean:

| ID  | Description                                                                                       | Mechanism                                                                                                                                  |
|-----|---------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| D1  | `3+` REQUEST response (live)                                                                     | Trigger from a real peer: re-handshake IR2UFV's session to one peer (kill IR2UFV-side session via SYS console, let it re-establish). Verify `3PLUS-WALK from=PEER entries=N queued=M` appears in the log; pcap shows trailing `3-` after the last record |
| D2  | Poison-reverse on session loss (live)                                                            | `sudo iptables -A OUTPUT -d 192.168.1.201 -p udp --dport 10093 -j DROP` for 60 s. Verify `POISON peer-down=IW2OHX-12 dest=… alt=(none, poisoning)` appears in the log; pcap shows RTT=60000 records to the other two peers within bucket-drain time. Undo with `iptables -D` to restore |
| D3  | Poison-NOT-issued when alternate path exists                                                     | Same as D2 but for a destination known to be reachable via two peers (e.g., a node both IW2OHX-14 and IW2OHX-4 advertise). Verify `POISON peer-down=… alt=IW2OHX-N` appears (no poison emitted, just the learned[] clear) |

### 10.2 Phase 2 Tests — Add xnet (IW2OHX-14, IW2OHX-4)

Builds confidence that the event-driven rate (1 / 2 s, bucket=4) is
acceptable to (X)NET V1.39.

| ID  | Description                                                                                       | Expected Result                                                                                                                            |
|-----|---------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| T20 | IR2UFV (now v2.2-rc4) sustains its xnet-14 and xnet-4 sessions over a 1-hour soak                | `L` on xnet-14 shows IR2UFV's Q/T values stable (Q ≤ 4, rtt 2-4), no DISC events                                                          |
| T21 | xnet-14's `D < IR2UFV` populates and stays fresh                                                  | Continuous capture over 30 min → IR2UFV's transit destinations remain visible in `D < IR2UFV` (with the direct-neighbour 120s keepalive keeping them alive) |
| T22 | RTT-change propagation latency                                                                    | Force a learned-RTT change on IR2UFV → corresponding update appears in xnet-14's D-table within bucket-drain + xnet's L3 propagation     |
| T23 | Wire-byte equivalence to xnet's own emissions                                                     | Diff IR2UFV's `mo -i` capture against xnet-14's `mo -i` capture for the same compact-record shape; all bytes match per skill §1.6        |

### 10.3 Phase 3 Tests — Add PCF (IW2OHX-12)

The conservative phase. Validates the PCF-specific bucket rate (1 / 5 s,
bucket=2) is gentle enough to keep PCF stable indefinitely.

| ID  | Description                                                                                       | Expected Result                                                                                                                            |
|-----|---------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| T30 | IR2UFV ↔ PCF-12 session stays CONNECTED through a 24-hour soak                                   | PCF's `l *` shows IR2UFV link with stable RTT 2-4 ticks, no RTT-saturation (4095), no reconnect events                                    |
| T31 | PCF accepts gradual advertisement of the full IR2UFV transit table                                | After ~30 min, PCF's L-table contains IR2UFV's transit destinations at the expected `learned + link_rtt` RTTs                            |
| T32 | Burst-then-quiet behaviour                                                                       | Generate a 20-record event burst on IR2UFV → only 2 records reach PCF in the first second (bucket size); remaining 18 over ~90 s        |

### 10.4 CREQ Forwarding Tests (§6 — unchanged from rc3)

These exercise the L4 transit hook. Require a destination only
reachable via a transit hop (force via `ro fl del` on the receiving
xnet — see §17.6 for the technique used in the rc3 attempt).

| ID  | Description                                                                                       | Expected Result                                                                                                                            |
|-----|---------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| T40 | CREQ arrives at IR2UFV addressed to a downstream-only destination → forwarded at L2 to the right next-hop peer | `CF-TRANSIT-FWD` line appears in IR2UFV's log with correct in/out links and TTL-1                                                          |
| T41 | CACK return through the same node                                                                | The reverse-direction CREQ-FWD also fires for CACK → connect completes; user session functional                                            |
| T42 | TTL expires mid-transit                                                                          | Inject a CREQ with TTL=1 to IR2UFV → IR2UFV drops the frame with `CF-TRANSIT-TTL-EXPIRED`; no infinite-loop scenario observed              |
| T43 | Destination not reachable via any peer                                                            | Forward not attempted; frame falls through to the existing NetROM L3/L4 path (returns 0 from `FlexNet_ProcessCF`)                          |

### 10.5 Operational / Sysop Tests

| ID  | Description                                                                                       | Expected Result                                                                                                                            |
|-----|---------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| T50 | `FLEXNETTRANSIT NO` in bpq32.cfg                                                                  | linbpq keeps v2.1.9 behaviour — no re-advertisement to peers, CREQ forwarding hook short-circuits via `g_flexnet_transit_enabled` check  |
| T51 | Per-peer rate-limit override via directive (future work)                                          | Documented in §13 as deferred — not in v2.2.0 GA                                                                                          |

### 10.6 Capture-Driven Validation

Reuse the Phase 2 / Phase 3 capture tooling (`xnet_monitor.py`,
`parse_hex.py`) to monitor each test pass. The acceptance bar is:
**every emitted byte must match the wire shape documented in
`TRANSIT_BEHAVIOUR_REPORT.md`** AND **token-bucket inter-frame gaps
must respect the per-peer family rate**.

Add a new analyser (`parse_advertise.py`) that consumes a capture and
reports per-peer emission rates, jitter-suppression effectiveness
(percentage of changes that DIDN'T fire an emission), and direct-
neighbour-keepalive cadence. This is the data we need to validate
the §5.4 parameters and tune them per real-world peer.

---

## 11. Rollout Plan

**SUPERSEDED 2026-09-21 — production IW2OHX-13 was promoted to a full
FlexNet router**, aligned with IR2UFV (`FLEXNETTRANSIT`,
`FLEXNETL2TRANSIT`, `FLEXNETPATHFORWARD`, `FLEXNETLT3BYTE` all `YES`,
`DIGIFLAG=1`), on Marco's explicit decision. `FLEXNETSSIDRANGE` stays
`13-13` and must not be aligned to IR2UFV's `0-8`: `IW2OHX-13` shares its
base call with other live nodes on the mesh.

The premise below — "it doesn't have a FlexNet peer that benefits from
transit advertisement … its FlexNet peers are themselves xnet, which
already act as transit" — **was wrong on the facts.** `IW2OHX-4` has no
direct FlexNet link to `IW2OHX-14`; its only paths were via `IW2OHX-12`
(rtt 196) and via `IW2OHX-13` (rtt 3). Production was the *cheap* path the
whole time and its neighbours had no way to use it. Within a minute of the
promotion `-4`'s destinations via us went **1 → 71** and via `-12`
**187 → 134**, and pinned connects 1, 3 and 4 hops beyond us all succeeded.

**The general lesson, worth more than the decision:** transit being inert
on the test bed says nothing about a production node. On IR2UFV transit
never fired because it ties or loses on cost everywhere and (X)Net keeps
the incumbent on a tie — its forwarding counters never left `0/0/0`. Check
whether the candidate node is cheap or expensive *relative to the incumbent
path at each neighbour* before predicting the effect.

**Amended 2026-06-05 (historical).** Production iw2ohx-13 stays on the
v2.1.x non-forwarding path indefinitely — it doesn't have a FlexNet peer
that benefits from transit advertisement (its FlexNet peers are
themselves xnet, which already act as transit). All v2.2 testing
happens on IR2UFV.

1. **Implement §5 with built-in observability.** The four
   `FlexNet_Info` log lines from §10.1.a are part of the
   implementation, not added later. They are the regression net —
   every state-machine transition emits a greppable line.
2. **Deploy to IR2UFV with transit off.** From rc4 D1 the compiled
   default *is* off (§15 Q2, superseded 2026-09-14), and IR2UFV's cfg
   carries an explicit `FLEXNETTRANSIT OFF` at line 376 as well — so the
   v2.2 code path ships compiled-in but inactive with no config edit at
   all. Verify L2 sessions to IW2OHX-14, IW2OHX-4, IW2OHX-12 stay
   healthy (same baseline as v2.1.38). Also confirm the default itself:
   a build with **no** directive in cfg must emit only the self record
   — the 2026-09-14 before/after capture recipe is the check
   (`udp port 10093 and (dst host <peer>)`, ~150 s ≥ one
   `FLEXNET_ADVERT_INTERVAL`, expect 0 non-self `3<CALL>` records).
3. **Flip `FLEXNETTRANSIT=YES` on IR2UFV.** Start with the
   conservative PCF rate (1 token / 5 s, bucket=2) applied to *all*
   peers initially. Run §10.1.b behavioural soak B1-B8 for 30 min.
4. **Widen xnet-family rate.** If B1-B8 clean, restore the xnet_like
   rate (1 token / 2 s, bucket=4) for IW2OHX-14 and IW2OHX-4 only.
   PCF stays on the conservative rate. Soak another 30 min.
5. **Targeted-disruption tests (§10.1.c D1-D3)** once steady-state
   is confirmed — `3+` response, poison-reverse with no alternate,
   poison-not-issued when an alternate path exists.
6. **24 h PCF soak (Phase 3).** §10.3 T30-T32 unchanged. This is
   the hard gate — PCF's L2 link must stay CONNECTED with cost
   stable through the full window. Have v2.1.38 rollback ready
   (`/home/bpq-ufv/linbpq.pre-v2.1.38-2026-06-03`).
7. **CREQ-forwarding tests (§10.4 T40-T43)** against the real
   topology — find a destination reachable only via one of IR2UFV's
   three peers, exercise the transit path.
8. **Tag v2.2.0 + GitHub release.** Update README.md (transit-mode
   section), ROADMAP.md, MEMORY.md. Skill update (§12).
9. **Do NOT promote to production iw2ohx-13.** Production stays non-
   only. If a future operational need for transit on iw2ohx-13 arises
   (e.g. an asymmetric peer wiring that benefits from it), reassess
   with a separate RFC; for now there's no payoff and only risk.

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
- **R5 — Token-bucket parameters tested live against PCF first
  (2026-06-05, refined 2026-06-07).** With IR3UFV removed from the
  test topology (§10) and the offline unit-test harness dropped
  (2026-06-07), the first exercise of the bucket math is against
  real peers — most critically PCF. If a bug in
  `flex_advertise_drain` produces a sustained over-emission to PCF,
  we'll cycle the L2 link (per the v2.1.30 experiment, 10-15 ms DM
  on unsolicited record). **Mitigation:** §11 rollout step 3
  applies the PCF rate (1/5 s, bucket=2) to ALL peers initially —
  even xnet — so a bucket bug can only over-emit by a factor of
  ~2.5 vs xnet's native rate, not by an order of magnitude. The
  `BUCKET peer=… tokens=… emit=…` log line emits per drain cycle,
  so over-emission is visible within seconds. Soak B5 explicitly
  measures wire-level inter-frame gaps per peer.
- **R6 — Less coverage of split-horizon / loop edge cases.** A
  controlled second linbpq peer would let us construct adversarial
  topologies (mutual-advertise cycles, multi-hop loops); the real
  topology is sparse. **Mitigation:** split-horizon is observable
  on the wire under natural traffic (test B6), and the
  `ADVERT-CHECK peer=… SUPPRESSED` log line shows the rule firing
  per-event. Loop protection at L4 is via NetROM TTL decrement
  (R3 mitigation) which we don't change.
- **R7 — Live-only `3+` and poison-reverse coverage (D1-D3).**
  These behaviours are tested by manipulating real peer state
  (iptables firewalling, session re-handshake). Less reproducible
  than synthetic injection; if a regression appears post-merge,
  the `POISON` and `3PLUS-WALK` log lines in §10.1.a are the
  first diagnostic. **Mitigation:** D2's iptables technique is
  well-bounded (single rule, easy revert). D1 falls back to
  session re-handshake (which we already exercise routinely on
  every IR2UFV restart). D3 validates the alternate-path branch.
- **R8 — No sub-second regression net for future parameter
  retuning (2026-06-07).** Dropping the unit-test harness means
  that if a future change wants to retune the jitter threshold or
  bucket parameters, the validation cost is a 30-min IR2UFV
  re-soak rather than a `make unit-tests` invocation.
  **Mitigation:** the §5 parameters have moved 4 times in 6 weeks
  in the v2.1.x line; the 30-min soak fits comfortably within
  that cadence. If parameter churn ever increases, the harness
  can be added later — the implementation's pure-function shape
  is harness-friendly by construction.

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

## 13.3 Implementation Record — rc4 D1-D3 landed 2026-09-17

Code for §5 is in `main` as `FLEXNET_VERSION_STR = "v2.2.0-rc4"`.
Builds clean on iw2ohx-gw; measured with `-Wall -Wextra -Wshadow`
`FlexNetCode.c` is at **55** warnings against a 56 pre-existing
baseline (one pre-existing unused-parameter removed, none added).

What landed, against the D1-D3 rows of §14:

| Spec | Implementation |
|---|---|
| §5.1 `FlexNetAdvertised[]` | `FLEXNET_ADVERTISED_ROUTE` / `_STATE`, parallel to `FlexNetSessions[]`. advs[] doubles as the pending queue (`pending` + `pending_rtt`) rather than a separate allocation, so repeated changes to one destination collapse in place while the bucket is dry. |
| §5.2 trigger (a) | `flex_learned_add` — both the RTT-change path and the new-entry path (a new destination is a change from nothing, which the spec's pseudocode left implicit). |
| §5.3 decision rule | `flex_advertise_check()`; `flex_expected_rtt()` computes `min(learned_rtt + link_rtt)` over all sessions except the target, so **split-horizon is structural** — no caller can omit it. |
| §5.3 trigger (b) | `flex_link_time_sample`, gated on ≥ 1 wire tick from `FlexNetLearned[].lt_anchor` (§15 Q6). |
| §5.4 token bucket | `flex_advertise_drain()`, called per `FlexNet_Timer` tick and again immediately after any check queues. |
| §5.5 keepalive | `flex_advertise_neighbours()` on the existing 120 s tick, `force=TRUE` (bypasses the jitter floor only). Also runs once when a peer first comes up, which is test B2. |
| §5.6 `3+` | `flex_advertise_walk_for_peer(direct_only=FALSE, force=FALSE)` + `eob_pending`; `flex_send_own_routes` gained a `defer_eob` argument so exactly one `3-` goes out, after the queue drains. |
| §5.7 poison-reverse | In `FlexNet_CloseSession`. The session is deactivated *before* the walk, which is what makes `flex_expected_rtt`'s alternate-path check see the network as it is rather than as it was. |
| §5.9 constants | All present, values as specified. |
| §15 Q4 | `advs[]` overflow rejects + warns once per peer via `Consoleprintf` (unconditional — it is an operator action item, not a trace line). |
| §10.1.a log lines | ADVERT-CHECK / BUCKET / POISON / 3PLUS-WALK all emit, plus NBR-REFRESH. Gated on `FLEXNET_DEBUG` so a `flexdebug` build carries them and a production build does not. |
| §10.1.b observability | `FL` gained a transit section: per peer family, learned / direct / advertised counts, queue depth, token credit, and the RTT=0 skip counter for B8. |

**Two deliberate deviations from the spec text**, both to be reviewed
before the tag:

1. **Peer-family detection uses the keepalive shape, not the AXIP MAP
   `B` flag** (§5.4). PC/Flexnet emits a 201-byte KA terminated with
   CR, (X)Net a 241-byte KA ending in a space — captured on the
   IR2UFV↔IW2OHX-12 link 2026-05-25, and already what the KA cadence
   and the old per-emit cap key off. That is protocol evidence; the
   MAP flag is a local config convention, and a MAP line edited to add
   `B` to a PCF peer would silently mis-size its bucket. Cost: the
   family is unknown until the peer's first KA, so we answer PCF while
   unknown — the slower, safer bucket.
2. **The lost peer's own entry is poisoned too** (§5.7 step 1 says
   "each entry that *isn't* a direct neighbour"). A direct neighbour
   is advertised to the other peers as `learned_rtt + link_rtt` like
   any destination, so leaving it out would leave them routing to a
   dead node through us. `flex_expected_rtt` still suppresses the
   poison when the same call is reachable via a surviving session.

**Third deviation, found by the first soak — a peer that comes up late
is seeded with the full view, not just the direct set.** Trigger (a)
fires on *change*, so a peer joining a node whose table has already
converged sees nothing. Measured on IR2UFV: the PCF peer re-established
after the two xnet sessions had populated ~190 destinations and its
`advertised[]` sat at **2** (the two direct neighbours) while the xnet
peers were at 119 and 126. PC/Flexnet does not send `3+`, so it would
have stayed there — transit toward the one peer family the whole rc4
redesign exists to protect would have been silently dead. §10.1.b B2
reads as "self + one record per direct neighbour" only because it
assumes a cold start, where the full view *is* the direct set; that row
needs restating as "the full learned view, which at cold start is the
direct set". The seed walk uses `force=FALSE` (a fresh session's
`advertised[]` is empty, so every entry fires on the never-advertised
sentinel anyway) and drains at the peer's family rate — ~16 min for a
190-entry table to a PCF peer, which is the bucket working, not a
burst. **This is the sustained 1 record / 5 s that Phase 3's 24 h soak
has to judge**: §14 already flags it as 10× xnet's natural 1 / 50 s to
PCF, and the seed is where that margin gets exercised hardest.

**Also fixed in passing:** a fresh session slot now clears its
`FlexNetLearned[]` / `FlexNetAdvertised[]` rows. The session reaper can
free a slot without going through `FlexNet_CloseSession`, and a new
peer inheriting the previous occupant's `advertised[]` would be told
nothing — we would believe it already knew routes it had never heard.

### First IR2UFV measurements (2026-09-17, transit=YES, flexdebug)

85 s window on the AXIP port, analysed with the new
`tools/parse_advertise.py`:

| Test | Result |
|---|---|
| **B5 — bucket cadence** | **PASS, on the wire.** Inter-record gap median **4.98 s** to the PCF peer (period 5 s) and **2.02 / 2.03 s** to the two xnet peers (period 2 s). Sub-period gaps are the burst allowance draining: 2 for PCF (burst 2), 8 and 3 for the xnet peers (burst 4). The console shows the same thing from the other side — PCF refills 0.20 tokens/s and emits one record per 5 s, xnet 0.50/s and one per 2 s, each peer independently. |
| **B6 — split-horizon** | **PASS.** Zero destinations advertised back to the only peer that could have taught them to us. |
| **B8 — RTT=0 skip** | **PASS.** Zero `ADVERT-CHECK … exp=0 … FIRED`. The skip happens in `flex_dtable_merge` before `learned[]`, so such a record can never reach the decision rule; `FL` carries the counter. |
| **B1/B2** | PASS. Three sessions up, `learned[]` populated, self-record plus the transit view emitted per peer. |
| **B3 — 120 s cadence** | Fires per peer, cadence not yet measured to the second (see the timestamp note below). |
| **B7 — jitter ratio** | **Not measurable as written — the test needs restating.** |
| D1-D3 disruption | Not yet run. No session loss occurred, so `POISON` has never fired. |

**B7's 3:1 target rests on an assumption this network contradicts.**
Measured on a converged window (last 150 decisions, xnet peers at
queue 0): 54 first-ever, 12 forced refresh, leaving **84 genuine jitter
decisions — 30 SUPPRESSED, 54 FIRED**, i.e. 0.56:1, not 3:1.

The suppression logic is working: every `delta=0` on a stable route is
suppressed, exactly as designed. The reason FIRED still dominates is
that the FIRED cases are **not jitter** — they are large, real changes:
`IQ2LB-0/5 exp=37 last=50 delta=13`, `IW8PGT-14 exp=8 last=16 delta=8`.
Learned RTTs to distant destinations on this network swing by 30-50 %,
far above a 10 % floor. B7 implicitly assumed a network whose RTTs
mostly wiggle by a few percent; ours does not.

**Consequence worth stating plainly: the token bucket, not the jitter
threshold, is what bounds emission here.** That is the right way round
— the bucket is a hard per-peer rate ceiling regardless of how
volatile the table is, and PCF safety should not depend on the network
happening to be quiet. Raising `FLEXNET_REFRESH_THRESHOLD_PCT` to cut
traffic would need ~50 % to bite on these deltas, which would start
discarding real routing information. **Recommendation: leave the
threshold at 10 % / 1 tick** and treat B7 as a descriptive measurement
rather than a pass/fail gate.

**B7 as written also cannot be run on a cold start.** It asks for ≥ 3:1;
the measured run gave **1 : 280**. That is not a failure of the jitter
floor, it is a cold start: 261 of those 280 were a destination's first
advertisement (`last=-1`, the never-advertised sentinel, which always
fires by design) and 19 were the forced 120 s neighbour refresh. Only
the remainder are jitter decisions at all. The ratio B7 describes is a
*steady-state* property — it appears once the table has converged and
the traffic is xnet re-sending destinations whose RTT has not moved.
Convergence to the PCF peer takes ~16 min for a ~190-entry table at
1 record / 5 s, so **B7 must be measured on a window that starts after
convergence**, and the row should say so. Measuring it over a cold
start will always fail, and would invite "fixing" a threshold that is
working correctly.

**Console lines needed a time base.** The §10.1.a lines went only to
`Consoleprintf`, which carries no timestamp, so B3's cadence and B7's
ratio-over-a-window could not be measured from the log at all. They now
also go through `FlexNet_Log` (already timestamped, `/tmp/flexnet_axudp.log`)
via a `FlexNet_Trace` macro. Both halves stay `FLEXNET_DEBUG`-gated.

### G1 is verified from the peers' own routing tables

The observation rc1-rc3 never produced. `D < IR2UFV` on an (X)Net peer
lists the destinations **it** reaches via IR2UFV — its own routing
decision, not our account of what we sent:

- **IW2OHX-4: ~110 destinations via IR2UFV** — DB0\*, DK0WUE, PE1\*,
  PI\*, SV1\*, HB9\*, OK0NAG, F3KT, VA3\*, VE3\*, K\*/N\*/W\*, the IGATE
  pair — the world IR2UFV learned from -14, plus `IW2OHX 14-14 3`, the
  re-advertised direct neighbour, and `IR2UFV 0-8 1` with its SSID range
  intact.
- **IW2OHX-14: 4 rows** — `IR2UFV 0-8 1`, `IW2OHX 4-4 3`, two HB9AK.

The asymmetry is the mechanism working, not a fault: split-horizon sends
each peer only what the *other* taught us, and -14 is the hub that
already holds a better path to everything it gave us, so our records
lose on cost there and win at -4. **IR2UFV is now a real transit path
between IW2OHX-4 and the wider FlexNet world.**

**No `?` indirect prefix in either table** (§5.8), costs are
`learned + link_rtt` as specified, SSID ranges survive.

Full tables: `research/transit_v2/rc4-2026-09-17/PEER_TABLE_EVIDENCE.md`.

What this does *not* cover: §6 CREQ forwarding, §5.7 poison-reverse (the
reaper hook landed the same day, untested), and PCF safety over 24 h.
PCF's reported link time to us went 19 s pre-deploy → 156 s at 11:02 →
117 s at 11:03, i.e. the post-restart INIT reseed converging back down
as v2.1.13 predicts — but it has to reach its old floor before Phase 3
is anything but open.

### Poison-reverse was hooked on a path peers do not die on

The most consequential thing the first soak found, and it would not
have shown up in a unit test.

§5.7 names the hook `FlexNet_HandleSessionDown`, which does not exist;
the obvious mapping is `FlexNet_CloseSession`, and that is where the
walk first went. But `FlexNet_CloseSession` needs an explicit DISC. In
a 25-minute IR2UFV window IW2OHX-4 cycled its session — `FL` showed its
uptime reset to 53 s — and the console logged **`session started` with
no matching `session closed` and zero `POISON` lines**. The slot had
gone through `FlexNet_Timer`'s ghost reaper instead, which
`memset`s the session directly. So **G4 was dead code on the live
network**: peers were never told that routes via a lost neighbour were
gone, and had to wait for their own ageing.

The walk is now `flex_advertise_poison_session(dead_idx, dead_call)`,
called from **both** paths — `FlexNet_CloseSession` and the reaper. The
reaper clears `sess->active` before calling it, because
`flex_expected_rtt` only counts active sessions as sources and that is
what makes the alternate-path check honest; it passes the stashed
`peer_callsign` for the log, since `sess->LINK` may already point at a
LINKTABLE slot BPQ has zeroed. A summary line (`peer X down — N learned
routes, P withdrawn, C covered by another peer`) is emitted at
`FlexNet_Info` level, not `FLEXNET_DEBUG`, because an operator wants it.

**Lesson for the remaining hooks:** §6's CREQ-forwarding hook has also
never been observed firing, across rc3 and rc4. That was put down to
rc3's cursor starving the emission stream, but this finding says to
check the *hook site* against a capture before believing that
explanation.

### §6's hook site IS reached — what's missing is transit traffic

"Never observed firing" was ambiguous between *not reached* and
*reached and correctly declining*. The IR2UFV log settles it: **15
inbound PID=CF frames in the soak window, all 20-35 bytes, all logged
`CF-NOT-L3RTT … falling through to NetROM L3/L4`** — i.e. the frame
reached the §6 branch, failed the "destination is not us / known via
another session" test, and fell through to local NetROM dispatch. Zero
`CF-TRANSIT-FWD`, zero `CF-TRANSIT-TTL-EXPIRED`.

So the hook is wired into a live path and is exercised. What has never
happened is a peer offering us a frame **for a third party**, which
requires IR2UFV to be that peer's *best* path to somewhere. Spot-check:
`D < IR2UFV` on IW2OHX-4 lists `IW2OHX 3-3 6`, but `D IW2OHX-3` on -4
resolves to `T=4` — -4 has a cheaper path and correctly declines ours.

**T40-T43 therefore needs a destination where our advertised cost
*wins* at the peer**, then an originated connect from that peer's side.
Cross-reference `D < IR2UFV` against each row's chosen `T=` to find the
candidates; the `ro fl del/add` SYS commands on -14 (§"Operational
Lessons") are the lever for forcing one. That is a concrete recipe,
where rc3 left this as "the hook looks right by inspection".

### §6's local-destination check is base-call only, by design

`flex_transit_creq_in`'s gate is
`dst_is_local = (memcmp(l3_dst, MYCALL, 6) == 0)` — the **base six
bytes**, SSID ignored. For a node advertising a `FLEXNETSSIDRANGE` that
is the right call: every SSID in the range terminates locally, and the
existing NetROM/APPLICATION dispatch is what should handle it.

The consequence is worth writing down because it closes off an
attractive shortcut: **a destination sharing our base callsign can
never be transited.** A second instance called `IR2UFV-9` behind
IR2UFV — tempting, because it introduces no new callsign to the mesh
and sits outside IR2UFV's advertised 0-8 range — would have every CREQ
for it classified as local and handed to NetROM, so §6 would never
fire. A controlled test pair therefore needs a **different base call**,
not just a different SSID.

### GAP — `FlexNetLearned[]` entries are never aged

`flex_learned_add` records `last_heard` on every refresh and **nothing
ever prunes on it.** An entry lives until its session dies or the same
key is overwritten. That is fine when a peer *withdraws* a destination
(RTT=60000 arrives and we propagate it) but wrong when a peer simply
**stops mentioning it** — which is what (X)Net does when its own entry
ages out. There is no withdrawal on the wire, so we keep the learned
entry and keep advertising a destination that our source gave up on.

Observed 2026-09-17: IW2OHX-14 aged IR2UFX out of its table silently.
IR2UFV's `learned[]` kept it at the last value it heard, `FL` still
showed `IR2UFX (0-0) T=4365 route: IR2UFV IW2OHX-14 IR2UFX` pointing at
a peer that no longer had the route, and IR2UFV would have re-seeded it
to IW2OHX-4 on the next session-up. Only a process restart cleared it.

**Fix shape:** age `FlexNetLearned[]` in `FlexNet_Timer` — drop entries
whose `last_heard` is older than a multiple of the peer's advertisement
cadence (xnet's own window is < 480 s, so ~600 s is a safe floor), and
treat the drop as a change event so the decision rule withdraws it
downstream. This composes with the hold-down below: ageing removes
entries nobody refreshes, hold-down stops our own withdrawal echoing
back. **Neither exists today, and between them they are why a dead
destination could outlive its origin by hours.**

### GAP — poison-reverse has no hold-down, so it can undo itself

Found the hard way on 2026-09-17 while standing up the IR2UFX test
pair. IR2UFX flapped on an ~11 s L2 cycle; each time it died IR2UFV
correctly poisoned it (`POISON peer-down=IR2UFX dest=IR2UFX-0/0
alt=(none, poisoning)` → `exp=60000` to every other peer). But by then
-14, -4 and -12 had all learned IR2UFX **from us**, so within seconds
they advertised it **back**, `flex_learned_add` accepted it,
`flex_expected_rtt` found a finite path again, and IR2UFV re-advertised
a finite cost — **contradicting the withdrawal it had just sent**.

After IR2UFX was shut down for good, the destination did not disappear.
It kept circulating with the cost climbing on every lap — IR2UFV's
learned value went 97 → 125 → 166 → 199 → 217 while the peers sat at
`T=127` (-14), `T=145` (-4) and `T=336` (-12). That is textbook
count-to-infinity, and at roughly **+50 per 100 s** it would need
~2 hours to reach PC/Flexnet's 4095 cap and well over a day to reach
`FLEXNET_RTT_INFINITY` (60000). Meanwhile the loop keeps refreshing the
entry, so ageing never fires either.

**What rc4 is missing is a hold-down timer.** §5.7 gets the withdrawal
right and then throws it away. The fix has a standard shape: after
poisoning `(dest, ssid_lo, ssid_hi)`, record the time and **refuse to
re-learn that key for a hold-down interval** — long enough for every
peer to have processed the withdrawal (a few advertisement cycles;
xnet's ageing window is < 480 s, so 60-120 s is the right order). A
re-learn during hold-down is by definition an echo of our own poison,
because nothing else could have originated it that fast.

This is exactly the adversarial-loop coverage that §13 R6 flagged as
thin when IR3UFV was dropped from the test topology, and it is the
first concrete cost of that decision. Note also that it is **not** a
transit-only problem in principle — but it is transit that creates the
conditions, because a non-forwarding node never re-advertises anything and so can
never echo a withdrawal back into the mesh.

**Operational note.** (X)Net offers no way to delete a learned
destination: `ROUTER`'s subcommands are `bc`, `flexnet` (link
*partners*, not destinations), `local` and `param`. A phantom must be
starved, not deleted — stop the node that injects it and let the mesh
age it out.

### New open question — `advertised[]` has no reclaim path

`FlexNetAdvertised[peer].advs[]` only ever grows. An entry is created
the first time we consider a destination for that peer and is never
removed — deliberately, because `last_advertised_rtt` is what tells us
a peer was told about a route, and that is exactly what makes the
poison guard work ("only withdraw what we actually advertised"). But
nothing ages out a destination that was withdrawn and stayed withdrawn.

Current headroom is comfortable: IR2UFV sits at **124-127 of 256** with
a ~190-destination network, and §15 Q4 makes an overflow reject-and-warn
rather than corrupt anything. The risk is a long-uptime node on a
churnier network — every transient destination ever seen consumes a slot
permanently, and the node hits the cap and stops advertising *new*
destinations while holding slots for ones nobody can reach.

A reclaim rule needs care, because the obvious one is wrong: dropping an
entry whose `last_advertised_rtt` is infinity loses the record that we
told the peer it was gone, so a later re-learn would look like a
first-ever advertisement rather than a recovery — harmless in itself,
but it also means a *repeated* withdrawal would be re-sent as new. The
safe shape is probably "reclaim entries poisoned longer ago than the
peer's own ageing window", which requires knowing that window.

**Not urgent, and not to be guessed at.** What would settle it is
watching `FL`'s `Advert` column on IR2UFV over days and seeing whether
it plateaus near the destination count or creeps.

### New open question — how long may we hold the `3-` after a `3+`?

§5.6 step 3 says to queue the trailing `3-` after the last record, and
the implementation does exactly that: `eob_pending` is cleared only once
the queue is empty, so end-of-batch means it. But §5.6 did not reckon
with the queue depth its own walk can produce. A `3+` arriving against a
stale view can queue ~190 records, and at the xnet bucket rate that is
~6 minutes before the `3-` goes out — on a PCF peer, ~16. If a peer
treats the token as flow control and waits for its release, we would be
stalling it for minutes.

**Not yet observed, and deliberately not coded around** (hard rule 1 —
capture first). Every `3+` seen on IR2UFV so far walked a current view,
queued 0 and released the token immediately: `3PLUS-WALK from=IW2OHX-14
entries=2 queued=0`, with three prompt `3-` in the capture. The
session-up seed makes that the normal case, since the view is current
by the time a peer asks. What would settle it is a capture of a `3+`
arriving against a deliberately stale view — worth adding to §10.1.c,
and worth checking whether xnet itself ever delays its own `3-`. If a
bound turns out to be needed, the shape is "release on queue-empty **or**
after N seconds, whichever comes first", not a smaller walk.

### L2 forwarding — §5.1's prohibition was wrong, and it is now implemented

**2026-09-17, evening.** The single biggest blocker on multi-hop transit
was not a missing feature but a **wrong conclusion recorded as fact**.
`flexnetd/PROTOCOL_SPEC.md` §5.1 stated that a transit node appending
itself to the digi chain breaks AX.25 V2 and that the pattern is
therefore illegal. A capture taken on PC/Flexnet IW2OHX-12 — a node that
really does transit for IW2OHX-4 ↔ IW2OHX-14 — shows it doing exactly
that, and working.

The mechanism is **symmetric digi-chain rewriting**: append the next hop
on the forward path, **remove it again** on the reverse path, so the
originator only ever sees the chain it sent and V2's reversal invariant
holds. v1.9.4 implemented the append and not the contraction, hit the
predicted symptom, and the conclusion drawn was far too strong — the
pattern is legal, the old implementation was half-finished.

Full write-up and the capture: `research/l2_forwarding_2026-09-17/
FLEXNET_L2_FORWARDING.md`. §5.1 of PROTOCOL_SPEC has been corrected and
a new §5.2 documents the mechanism.

Implemented as `FLEXNETL2TRANSIT` (default NO), one 6-line hook in
`L2Code.c` before the stock `Digipeat()`, everything else in
`FlexNetCode.c`. Per-circuit reverse-path state keyed on
`(src, dst, port)` is **not optional** — on the reverse path "the digi I
appended" and "a digi the originator supplied" are indistinguishable by
inspection, so a transit node must remember. Bounds: 8 digis max
(AX.25's own limit, the only TTL analogue available), never append a call
already in the chain, 64 concurrent circuits, 900 s idle age-out.

**Consequence for this RFC.** §4.3's premise — that multi-hop transit
arrives as a NetROM L4 CREQ — does not describe (X)Net ↔ PC/Flexnet
traffic at all; in the capture, PID=CF frames appear on only one of the
two links and none belong to the user session. CREQ remains plausible for
a genuine BPQ/linbpq peer, which is a separate and still untested path.
NG6 is struck: the reason we could not carry what we advertised was not
"(X)Net never sends CREQ", it was that we had no L2 forwarding at all.

### Verified, and the one thing that is not

Verified deliberately: `IW2OHX-14 → IR2UFV → IW2OHX-4 → IQ2LB-6`, append
and contraction both seen on the wire.

Verified unprompted, which is the stronger evidence — with all
instrumentation reverted and no test traffic being generated, the
counters climbed on their own on real third-party traffic
(`L2FWD IW7TY-15->IW2OHX-13 via IW2OHX-4`). That also retires the
`peer=IW7TY` mystery from the IR2UFX teardown work: IW7TY-15 is a real
station transiting this node, not a phantom.

**Verified 2026-09-18:** the apparent 2:1 skew (`contracted` 72 vs
`extended` 37) was a measurement artefact, not behaviour. Clean
per-window deltas from `quad-watch` are 1:1 (`+5/+5`, `+4/+4`, `+0/+0`),
and the skewed windows are exactly those containing a process restart,
which zeroes both counters — visible as *negative* deltas (`-48/-45`,
`-9/-9`). A cumulative snapshot spanning a restart contracts chains
whose appends belonged to the previous process lifetime. Compare deltas,
never cumulative counters.

### The path reply is the defect, not the forwarding (2026-09-17 21:40)

A connect from IW2OHX-4 to DB0ALG through IR2UFV failed. The cause
inverts the conclusion recorded three hours earlier in this same
section, so it is worth stating bluntly: **answering a PATH_REQ is what
breaks transit.**

```
21:14  PATH-REP-TX -> IW2OHX-4 target=DB0ALG hops=11   => link failure
21:32  PATH-REQ-NOANSWER  (path_len=0)                  => *** connected
```

Eleven callsigns is ten digipeaters plus the destination; AX.25 allows
eight. -4 cannot express that address field, so it shows no `*** route:`
line and cannot build the connect. With no answer it falls back to the
two-digi chain `IW2OHX-4* IR2UFV`, each transit node appends its own next
hop (`L2FWD ... digis now 3`), and the connect completes across ten hops.

This is the mechanism §5.2 of PROTOCOL_SPEC now documents working
exactly as intended: **hop-by-hop rewriting means no node ever needs the
full path.** Supplying one replaces a working mechanism with an
impossible one.

It also explains the degradation that the watch was set up to chase.
Fresh start → cache empty → silent → transit works. Our own background
probe then returns `PATH-REP-RX hops=10`, the cache fills, and every
subsequent request for that destination is answered with an uncarryable
chain. **Our probe poisons our own transit**, per destination, as each
one gets probed — a slow partial failure in which the node keeps looking
healthy.

**Fixed in v2.2.0-rc5** (`PATH-REQ-TOOLONG`): answer only with a chain
the asking peer can carry -- at most the answering port's
`PORTMAXDIGIS`, bounded by AX.25's 8, counted after dropping the final
destination -- otherwise stay silent, which is the tested-good path. The existing refusal to answer a *truncated* chain is sound and
stays — what is missing is the symmetrical guard, because **a chain that
is too long is also a wrong chain.**

Two further defects in the same log: `age=1789673536s` in the NOANSWER
line is `now - 0`, an uninitialised `path_updated` read (harmless today,
since `path_len > 0` is tested first, but the log lies); and the comment
claiming our probes "do not receive replies" is false — 185 `PATH_REP
from IW2OHX-14` and 39 from -12 are in the log.

Full write-up: `research/l2_forwarding_2026-09-17/PATH_REPLY_TOO_LONG.md`.
A controlled flip (`/tmp/causation-test.sh` on gw) is armed to confirm
causation rather than infer it.

### Open inconsistencies under watch (as of 2026-09-17 18:00 local)

Left running overnight, read-only, on gw:

* `tools/quad-watch.py` — samples IR2UFV, -14, -4 and (hourly, chained
  through -14) -12 within seconds of each other and cross-checks the four
  tables. A distance-vector inconsistency is by definition a
  *disagreement between two tables*, so it is invisible from either end
  alone — which is why single-sided `pcf-watch.sh` never found one.
  Alerts in `/tmp/quad-watch/alerts.log`.
* rotating `tcpdump`, `udp port 10075 and not udp port 10093` (the
  exclusion keeps the production -13 instance out), 8 x 20 MB ring in
  `/tmp/ir2ufv-wire/`, so any alert can be traced to frames.

Three things already look wrong and are the reason the watch exists:

1. **The IW2OHX-4 session restarts.** First quad sample caught
   `up=00:07:21` against -14's `00:26:01` on a node that had not been
   touched for 26 minutes, with `routes=0` learned from -4 at that
   moment. Cause unknown.
2. **PCF's advert queue does not drain to zero.** -12 sampled at
   `advert=110 queued=13`, and the queue has been seen at 102 → 68 → 12
   over 10 minutes. Whether it ever reaches 0 between refreshes is
   exactly what `A7 QUEUE_STUCK` will answer.
3. **PCF's cost to us is 2348/5** against 1/1 for its (X)Net peers. Not
   interpretable yet: IR2UFV was restarted ~25 times during development,
   so the cost ring is full of that churn. The overnight quiet window is
   what makes this measurable.

**Not yet done:** §10.1.b/c Phase 1 soak (B1-B8, D1-D3), §10.2/§10.3
Phase 2/3, §6 CREQ-forwarding verification (hook unchanged from rc3,
still never observed firing), the append/contract asymmetry above, the
linbpq↔linbpq L2 *link* teardown
(`research/l2_forwarding_2026-09-17/IR2UFX_LINK_TEARDOWN.md` — a
different problem from L2 forwarding, and one that has never affected the
live xnet/PCF peers), and the v2.2.0 tag.

---

## 14. Implementation Schedule

Estimate for v2.2-rc4 (the event-driven redo). Builds on the rc3 code
in main as the starting point — the `flex_learned_add` hook, the
CREQ-forwarding hook in `FlexNet_ProcessCF`, and the `FLEXNETTRANSIT`
directive are already there. The replacement work is concentrated in
§5 (route advertisement) and the new test infrastructure in §10.

| Day      | Work                                                                                              |
|----------|---------------------------------------------------------------------------------------------------|
| **D1 AM**  | Implementation prep — design the four `FlexNet_Info` log lines (ADVERT-CHECK / BUCKET / POISON / 3PLUS-WALK per §10.1.a) so they emit at every state-machine transition; these are the observability that replaces unit-test assertions. (Original schedule item — building an offline unit-test harness — was dropped 2026-06-07; IR2UFV's real peer traffic is sufficient to exercise the §5 logic deterministically enough.) |
| **D1 PM**  | Drop rc2's cap+cursor block from `flex_send_own_routes`. Add `FlexNetAdvertised[]` parallel array + `flex_advertise_check()` decision rule (§5.3). Add `FLEXNET_REFRESH_THRESHOLD` jitter suppression. **Flip the compiled default to transit-off** — `g_flexnet_transit_enabled = FALSE` (§15 Q2 as superseded 2026-09-14), so a node without a `FLEXNETTRANSIT` line keeps v2.1 behaviour; document the directive and its default in `README.md` (it is currently undocumented, which is how prod ended up in transit mode by omission). Build only — no behaviour-changing wire emissions yet. |
| **D2 AM**  | Token bucket — `flex_advertise_drain()`, per-peer family detection (PCF vs xnet_like), refill schedule. Hook into `FlexNet_Timer`. Add the `peer_family` flag to `FLEXNET_ADVERTISED_STATE`. |
| **D2 PM**  | Wire `flex_advertise_check` into the four trigger sites (§5.3 (a)-(d)): `flex_learned_add` RTT-change, `flex_link_time_sample` link-RTT change, `FlexNet_HandleSessionDown` poison, 120 s direct-neighbour keepalive. Implement `is_direct_neighbour` flag. |
| **D3 AM**  | `3+` REQUEST response path — walk learned[] through decision rule, drain via bucket, queue trailing `3-`. Verify against §5.6 sequence. |
| **D3 PM**  | Poison-reverse on session loss — `FlexNet_HandleSessionDown` callbacks. Implement alternate-session check (don't poison if another peer covers the dest). |
| **D4 AM**  | Deploy to IR2UFV with `FLEXNETTRANSIT=NO` first to confirm non-forwarding mode is unaffected, then flip to `=YES` with the conservative PCF rate (1/5 s, bucket=2) applied to all peers. Run §10.1.b behavioural soak B1-B8 for 30 min while monitoring the four FlexNet_Info log lines plus eth0 pcap. Iterate on jitter threshold and bucket math by tweaking constants in `FlexNetCode.c` and re-soaking (~30 min cycle). Build `parse_advertise.py` analyser against the captured pcap. |
| **D4 PM**  | Phase 2 T20-T23 tests with xnet-14 + xnet-4 (cap=8 / 120 s rate is conservative; event-driven is even gentler). 1-hour soak. |
| **D5 AM**  | Phase 3 T30-T32 PCF tests — the conservative pass. 24 h IR2UFV ↔ PCF soak before progressing. |
| **D5 PM**  | T40-T43 CREQ forwarding tests (rc3 hook code unchanged; just need to confirm with the now-fresh advertisements driving real route choices). |
| **D6**     | Wire-byte comparison vs xnet's emissions (T23, T31). Address any deltas. |
| **D7**     | Promote to production iw2ohx-13. Tag v2.2.0. GitHub release. Skill / RFC / memory updates. |

Total: ~1 working week of focused work, but spread across calendar
days because of the soak windows. The 24 h PCF soak (D5 → D6) is the
hard gate before production promotion.

**What's NOT changing from rc3** (and therefore not in this schedule):
- `flex_learned_add` (§5.2 unchanged)
- CREQ-forwarding hook in `FlexNet_ProcessCF` (§6 unchanged)
- `FLEXNETTRANSIT` directive parsing (§5.9 constants only)
- All data structures from `FLEXNET_LEARNED_STATE` (the `learned[]`
  side is right; what's new is the `FlexNetAdvertised[]` side).

**Risk reserves:** add 1-2 days of buffer for D4-D5 if jitter
suppression needs tuning beyond the 10% / 1 tick default, or if PCF
proves more sensitive than the 1 / 5 s bucket suggests. The Phase 2
research established 1 / 50 s as PCF's natural rate; we're 10× more
aggressive — that's the margin we're testing.

---

## 15. Decisions (Locked 2026-05-17; Q1 superseded 2026-05-18, Q2 superseded 2026-09-14, Q4–Q6 added 2026-05-18)

1. **Q1 — Single 120 s cadence** for all peer families. ACCEPTED
   on 2026-05-17 → **SUPERSEDED on 2026-05-18.** Per the §5 rewrite,
   advertisement is now event-driven with per-peer token buckets
   (PCF 1 / 5 s bucket=2, xnet-like 1 / 2 s bucket=4). The 120 s
   cadence survives only as the direct-neighbour keepalive interval
   (§5.5). Single uniform cadence proved inadequate per §16-§17.
2. **Q2 — `FLEXNETTRANSIT` default.** ACCEPTED as **YES** on 2026-05-17
   → **SUPERSEDED on 2026-09-14: the compiled default becomes `NO`.**
   Implemented as part of rc4 D1 (§14).

   Rationale — the YES default caused a live misconfiguration rather than
   a hypothetical one. Production IW2OHX-13's `bpq32.cfg` carried no
   `FLEXNETTRANSIT` line, so it silently ran the rc2 cap+cursor
   re-advertisement for months, contradicting §11's "production stays
   non-forwarding" twice over. A 150 s capture of its outbound AXIP on
   2026-09-14 showed 16 transit records (`3HB9AK` / `3HB9AM` / `3HB9ON`)
   to both peers; adding `FLEXNETTRANSIT NO` + restart took that to 0.

   The principle: **transit is a role a node opts into, never one it
   inherits by omission.** An operator who never heard of the directive
   must get v2.1 non-forwarding behaviour, which is also the safe behaviour toward
   PC/Flexnet peers (§16-§17: transit emission is precisely what broke
   PCF in rc1-rc3). A silent default that enables a still-unproven code
   path on any node that forgets a config line is the wrong trade.

   Operationally this changes nothing today: prod IW2OHX-13 carries an
   explicit `NO` (cfg line 14) and IR2UFV an explicit `OFF` (cfg line
   376), so both nodes keep their current behaviour regardless of the
   default. Test bed must therefore set `FLEXNETTRANSIT YES` explicitly
   when Phase 1 flips on — see §11 step 3.
3. **Q3 — Defer** factoring transit logic into the shared
   `flexnet_l3.c/h` module. ACCEPTED, still valid.
4. **Q4 — `FLEXNET_MAX_LEARNED_PER_NEIGHBOUR` overflow handling.**
   ACCEPTED on 2026-05-18. With xnet-14 already at 188 learned routes
   (74 % of the 256 default cap), overflow is reachable in normal
   operation. On overflow `flex_learned_add` **rejects + Consoleprintf
   warns** (`"FlexNet: learned[] full for peer %s, dropping %s-%d"`)
   instead of silent overwriting. Operators can bump
   `FLEXNET_MAX_LEARNED_PER_NEIGHBOUR` to 512 (or higher) in a rebuild
   — static heap absorbs it (~70 kB / peer at 512). Rationale: silent
   loss of routes was the failure mode in rc1; loud failure is
   debuggable. The 188 → 256 gap is enough for rc4 soak; production
   rebuild with 512 is a follow-up if needed.
5. **Q5 — RTT=0 sentinel handling** (closes OQ1 from §13.2).
   ACCEPTED on 2026-05-18. xnet emits RTT=0 records for some
   destinations (e.g. `DM0ZOG` samples `[0, 49, 120, 727]` observed
   in Phase 2). Meaning unconfirmed — likely a "newly learned, not
   yet measured" sentinel distinct from 60000 poison. For rc4:
   `flex_dtable_merge` **skips records with RTT=0** without adding
   them to `learned[]`, and logs once per `(peer, dest)` pair
   (`"FlexNet: skipping RTT=0 from %s for %s"`) using a static
   dedup set to avoid log spam. Defer empirical clarification to a
   Phase 4 capture (active probe an RTT=0 destination, watch for
   subsequent non-zero update). If Phase 4 shows RTT=0 is meaningful
   routing info, revisit in v2.3.
6. **Q6 — `flex_advertise_check` link-RTT trigger dampening**
   (§5.3 trigger (b)). ACCEPTED on 2026-05-18. When our smoothed
   RTT to peer P changes, every learned route through P would
   re-evaluate `expected` — potentially 200+ check calls per tick on
   smoothed-RTT jitter. **Trigger (b) fires only when link-RTT delta
   ≥ 1 tick (100 ms)** since the last advertisement-cycle anchor for
   this peer. Smoothed-RTT IIR filter noise (sub-tick) does NOT
   trigger re-evaluation. This is in addition to the §5.3 step (3)
   per-record jitter floor (10 % / 1 tick on the *expected* RTT
   itself). The two thresholds compose: trigger (b) gates whether to
   even walk; step (3) gates whether to emit per-record. Validate
   the dampening doesn't suppress legitimate RTT shifts during T22
   (RTT-change propagation latency).

---

_RFC authored by Claude on 2026-05-17. Decisions accepted same day;
implementation rc1/rc2/rc3 reverted same day — see §16-§17 for the
lessons. §5 was rewritten on 2026-05-18 (event-driven model), Q1
revised accordingly, and Q4–Q6 added the same day to lock the
overflow handling, RTT=0 sentinel, and link-RTT trigger dampening
before rc4 implementation starts._

---

## 16. Lessons Learned — Step 2 First Attempt (2026-05-17) — _Historical_

> **Reader note:** §5 was rewritten on 2026-05-18 with an event-driven
> model that supersedes the cap+rotating-cursor design described and
> proposed in this section. §16 and §17 are preserved as the narrative
> of what was tried, what broke, and why the spec was changed. They
> are NOT the spec. Implementations follow §5.

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

## 17. Lessons Learned — Step 2 Redo + Step 3 First Attempt (2026-05-17, later) — _Historical_

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
   _Superseded 2026-06-05 (IR3UFV dropped) and 2026-06-07 (offline
   unit tests also dropped): see §10 amendment — testing is now
   purely behavioural on IR2UFV's real peer links, with state-
   machine observability via four `FlexNet_Info` log lines built
   into the §5 implementation._
3. **Once stable on linbpq ↔ linbpq, add xnet target.** xnet is
   tolerant of cap=8 (the rc2 verification showed this works
   against xnet-14 and xnet-4).
4. **PCF support last.** Treat it as a separate sub-project under
   §17.4 #1.

---

_RFC §17 lessons authored by Claude on 2026-05-17 after the rc2 +
rc3 revert. Step 2 redo is parked; needs more design before
re-implementation._
