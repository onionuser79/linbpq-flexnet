# The -4 and -12 links fail for opposite reasons

**2026-09-18.** From a 4h40m AXUDP capture (09:07–13:47 UTC) on all three
FlexNet links at once, plus 6h40m of `flexdebug` log. This answers step 1
of `../OPEN_NEXT_link_instability.md` — *who tears down first, per link* —
and the answer splits the blocker into two unrelated faults.

## Method

`tools/axudp_teardown.py` decodes the AXUDP transport straight out of the
pcap: BPQ's `SendFrame()` hands `&buff->DEST[0]` to `sendto()`, so the UDP
payload is a bare AX.25 frame with no AXUDP header of its own. Direction
comes from the **IP header**, not the callsigns, because a digipeated
frame carries someone else's calls while still telling us which host
transmitted it.

That distinction is not cosmetic. Counting every frame, `-14` looked like
69 outbound SABMs; counting only frames **without digipeaters**, it is 2.
The other 67 were user connects passing through us. Any teardown census
that does not exclude transit is wrong, and wrong in the alarming
direction. `--link-only` is what enforces it.

## Result

Own L2 sessions only, 09:07–13:47 UTC:

| Link | our DISC | their DISC | our SABM | their SABM | Out I | In I | verdict |
|---|---|---|---|---|---|---|---|
| IR2UFV ↔ **IW2OHX-12** (PCF) | **0** | **6** | 0 | 8 | 3882 | 862 | peer tears down, always |
| IR2UFV ↔ **IW2OHX-4** (TNC4e) | **33** | 1 | 54 | 22 | 6001 | 497 | **we** tear down, 33:1 |
| IR2UFV ↔ IW2OHX-14 ((X)Net) | 0 | 1 | 2 | 0 | 1167 | 1097 | stable |

Two different faults wearing one symptom. Note also how cleanly the
I-frame ratio sorts them: -14 is balanced at 1.06 and never cycles; -12
is 4.5:1; -4 is 12:1 and cycles hardest. We are the talker on both
churning links.

## IW2OHX-4 — we hang up on a peer that is only slow

Every one of our 33 DISCs has the same shape:

```
12:42:36  Out I  F N(S)=3          12:42:36  In RR F N(R)=4     acked at once
12:42:38  Out I  F N(S)=4          12:42:38  In RR F N(R)=5     acked at once
12:42:40  Out I  F N(S)=5          (nothing)                    peer goes quiet
12:42:43  Out RR F N(R)=4   +3.04s
12:42:46  Out RR F N(R)=4   +3.05s
12:42:49  Out RR F N(R)=4   +3.05s
12:42:52  Out RR F N(R)=4   +3.04s
12:42:55  Out DISC                                              we give up
12:43:39  In  I    N(S)=4                                       -4 comes back
12:43:39  Out DM                                                too late
```

Five polls at exactly `FRACK`, then we close. IR2UFV's AXUDP port had
`FRACK=3000` and `RETRIES=5` — **15 seconds of patience**. The peer
returned **59 seconds** later, still on the same N(S) sequence, so it had
never lost the session. We tore down a link that was going to recover.

The classifier found no retransmission storm and no inbound DM before the
teardown: this is plain N2 exhaustion against a peer that stalls, not the
`L2Code.c:4028` stuck-window path from
`../l2_forwarding_2026-09-17/IR2UFX_LINK_TEARDOWN.md`.

Then the real cost lands: the session comes back, and we re-seed the peer
with the whole ~210-destination table, because `advertised[]` is cleared
on every session death. At ~10-minute intervals, forever.

**Fixed 2026-09-18 by configuration, not code:** `RETRIES` 5 → 25 on
IR2UFV's AXUDP port (75 s of patience at the unchanged `FRACK=3000`), and
`L4TIMEOUT` 60 → 120 to keep the `L4TIMEOUT > FRACK x RETRIES` invariant
the config file itself documents. `FRACK` was deliberately left alone so
only one variable moved and retransmission latency after genuine loss is
unchanged.

The cost of the new setting is bounded and real: a genuinely dead -4 now
holds a session, and its destinations stay advertised, for up to 75 s
instead of 15 s. That is the trade being made, and it is much cheaper
than a full table re-dump every ten minutes.

## IW2OHX-12 — PC/Flexnet drops a link that is working perfectly

The opposite. Every PCF DISC arrives on a healthy link, moments after PCF
has acknowledged our traffic:

```
09:45:49  Out I  F N(S)=6   ->  In RR F N(R)=7   ->  +0.03s  In DISC
09:46:49  Out I  F N(S)=5   ->  In RR F N(R)=6   ->  +0.22s  In DISC
11:42:34  Out I  F N(S)=7   ->  In RR F N(R)=0   ->  +0.02s  In DISC
```

No loss, no timeout, no stall — PCF acks and hangs up. Our timers are
irrelevant here, and no change on our side can prevent it directly.

What we *can* see is why we look expensive to it. Our I-frames to -12
arrive at a steady **one per ~5 s** — the PCF token-bucket rate — and
they never stop, because the queue never empties. PCF measures link cost
from inter-event timing ([[feedback_pcf_cost_ring_inter_event]]), so a
permanently busy link is a permanently expensive one; hence `883/5`
against (X)Net's `1/1`, and hence the cycling. The lever is volume, not
timers.

## What the advertisement volume actually consists of

This corrects `../fix_finder_2026-09-18/PHASE_CONCLUSION.md`, which named
the relative 10 % jitter threshold as "the one defect left". Breaking
20 464 `ADVERT-CHECK` decisions down by cause over 6h40m
(`tools/`-side `advert_breakdown.py` shape):

| peer | total | **first-time** | jitter | withdraw | restore | suppressed |
|---|---|---|---|---|---|---|
| IW2OHX-4 | 20464 | **16325 (80 %)** | 2603 | 458 | 555 | 523 |
| IW2OHX-12 | 9624 | **4806 (50 %)** | 2928 | 531 | 414 | 945 |
| IW2OHX-14 | 855 | 270 | 355 | 100 | 41 | 89 |

A `last=-1` fire is a destination this peer has never been told about —
i.e. a **re-dump after a session re-init**, not jitter. There were 76
`SEED` events in the window; 16325 / 211 routes ≈ 77 re-dumps. They match.

So the dominant cost is the re-dump, not the threshold. And of the jitter
that does exist, **56–58 % is `delta > 20` ticks (>2 s)** — genuinely
large RTT swings, which is what a congested link produces. Raising the
absolute floor would catch the `delta <= 2` cases: about 16 % of jitter,
under 3 % of total traffic. Worth doing eventually; not the fix.

**The re-dump cannot simply be skipped.** `advertised[]` is cleared on
purpose, and adopting it the way `learned[]` is adopted would be wrong
here: the peers demonstrably *do* drop their view of us when the link
drops (-4's `L` showed 7 destinations via IR2UFV seconds after a reset,
then 207 twelve minutes later), and PC/Flexnet never sends `3+` to ask
for a refresh. Skipping the re-seed would leave PCF permanently blind.

The re-dump has to become cheaper, not disappear — and the first-order
way to make it cheaper is to stop triggering it, which is what the -4
timer change does.

## Destination table vs reality

The first connect probes, run against destinations the table marked with
a resolved path (`!`):

```
IW2OHX-4   CONNECTED 6s
IW2OHX-14  CONNECTED 6s
VA3BAL-7   TIMEOUT  63s  !
VA3BAL-8   TIMEOUT  63s  !
VA3BAL-9   TIMEOUT  12s  !
VA3BAL-10  ERROR         !
```

The `!` marker means a path was resolved, and it is not a reachability
claim — but it is being read as one, and that is exactly the "connects
work or don't depending on when you try" complaint. Quantifying this
properly over 24 h is what `tools/linkstab.py` is for.

## Open after this

1. Does the `RETRIES` change actually collapse our outbound DISC count to
   -4? Falsifiable: re-run `axudp_teardown.py --link-only` on the post-fix
   capture and compare against the 33:1 above. **If our DISCs do not drop,
   this diagnosis is wrong.**
2. -12 needs a volume reduction, not a timer. Candidates in order of
   expected effect: coalesce the per-peer queue by destination; seed
   direct neighbours first and trickle the rest; then the absolute jitter
   floor.
3. The hold-down on transitions to infinity (RFC §13.3) is still unbuilt;
   withdraw+restore is 989 of the -4/-12 fires, about 7 %.
