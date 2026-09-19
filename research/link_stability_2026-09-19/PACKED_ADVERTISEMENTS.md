# One record per frame — the advertisement volume defect

**2026-09-19.** From the 22 h overnight capture of all three IR2UFV
FlexNet links (2026-09-18 08:58Z → 2026-09-19 07:03Z) plus a 17.2 h
`linkstab` run. This closes item 2 of
`../link_stability_2026-09-18/TEARDOWN_DIRECTION.md` ("-12 needs a volume
reduction, not a timer") and confirms item 1 (the `-4` timer fix).

## 1. The `-4` timer fix is confirmed

The falsifiable prediction was: *if our outbound DISC count to -4 does
not collapse, the diagnosis is wrong.* Counting own L2 sessions only
(`axudp_teardown.py --link-only --local-ip 192.168.1.202`):

| window | our teardowns to -4 | rate |
|---|---|---|
| pre-fix 08:58–13:52Z (4.9 h) | 38 | 7.8 / h |
| post-fix 13:52Z–07:03Z (17.2 h) | **1** | **0.06 / h** |

`RETRIES` 5 → 25 removed 99 % of our teardowns. The diagnosis held.

## 2. After that fix, all three peers tear us down and we tear down nobody

| link | our teardowns | their teardowns | how they end it |
|---|---|---|---|
| IW2OHX-12 (PC/Flexnet) | 1 | 21 | `DISC` |
| IW2OHX-4 (TNC4e) | 1 | 26 | `DM` |
| IW2OHX-14 ((X)Net) | 0 | 12 | `DM` |

`DM` rather than `DISC` means the peer had already dropped the session
silently and only said so when we next polled.

**Three unrelated implementations all dropping us is a statement about
us, not about them.** That is what pointed at emission volume.

> Method trap: `axudp_teardown.py` defaults `--local-ip` to the most
> frequent *source* address. On the `-14` capture the peer out-talks us,
> so the tool adopted the peer's address and reported every direction
> backwards — "we tear down" when the truth is the opposite. Always pin
> `--local-ip`.

## 3. The defect — one route record per AX.25 I-frame

The compact CE route format is **one `'3'` per FRAME**, then N
fixed-shape records, then `CR`. Every peer in the mesh does this. We did
not: we emitted one record per I-frame, each with its own `'3'`.

Measured over the 22 h window, outbound route frames only:

| peer | frames | records | **records/frame** | bytes/frame |
|---|---|---|---|---|
| IW2OHX-12 | 14055 | 14055 | **1.00** | 12.2 |
| IW2OHX-14 | 3642 | 3642 | **1.00** | 12.0 |
| IW2OHX-4 | 12101 | 12101 | **1.00** | 12.2 |

Exactly 1.00 on every link for 22 hours — the fingerprint of the bug.
What the peers send us over the same window:

| peer | their max info body | their mean |
|---|---|---|
| IW2OHX-14 ((X)Net) | 248 B | 113.6 B |
| IW2OHX-4 | 247 B | 101.3 B |
| IW2OHX-12 (PC/Flexnet) | 205 B | 205.0 B |

`PACLEN` on the port is 236. **We were using 15 bytes of it — 6 %.**

### Why that destabilises the link

The token bucket to a PC/Flexnet peer refills at 1 token / 5 s, and a
token bought one *record*. So:

* the drain rate to `-12` was **12 records/min**;
* the queue was fed at **26.8 records/min** (27762 advertisement fires
  in 17.25 h).

The queue was **structurally oversubscribed 2.2×** and could never
empty. Measured: median depth 72, max 199, **non-empty 80 % of the
run**. A ~210-destination re-dump after a session reset needed 210
frames — **17.6 minutes** of continuous transmission.

That is the "permanently busy link is a permanently expensive link"
mechanism from `../link_stability_2026-09-18/TEARDOWN_DIRECTION.md`,
with a number attached.

It also corrects that document's conclusion that the re-dump "cannot be
made cheaper, only less frequent". It can: the re-dump is the same
number of *records* in ~1/19 of the *frames*.

## 4. The fix

`flex_advertise_drain()`: **a token now buys a FRAME, not a record.**
The frame is filled with as many queued records as the byte budget
allows (`FLEXNET_ADVERT_FRAME_BYTES` = 200, under the 236 `PACLEN`).

`flex_build_route()` was split: `flex_build_route_rec()` writes one bare
record, and `flex_build_route()` remains as the `'3'` + record + `CR`
wrapper for the call sites that legitimately send exactly one.

**The I-frame rate to the peer is unchanged.** That is the whole safety
argument. The rc1 flood (`RFC_TRANSIT_ROLE_V2.md` §16.2) saturated
PC/Flexnet with *~50 I-frames in under 2 seconds* and left it in a state
that needed a manual reset. Nothing here raises frames per second —
only bytes per frame, and only to what PC/Flexnet itself transmits.

## 5. Test

`tools/unit/test_compact_pack.c`, built by `tools/unit/extract.sh`,
which lifts `flex_build_route_rec`, `flex_build_route` and
`flex_parse_compact_records` **verbatim out of FlexNetCode.c** so the
test cannot drift from shipped code. 30 checks, clean under
`-fsanitize=address,undefined`:

* a single-record frame is byte-identical to the pre-packing format;
* packed frames round-trip through the real parser;
* **real captured (X)Net and PC/Flexnet frames** decode as expected,
  including space-padded callsigns and `'?'` = SSID 15;
* our packed bytes equal the captured PC/Flexnet shape **byte for byte**;
* a withdrawal (RTT 60000) packs beside a live route and still decodes
  as infinity;
* the byte budget is never overrun and a record that does not fit is
  rejected rather than truncated.

Per-file warning count against the pre-change baseline: **47 → 47**, no
new warnings.

## 6. Measured on the wire (v2.2.0-rc6, deployed 09:19 CEST)

Outbound route frames, first minutes after deploy:

| peer | records/frame | bytes/frame | max frame |
|---|---|---|---|
| IW2OHX-12 | 6.30 | 70.0 | 181 B |
| IW2OHX-4 | 3.31 | 37.0 | 197 B |

Queue to `-12`, the same measurement before and after:

| | rc5 | rc6 |
|---|---|---|
| queue non-empty | **80 %** of run | 0 % |
| median depth | 72 | 0 |
| 100 s after link-up | 183 queued, 76 advertised | **0 queued, 202 advertised** |
| still queued at 11 min | 111 | — |

The cold-start seed that took ~17.6 minutes now completes inside
100 seconds and the queue sits empty.

Records/frame in steady state (6.3, 3.3) is below the ~17 the budget
allows, and that is the intended shape: once the backlog is gone there
is rarely more than a few records waiting. Packing is doing its work
during the bursts, which is where the cost was.

## 7. Still open

* **PC/Flexnet's own teardown timer.** Every `-12` DISC lands an exact
  multiple of 60 s after our INIT (120, 180, 2040, 5160, 7044, 12000 …),
  and the cycle is rigid: `DISC` → `SABM` same second → 60 s → `DISC` →
  180 s → `SABM`. PCF evaluates something once a minute and cycles the
  link when it trips. That timer is not ours to change; reducing how
  busy and expensive we look is the only lever we have, and this is the
  biggest one available. Whether it is *enough* is what the soak
  measures.
* `-4` and `-14` ending sessions with `DM` — the peer forgetting the
  session silently — is not yet explained.
* The absolute jitter floor (`delta <= 2` ticks, ~16 % of jitter fires)
  and the §13.3 hold-down on transitions to infinity remain unbuilt.

---

# Second defect — unsolicited re-INIT on a healthy link (v2.2.0-rc7)

Found during the rc6 soak, 2026-09-19 08:00Z. `linkstab` reported
`SESSION_RESTART IW2OHX-14: uptime 00:40:13 -> 00:00:37`, but
`axudp_teardown.py` found **no L2 event at all** on that link — no DISC,
no DM, no SABM, anywhere in the capture. The wire showed instead:

```
07:19:23  In DM -> Out SABM -> In INIT + Out INIT   normal startup handshake
07:59:59  Out INIT                                   unsolicited
08:01:23  Out INIT                                   unsolicited
```

**We were re-INITing the FlexNet session on a link that was perfectly
up.** That matters because a re-INIT reseeds the peer's link-cost ring
with a `600` outlier — PC/Flexnet's `L *` read `600 4095 1` for us
against `1 1 1 1 …` for its (X)Net peers, with our advertised cost at
`1565/5` against their `1/1`.

## Mechanism

`FlexNet_Timer`'s proactive init scan re-handshakes any link that looks
un-initialised:

```c
if (L->L2STATE != 5)  continue;
if (L->FlexNetLink)   continue;      /* the only guard */
... FlexNet_InitSession(L, port);
```

BPQ clears `FlexNetLink` during internal L2 maintenance without putting
anything on the wire — v2.1.15 already documented exactly this and added
an established-guard. But that guard went on the **same-LINK** path only.
`FlexNet_InitSession` has three:

| path | condition | guarded before rc7 |
|---|---|---|
| 1 | same LINK pointer | ✅ v2.1.15 |
| 2 | **same callsign, new LINK pointer** | ❌ |
| 3 | no session — allocate a slot | n/a, INIT is correct |

Path 2 unconditionally cleared `got_peer_init`, `flex_est_inferred` and
`sent_routes`, reset `session_start` (hence the phantom 40:13 → 00:37)
and **sent INIT**.

## Why path 2 can be guarded safely

A genuine L2 reconnect cannot reach path 2. Losing the link runs
`FlexNet_CloseSession` (or the ghost reaper) first, which deactivates the
slot — so the reconnect lands on path 3 and INITs correctly. Arriving at
path 2 with an **established** session therefore means only the
LINKTABLE pointer moved: BPQ recycled the peer into a different `LINKS[]`
slot while the L2 session stayed up.

So rc7 migrates the pointer, keeps the session, and returns. Session
state, `sent_routes` and `session_start` are all preserved, and nothing
goes on the wire.

**Deliberately not done:** forcing a re-seed whenever an inbound INIT
arrives. It looks like a safer belt-and-braces, but `-4` sent 83 INITs
against 26 teardowns in the 22 h capture — roughly 3 INITs per session —
so it would have triggered ~83 full table re-dumps a day for no gain.

## Status

Built clean, per-file warning count 47 → 47. Deployed to IR2UFV
2026-09-19 08:07Z. The two defects interact: rc6 made the re-dump ~19×
cheaper, rc7 stops us triggering re-dumps (and poisoning peer cost
rings) on links that never dropped.

Stability can only be measured fairly with both in, which is why the rc6
soak was cut at 0.8 h and rc7 restarts the clock. rc6's volume result
stands on its own and is recorded above.

---

# Soak result, rc7 at 1.1 h

| | rc5 baseline (17.2 h) | rc7 (1.06 h) |
|---|---|---|
| our teardowns → -12 / -14 | 1 / 0 | **0 / 0** |
| peer teardowns → -12 | 21 (1.22/h) | **0** |
| peer teardowns → -14 | 12 (0.70/h) | **0** |
| unsolicited re-INITs | 3–4 per link per 0.8 h (rc6) | **0** |
| SESSION_RESTART | 34 (1.97/h) | 0 logged |
| QUEUE_DEEP | 405 | **0** |
| PCF_OVERRUN | 307 | **0** |
| queue to -12, non-empty | 80 % | **7 %** |
| queue to -12, median/max | 72 / 199 | **0** / 27 |

## PC/Flexnet's cost ring for us is converging

The single clearest measure, read off `L *` on IW2OHX-12:

```
rc5 2026-09-18:   883/5   600 600 4095 1 1 1
rc6 09:29:       1565/5   600 4095 1
rc7 08:43:        588/5   600 4095 1 1 1 1 1 1
rc7 09:11:        336/5   600 4095 1 1 1 1 1 1 1 1 1 1 1 1
```

`1` is the healthy sample — what IW2OHX-14 shows (`1/1`, sixteen 1s, up
5 d). The leading `600 4095` are this session's start seeds; the ring is
16 slots, so they age out. **Every entry we have contributed since the
rc7 restart is a `1`**, which is what "stop reseeding the ring" looks
like from the far end.

## IW2OHX-4 is a separate, non-ours fault

`-4` got worse, not better: 6 peer teardowns in 1.06 h (5.7/h) against a
1.51/h baseline. It is not the packing:

* none of the six `DM`s follows a large frame — they follow silence and
  our RR polls, or 14–24 byte frames. Same "peer forgot the session"
  signature as rc5.
* the largest frame we send `-4` is 198 B; `-4` sends *us* up to 247 B.
* **`-4` is flapping against PC/Flexnet as well** — PCF's `L *` shows
  `IW2OHX 4-4 ... 5m, 6s` with its ring restarting (`600 1`). Nothing on
  the `-4` ↔ PCF link changed.
* `-4`'s own advertisements to us collapsed to 9 frames in an hour, max
  25 B (rc5: 32/h at up to 247 B) — a node that keeps losing its table.

`-4` is the RAM-only TNC4e that was physically reset this week. Its
churn is its own; what changed is that it no longer costs us the routing
table or a 17-minute re-dump.

**Caveat, honestly:** one outbound teardown to `-4` in this window
(0.94/h) against 0.06/h over the 17.2 h post-`RETRIES` run. n=1 on a
peer that is visibly unwell, so no conclusion — but worth re-checking
over a longer soak.

## What this does and does not prove

Proven: the volume defect and the re-INIT defect are both fixed, and the
peer-side cost we were manufacturing is falling.

Not yet proven: that `-12` has stopped cycling. rc5's rate predicts ~1.3
teardowns in 1.06 h and we saw 0, which is encouraging but one hour
against a fault whose observed period ranged 6 min to 3.3 h is not a
result. PC/Flexnet's own 60 s evaluation tick is still there and is
still not ours to change.
