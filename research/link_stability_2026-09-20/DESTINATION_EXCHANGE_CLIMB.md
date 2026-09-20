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

## The trigger

All **8** independent teardowns follow one of our outbound LT frames
within 90 s (7 within 60 s), and exactly those 8 of our 98 LT sends are
followed by a teardown:

```
09:10:10 -> 50.4s   12:16:07 -> 15.0s   15:04:18 -> 32.9s
09:37:18 ->  7.6s   13:45:45 -> 38.1s   16:34:13 -> 38.1s
10:56:07 -> 78.1s   14:59:44 -> 33.0s
```

Chance coverage of eight 90 s windows over 98 sends in 8.6 h is 28.4 %,
so p ≈ 4×10⁻⁵. The LT send is not itself harmful — 90 of 98 are
harmless. It is the moment PCF re-evaluates the link, and what it
re-evaluates is how much we have been sending it.

## The cause: 35.7 % of what we advertise is a routing loop

In the 60 s before each independent teardown we push a **median of 40
route records** at PCF against a baseline median of **5** — 8× — and in
six of the eight cases PCF was mid-dump toward us at the time (51–136
records inbound).

Over the whole capture:

| | records |
|---|---|
| we → PCF | **6701** |
| PCF → we | 481 |
| ratio | **13.9 : 1** |

For 204 destinations. Breaking down what those 6701 records *say*:

| | share |
|---|---|
| first sighting | 3.0 % |
| identical to what we last sent | 16.6 % |
| changed value | **80.3 %** |
| …of those, moved < 10 % | 0.5 % |

So the 10 % jitter threshold is not leaking — the values really are
moving. They are moving because they are **climbing**:

```
K1YMI /0?   279 records, 235 distinct values, 109 → 4910
IW2OHX/44   135 records,  83 distinct values,   7 → 2384
WA2UPK/24   117 records, 109 distinct values, 329 → 4978
DB0BIB/0:   116 records,  28 distinct values,  13 → 2312
```

K1YMI's actual series:

```
270 2660 270 342 385 433 548 781 1113 2257 1783 798 1136 1618 2304
173 137 173 137 154 220 314 248 279 447 637 807 459 315 355 506 577
720 1040 1481 1642 2078 2338 2959 3329 1898 2135 2702 3848 4329 …
```

A geometric climb of roughly ×1.2–1.4 per step, collapsing back to a low
value and climbing again. That is a distance-vector count-to-infinity:
the cost accumulates one lap at a time, and each lap it passes through
us we advertise the new rung.

**43 of 204 destinations climb this way, and they alone are 35.7 % of
our 6701 records.**

This is exactly the failure `CLAUDE.md` already listed as open — *"the
purely relative 10 % advertisement jitter threshold (high-RTT
destinations never settle, so they re-advertise forever)"* and *"the
absence of a hold-down on transitions to infinity"* — and the mechanism
`RFC §13.3` predicted: poison-reverse undoes itself, the peers echo our
withdrawal back, we re-learn it and advertise a finite cost again.

A 10 % relative threshold cannot stop this. Every rung of a ×1.3 ladder
clears 10 % by construction.

## Two hypotheses that the data killed

Recorded because both are plausible, both have supporting comments in
the source, and both are wrong here.

**1. `RTT=60000` on the wire.** We emit the withdrawal sentinel as a
literal `60000` — 310 times in the capture. PCF's own maximum in 481
records is `1080`, and its cost ring saturates at `4095` (a 12-bit
field), so the value is genuinely unrepresentable to it. But of **47**
over-limit bursts only **3** were followed by a teardown within 120 s,
and most teardowns had no over-limit record for 15–30 minutes before
them. Real hygiene defect, not this fault. Clamped anyway — see below.

**2. The keepalive has no CR.** `flex_build_keepalive` emits 241 B
(`'2'` + 240 spaces, no terminator) to every peer; PCF emits 201 B
(`'2'` + 199 spaces + `CR`). The comment above that function states that
**PCF silently discards KAs whose last byte isn't CR**, and that this
caused *"DISC every ~5 min"* — the per-session shape mirror added in
v2.1.10 was removed again in v2.1.13. The wire says the concern no
longer applies: PCF answers **1118 of 1118** of our 243-byte
CR-less keepalives, median 0.01 s. Left alone.

> **Method note.** Both were tested by trying to *falsify* them, and
> both died on the second test. The correlation that survived
> (advertisement volume) is also the only one with a plausible
> mechanism on PCF's side, and it is the one the source already
> predicted.

## The fix

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

IR2UFV only, **v2.2.1-rc1**, 2026-09-20T17:09:53Z. Production IW2OHX-13
untouched. The `tfix` capture and `linkstab` were **deliberately left
running across the deploy**, so the before/after is one continuous
measurement sliced at the cutover rather than two runs of a tool.
