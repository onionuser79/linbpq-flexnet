# v2.2.1 outcome — the `3+` defects are fixed, the `-12` teardown is not

Written 2026-09-21, at the point the rc was promoted to v2.2.1 and rolled to
production. This file exists so the next session does not have to re-derive
the verdict from the pcaps, and does not mistake a fixed defect for a fixed
symptom.

## What the capture says

One continuous capture per link, started 2026-09-20 08:10Z and deliberately
left running across the rc1 (15:09Z), rc2 (16:19Z) and rc3 (19:02Z)
cutovers. Sliced with `/tmp/ba.py <pcap> <local> <peer> <cutover>` and
`/tmp/plus.py <pcap> <local> <peer>` on `iw2ohx-gw`.

`IW2OHX-12` (PC/Flexnet), around the rc3 cutover:

| | before, 10.85 h | after, 13.41 h |
|---|---|---|
| independent teardowns | 9 — **0.83/h** | 15 — **1.12/h** |
| route records out | 11714 — 18.0/min | 11818 — 14.7/min |
| route records in | 481 | 336 |
| from climbing destinations | 26.7 % | 34.5 % |
| withdrawals (`60000`) | 412 | 967 |
| finite records > 4095 | **33** | **0** |

`3+` → teardown mapping over the whole 24 h: **23 inbound `3+`, 23 followed
by an independent teardown within 120 s** (chance coverage 3.2 %,
p ≈ 3.1e-35), and **1 of 24** independent teardowns with no `3+` before it.
The node sends no `DISC` and no `SABM`; PC/Flexnet initiates 100 %.

## The conclusion, stated plainly

The short answer to `3+` was a real defect and **was not the mechanism that
ends the session.** rc2 made the answers complete (162 records where it had
sent 3-72) and the teardowns continued; rc3 fixed the premature `3-` and they
continued again, at a marginally higher rate.

What is now known that was not known before:

- Answers are correct. 157-171 unique records of a ~204-entry table, where
  the pre-rc2 code sent 3-72. Every peer's table for us was **wrong**, and
  that is fixed independently of the teardown.
- The clamp works: 33 over-limit finite records before, 0 after.
- The remaining question is **what PC/Flexnet does with a correct but large
  answer** — 157-171 records spread over 40-120 s, 16 per frame, one frame
  per 5 s to a PCF-family bucket. That is the shape to test next, and it has
  never been tested, because until rc2 it was never emitted.

## Two numbers that look like regressions and are not measurable as such

The climbing-destination share (26.7 % → 34.5 %) and the withdrawal count
(412 → 967) both rose. **The denominator changed with the fix**: a full-table
answer re-sends every destination including the climbing ones, and the climb
guard's own withdrawals are counted in the second row. Neither figure is a
clean before/after and neither should be read as `flex_climb_is_loop()`
failing. A clean measurement needs two windows with the same request
pattern — i.e. after the teardown cause is found and the sessions are long
enough to compare.

## `IW2OHX-14` — a weak signal, unattributed

2 independent teardowns in the 13.45 h after rc3 against **0** in the 10.87 h
before. One of the two is clearly peer-side: 10 inbound `SABM` in 5 s (its
own retry ladder) followed by a burst of `DM`. The other is a single `DM` at
07:15:28Z. Two events is not a rate; do not treat this as a v2.2.1 regression
without a longer window, and note that `-14` answers our `3+` exchange
without dropping at all.

## Deployment cutovers, for slicing captures that span them

| When (UTC) | Node | Version |
|---|---|---|
| 2026-09-20 13:09Z | IR2UFV | rc1 (climb guard) |
| 2026-09-20 16:19Z | IR2UFV | rc2 (`force=TRUE`) |
| 2026-09-20 19:02Z | IR2UFV | rc3 (EOB quiet window) |
| **2026-09-21 08:36Z** | **IR2UFV** | **v2.2.1** (version string only vs rc3) |
| **2026-09-21 08:41Z** | **production IW2OHX-13** | **v2.2.1**, from v2.1.42 |

The two `tfix-mon-{12,14}` captures on `iw2ohx-gw` were running before the
v2.2.1 cutover and were left running through it, so the same before/after
recipe applies. rc3 → v2.2.1 is a version-string-only change, so the only
thing that moved at 08:36Z is the link uptimes.

## Production went to v2.2.1, and why that is safe

Production `IW2OHX-13` is a leaf: explicit `FLEXNETTRANSIT NO`, `DIGIFLAG=0`,
and it has **no PC/Flexnet peer** (its FlexNet neighbours are `IW2OHX-14` and
`IW2OHX-4`, both (X)Net). With transit off the advertisement walk has one
entry — its own — so the whole of v2.2.1's advertisement-plane behaviour is
inert there. What production actually gains by moving off v2.1.42 is
**v2.2.0**: packed route advertisements and the established-guard that stops
a spurious re-INIT when BPQ recycles a peer's `LINKTABLE` slot.
