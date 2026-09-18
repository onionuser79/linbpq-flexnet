# BLOCKER for Monday — the -12 and -4 links keep resetting

**Recorded 2026-09-18, end of phase, at Marco's direction.**

> "If we don't solve this, it doesn't make sense to go ahead with
> implementing other features."

**Nothing else gets built until this is fixed.** The destination table is
not trustworthy while the links keep re-negotiating: destinations
disappear and come back, and a connect works or doesn't depending on
when you try it. Every feature measured on top of that is measured on
sand — and several of today's measurements had to be discarded for
exactly this reason.

## The evidence, and what it rules out

Operator captures at end of phase:

**From IW2OHX-14** — its own link table:
```
14:IR2UFV       3 Q 254  ...  1h 45m
14:IR2UFV       1 F   3  ...  1h 45m
 1:IW2OHX-12    4 F   1  ...  4d 10h
 2:IW2OHX-13    1 F   3  ... 15h 27m
```

**From IW2OHX-4**:
```
 4:IR2UFV       1 Q 255  ...  5s 000      <- just reset
 4:IR2UFV       7 F   3  ...  5s 000      <- 7 destinations, not ~199
 3:IW2OHX-12    1 F 101  ... 12m 08s      <- also churning, cost 101
 2:IW2OHX-13   98 Q 254  ...  1h 13m
```

**From IW2OHX-12** (`L *`, with its per-link cost rings):
```
IR2UFV  0-8  ( 883/5)   16m,16s P 5
                 600 600 4095 1 1 1
IW2OHX  4-4  ( 151/151) 13m, 8s P 4
                 600 1 1 1
IW2OHX 14-14     1/1     4d,10h P 3
                 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
IQ2LB   6-6      1       6d, 5h P 2 @
                 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
```

Read the uptimes together and the pattern is unambiguous:

| Link | Uptime | Verdict |
|---|---|---|
| IW2OHX-12 ↔ IW2OHX-14 | **4d 10h** | rock solid |
| IW2OHX-12 ↔ IQ2LB-6 | **6d 5h** | rock solid |
| IW2OHX-14 ↔ IW2OHX-13 | 15h 27m | solid |
| IW2OHX-14 ↔ IR2UFV | 1h 45m | tolerable |
| **IW2OHX-12 ↔ IR2UFV** | **16m** | churning |
| **IW2OHX-12 ↔ IW2OHX-4** | **13m** | churning |
| **IW2OHX-4 ↔ IR2UFV** | **5s** | churning |

**This rules out "PCF cycles everything".** -12 holds -14 for four days
and IQ2LB for six. It only churns against **IR2UFV and IW2OHX-4**.

It also rules out "IR2UFV is simply unstable": its link to -14 held for
1h45m in the same period.

**IW2OHX-4 is in both churning pairs.** It is the RAM-only TNC4e that was
physically reset today and has flapped repeatedly since. So one
hypothesis is that -4 is the primary fault and IR2UFV↔-12 churn is
secondary. But that does not explain -12↔IR2UFV on its own, so it cannot
be assumed.

## The cost rings are the sharpest clue

PCF's ring for IR2UFV reads `600 600 4095 1 1 1`, and for -4
`600 1 1 1`. The `1`s are healthy. The `600`s and the saturated `4095`
are what hold the advertised cost at **883/5** (against -14's `1/1`).

Two things already known about those numbers, from earlier work:

* **`600` is what a re-INIT seeds.** The v2.1.16 reaper was written
  specifically to keep a session established so that "the peer never
  sees a re-INIT — so its link-cost ring isn't reseeded with `600 …`
  outliers". Every re-INIT we send therefore *poisons* PCF's cost view
  of us, and a short-cycling link re-INITs constantly.
* **`4095` is a saturated entry from a negative-delta wrap**, the failure
  recorded in `feedback_pcf_lt_rate_limit_floor` (LT interval below
  ~320 s produced exactly this).

Also note both churning rings are **short** (6 and 4 entries) where the
stable ones are full at 16 — the ring is being restarted, not just
polluted.

## Where to start on Monday

Order matters; this is a stability problem, so measure before changing.

1. **Establish who tears down, per link.** For each of the two pairs,
   who sends DISC/SABM first — us, or them? Monitors 2 and 3
   (`/tmp/mon-4`, `/tmp/mon-12`) already capture one file per peer; a
   teardown is `ctl=DISC (0x43)` / `SABM (0x2F|0x3F)`, and direction is
   `In`/`Out` in the capture. This single fact splits the problem in
   half and we do not yet have it for -12.
2. **Separate -4's hardware flapping from the -12 relationship.** -4 was
   reset today; if its churn is hardware, IR2UFV↔-12 should still churn
   with -4 quiet. Watch a window where -4's link is stable.
3. **Count our own re-INITs to -12** and correlate with the `600`s
   entering its ring. If every re-INIT costs us a 600, then every
   short-cycle is self-reinforcing: churn → re-INIT → cost 883 → PCF
   prefers other paths and cycles the quiet link → churn.
4. **Then, and only then**, the advertisement-volume defect
   (144 adverts/min against PCF's 12/min bucket drain, in
   `fix_finder_2026-09-18/PHASE_CONCLUSION.md`). It is very likely a
   *contributor* here — a permanently saturated link is a permanently
   busy one — so it may turn out to be the same bug. But it was measured
   during churn, so treat the number as provisional.

## What NOT to do

* Do not build further features on top of this.
* Do not trust any before/after measurement taken across a link reset —
  several of today's had to be thrown away. Check the process pid and
  the link uptimes first, and treat a negative counter delta as a
  restart marker rather than data.
* Do not read PCF's cost to us as a verdict on our code until the ring
  stops being reseeded; `883/5` is mostly two `600`s and a `4095`.
