# IR2UFV ↔ IW2OHX-12 — the destination exchange feeds a loop

**2026-09-20.** Taken after the telnet-freeze fix
(`TELNET_SLEEP_FREEZE.md`) had removed the last *node-level* stall, on
the standing `tfix` watch: 8.6 h of AXUDP capture
(`/tmp/tfix-mon-12/link.pcap0`) plus `linkstab` FL polls
(`/tmp/tfix-linkstab/fl.jsonl`), IR2UFV running v2.2.0.

The link was still recycling. This is what it was.

## What the link actually did

`linkstab` reported only **one** `SESSION_RESTART`, which is why the
problem looked smaller than it was: the other seven cycles removed
`IW2OHX-12` from the `FL` table entirely rather than restarting it in
place, and the restart detector only watches uptime going backwards.
Reading `fl.jsonl` for the link *disappearing* gives the real count:

| uptime reached | then |
|---|---|
| 5215 s (86 m) | gone |
| 4572 s (76 m) | gone |
| 4556 s (76 m) | gone |
| 5190 s (86 m) | reset in place |
| 4211 s (70 m) | gone |
| 77 s | gone |
| 5215 s (86 m) | gone |

Seven cycles in 8.6 h, most at 70–87 minutes of uptime.

> **Trap, and it cost the first hour: `linkstab`'s `SESSION_RESTART`
> under-reports.** A link that vanishes from `FL` and comes back is not
> a restart by its definition. Count disappearances too, or the fault
> looks seven times smaller than it is.

## Who tears down

`axudp_teardown.py --local-ip 192.168.1.202 --peer-ip 192.168.1.201
--link-only`, over the whole capture:

```
Out I 3642   In RR 3269   Out RR 1532   In I 1530
Out UA  31   In SABM 17   In DISC  14   In DM    4
teardown Out=0 In=18 | setup Out=0 In=17  -> peer tears down
```

**We never send a DISC or a SABM. PC/Flexnet initiates all 18.** That
halves the problem: nothing in our session teardown path is involved,
and the question is what we are doing that makes PCF hang up.

Note the first line too — we send **2.4× more I-frames than we receive**.

## The teardowns come in pairs, and only the first one is a cause

Every teardown has a partner **exactly 60 s later**:

```
09:11:01 DM   → 09:12:01 DISC      12:16:22 DISC → 12:17:23 DISC
09:37:26 DISC → 09:38:26 DISC      13:46:23 DISC → 13:47:23 DISC
10:57:26 DM   → 10:58:26 DISC      15:00:17 DISC → 15:01:17 DISC
15:04:50 DISC → 15:05:51 DISC      16:34:51 DISC → 16:35:51 DISC
```

The second of each pair is **a consequence, not a cause**. After the
first teardown the session re-establishes, and PCF's link-time report to
us reads `600 600` — its seed value on a fresh session, the same `600`
that re-INITs used to put in its cost ring — and it drops again 60 s
later. Decoding the LT stream makes this unambiguous: every `600` in the
capture sits in a window containing `In SABM` / `Out UA` / `Out '0'`.

So there are **8 independent teardowns**, not 18.

> **Trap: `600` is PCF's session seed, not a symptom of the thing that
> preceded it.** Reading the pairs as sixteen separate events, or the
> `600`s as evidence about our behaviour just before them, inverts the
> causality.

## What PC/Flexnet is telling us

PCF answers every one of our `'2'` keepalives with `'1'<lt>` — 1118 of
1118, median 0.01 s — and the values it reports are exactly `5295/n`:

```
n:    3     4     5    6    7    8    9   10   11   12   13   14   15   16
    1765  1324  1059  883  757  663  589  530  482  442  408  379  354  332
```

A 16-slot ring holding one very large sample taken at session start,
diluted by each subsequent one. It bottoms out at `5295/16 ≈ 331`, and
`332` is the value present just before several of the teardowns.

Meanwhile **our** advertised link time is the constant `5`, 98 times in
8.6 h. That is the `/5` in PCF's `L *` row for us (`883/5`, `336/5`);
its (X)Net peers read `1/1`.

## The trigger — CORRECTED

**First reading, and it was wrong.** All 8 independent teardowns follow
one of our outbound LT frames within 90 s, and exactly those 8 of our 98
LT sends are followed by a teardown (p ≈ 4×10⁻⁵). That correlation is
real, but it identifies the wrong frame: we answer a `3+` with an LT
*and* a table walk in the same instant, so both carry the same
timestamps.

**The cause is PC/Flexnet's `3+` full-table request.**

```
inbound '3+' requests             : 8
independent teardowns             : 8
'3+' followed by a teardown ≤120s : 8/8      p ≈ 2.5e-13
teardowns with no preceding '3+'  : 0/8
```

A perfect 1:1 mapping in both directions. What we sent back each time,
against a table of 204 destinations:

| `3+` at | → teardown | records sent | of 204 |
|---|---|---|---|
| 09:10:12 | 49 s | **3** | 1.5 % |
| 09:37:18 | 7 s | 24 | 12 % |
| 10:56:09 | 77 s | 45 | 22 % |
| 12:16:08 | 15 s | **3** | 1.5 % |
| 13:45:45 | 38 s | 38 | 19 % |
| 14:59:44 | 33 s | 72 | 35 % |
| 15:04:18 | 33 s | 39 | 19 % |
| 16:34:13 | 38 s | 36 | 18 % |

…each followed by a `3-` end-of-batch. PC/Flexnet asks for our table,
receives between 1.5 % and 35 % of it plus "that's all", and hangs up.

Two of the answers were **three records**. That is why delivery rate
never explained it — the tiny answers were dropped exactly as fast as
the large ones, and the earlier advertisement-volume correlation
(median 40 records in the preceding minute vs a baseline of 5) is a
*symptom of the same exchange*, not the mechanism.

### Why the answer was short

`FlexNetCode.c:2141` passed `force=FALSE` to the `3+` walk, so an
explicit request for the whole table was run through the 10 %
change-detection threshold — a filter that exists to keep *unsolicited*
advertisements off the wire.

The `force=FALSE` was inherited from `flex_advertise_seed_peer()`, where
it is correct and says so:

> *force=FALSE deliberately: a fresh session's advertised[] is empty, so
> every entry fires on the never-advertised sentinel anyway.*

That precondition holds at a cold start and **not** mid-session. A `3+`
arriving on an established session finds `advertised[]` fully
populated, the never-advertised sentinel no longer fires, and the
threshold filters the table down to whatever happened to have moved.
`flex_advertise_neighbours()`, the third caller, already used
`force=TRUE`.

It also explains the shape we kept seeing: the **seed dump after a
session restart is complete** (345 records) while a `3+` response
minutes later is three. Same walk, same peer, opposite outcome, because
one ran against an empty table and the other did not.

### It was never only PC/Flexnet

`IW2OHX-14` sent a `3+` once in the same capture, at 17:09:07. We
answered with **5 records — 2 unique and 3 duplicates** — for a table of
the same ~204 destinations. No teardown followed.

So we have been answering *every* peer's full-table request with
whatever happened to have moved. (X)Net tolerates it and PC/Flexnet does
not, which is the only reason this surfaced on `-12`. The corollary is
that the destination table every FlexNet peer holds for us has been
wrong for as long as this has been in, not merely unstable — and the
`-12` teardowns were the symptom that made it visible rather than the
extent of the damage.

## The fixes

### 1. Answer a full-table request with the full table (the stability fix)

`force=TRUE` on the `3+` walk. It bypasses the jitter threshold only —
split-horizon, the GA scope gate and the poison hold-down all still
apply, and the token bucket still meters delivery, so 204 records leave
as ~13 frames over ~65 s. PC/Flexnet's own bulk dumps to us run an order
of magnitude faster than that.

A climb-suppressed destination (below) would have re-opened the same
hole by being silently omitted, so under `force` it answers explicitly
with a withdrawal instead of saying nothing.

### 2. Count-to-infinity containment (an efficiency fix, not the cause)

`flex_climb_is_loop()` in `FlexNetCode.c`, called from
`flex_advertise_check()` before the jitter test.

Per (peer, destination) we keep the cheapest cost seen — `rtt_floor` —
and `climb_steps`, the number of consecutive rises away from it. A fall,
or a first sighting, re-floors and resets the count: that is a route
settling. When a cost has risen for at least `FLEXNET_CLIMB_MIN_STEPS`
(3) consecutive steps **and** reached `FLEXNET_CLIMB_RATIO` (4) × the
floor, we withdraw it once, log `CLIMB-WITHDRAW`, and let the existing
poison hold-down keep it quiet.

Both conditions are needed:

* **the ratio alone** would catch an honest re-route onto a path 4×
  worse, which is one rise, not a loop;
* **the step count alone** would catch any slowly-degrading real link.

Plus a backstop in `flex_build_route_rec()`: a **finite** cost above
`FLEXNET_RTT_WIRE_MAX` (4095) is clamped, so no call site can put an
unrepresentable number on a PCF peer's wire. The 60000 withdrawal
sentinel is exempt — it is a signal, not a measurement.

### Tests

`tools/unit/test_climb_guard.c`, extracted from source by
`tools/unit/extract.sh` like the existing test, 17 checks:

* a steady cost, including our own direct neighbours at 1 and 2, never
  trips it however long it runs;
* jitter within ~1.6× of the floor never trips it;
* a single 10× re-route does not trip it, nor do two consecutive rises;
* a ladder that stays under 4× is left alone (the deliberate boundary);
* the real K1YMI series is caught — 45 rungs become 3 withdrawals;
* infinity is never an input, only an outcome;
* state resets after a trip, so a recovered route is judged fresh;
* the wire clamp fires at 4095 and leaves 60000 and ordinary costs
  alone.

`test_compact_pack.c` still passes (30 checks). `FlexNetCode.c` warning
count under `-Wall -Wextra -Wshadow` is **52, unchanged** from the same
build tree before the change; `flexnet_l3.c` stays at 0.

## Deployed

IR2UFV only; production IW2OHX-13 untouched. The `tfix` capture and
`linkstab` were **deliberately left running across each deploy**, so the
before/after is one continuous measurement sliced at the cutover rather
than two runs of a tool.

* **v2.2.1-rc1**, 17:09:53Z — climb guard + wire clamp.
* **v2.2.1-rc2**, 18:19:06Z — the `3+` force, plus a persistent climb
  floor.

### What rc1 proved, and what it did not

rc1 ran 63 minutes with **zero teardowns** — and that was **not
evidence**, because no `3+` arrived in the window. The pre-fix `3+`
interval is 75-90 min; the link was never tested.

rc1 also showed the climb guard firing (54 withdrawals, and the wire
clamp perfect at 0 records >4095 against 33 before) while the climbing
share stayed at **28.0 %, against 27.8 % before it existed**. Resetting
`rtt_floor` on a trip let the destination re-floor at its inflated cost
and the ladder simply resumed. rc2 makes the floor persist and latches
a `looping` flag until the cost is credible again: K1YMI's real
45-rung series goes to 29 records on the wire, 16 suppressed.

**The verification that matters is the next `3+`.**
