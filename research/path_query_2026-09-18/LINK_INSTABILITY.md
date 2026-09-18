# Why the FlexNet links to IW2OHX-14 and IW2OHX-12 keep dropping

**2026-09-18, from a 12-hour overnight watch** (48 `quad-watch` samples,
full `flexdebug` console). The instability is not a link-layer problem.
It is a **self-sustaining advertisement storm**, and the link drops are
its symptom.

## The evidence

Overnight alert counts from `quad-watch`:

| Check | Count | What it saw |
|---|---|---|
| A3 SPLIT_HORIZON | 74 | `learned 102 from -14 yet advertise 81 back` |
| A1 ADVERT_MISMATCH | 77 | `we advertise 198, -14 installed 3` |
| A4 SESSION_RESTART | 20 | all three peers, through the night |
| A12 COST_DRIFT | 2 | -14's cost to us: `3 -> 303` |

Advertisement volume from the console:

```
16897 ADVERT-CHECK ... FIRED
 4867 ADVERT-CHECK ... SUPPRESSED      <- the jitter threshold stops only 22%
 2187 fired with exp=60000             <- withdrawals (route gone to infinity)
 1184 fired with last=60000            <- and back again
 7707 fired at peer=IW2OHX-12          <- the PCF peer, slowest bucket of the three
```

PCF's advertisement queue, sampled every ~5 min all night:

```
queue>0 in 4165 of 4165 bucket ticks (100%)
... 176 -> 140 -> 98 -> 65 -> 29 -> 0 -> 183 -> ...
```

It reaches zero only momentarily before another ~180-route burst lands.
At the PCF bucket rate (one record per 5 s) 198 routes take **16.5
minutes** to drain, and the bursts arrive roughly every 30 minutes.

PCF's own link cost to us, from its `L` table, against its other peers:

```
2026-09-17 16:03   IR2UFV 588/5    IW2OHX-14 1/1   IW2OHX-4 68/68
2026-09-18 03:35   IR2UFV 1324/5   IW2OHX-14 1/1   IW2OHX-4 1/1
```

Ours got **worse** by 736 while both its (X)Net peers settled to 1/1.

## The loop

1. A session restarts. The learned table is **wiped and rebuilt from
   scratch** — visible in the console as the counter collapsing:
   `total learned=141` … `total learned=1`.
2. Every re-learned route fires an advertisement to the other two peers.
   ~198 destinations x 2 peers.
3. PCF's bucket drains one record per 5 s, so the burst occupies the link
   for ~16 minutes and the queue never empties.
4. Meanwhile routes flap through infinity — 2187 withdrawals and 1184
   returns — each firing again.
5. The link to PCF is now carrying continuous FlexNet control traffic.
   PCF's measured cost to us climbs (588 -> 1324) and it **cycles the
   link**: 7 inbound SABMs (`ctl=0x3F`) from -12 overnight, against one
   each from -14 and -4.
6. That restart takes us back to step 1.

The restarts average one every ~35 minutes, which is the same period as
the advertisement bursts. Cause and effect feed each other.

## Three contributing defects

**1. A session restart discards everything learned from that peer.** The
routes are almost certainly still valid — the peer is right there,
reconnecting. Wiping and re-learning converts a link blip into a
full-table re-advertisement. This is the biggest single amplifier.

**2. The jitter threshold is purely relative, so high-RTT destinations
never settle.** `FLEXNET_REFRESH_THRESHOLD_PCT` is 10%:

```
ADVERT-CHECK peer=IW2OHX-12 dest=N4FLA-0/4 exp=3535 last=2792 delta=743 FIRED
```

N4FLA sits at RTT ~3500 and jitters by hundreds naturally, so it clears
10% on nearly every update and re-advertises forever. Anything far away
(N4FLA 3535, DB0TOD 990, W2KPQ ~150) behaves the same. It needs an
absolute floor alongside the percentage, scaled so that distant noisy
destinations are quiet unless they genuinely change.

**3. Infinity transitions are advertised immediately.** A route going to
60000 and back is 3371 of the fired events. `FLEXNET_POISON_HOLDDOWN`
already exists for our own poison-reverse; the same hold-down should
gate any transition to infinity, so a link blip does not withdraw 198
routes before it recovers.

A fourth, smaller one: **the per-peer queue does not coalesce.** A
destination can be queued several times over while the bucket trickles
it out, so the backlog contains stale duplicates. Coalescing by
destination would shrink the burst without changing any policy.

## What this is NOT

* Not the L2 link. The AXIP transport is fine — the drops are FlexNet
  peers deliberately re-establishing (`SABM`), not frames failing.
* Not the linbpq<->linbpq teardown in
  `../l2_forwarding_2026-09-17/IR2UFX_LINK_TEARDOWN.md`. That one is a
  different fault, on a link that carries no advertisement load.
* Not L2 forwarding, which stayed correct throughout
  (`extended`/`contracted` tracking each other, `declined=0`).

## Suggested order of work

Fix 1 first and re-measure before touching anything else: if a restart
stops wiping the table, steps 2-6 of the loop lose their fuel, and the
remaining two defects may turn out to be tolerable on their own.
