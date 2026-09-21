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

The two `tfix-mon-{12,14}` captures on `iw2ohx-gw` ran continuously from
2026-09-20 08:10Z and were left running through the v2.2.1 cutover, so the
same before/after recipe applies to everything up to that point. rc3 →
v2.2.1 is a version-string-only change, so the only thing that moved at
08:36Z is the link uptimes.

## ⚠ The capture era ends 2026-09-21T09:39Z, and the topology changed after it

**`tfix-mon-{12,14}` stop at 2026-09-21T09:39:03Z** (both files are intact
and hold 27.5 h). They were killed by a `pkill -x tcpdump` aimed at a
short ad-hoc capture — all three conclusions above were computed before
that point and are unaffected, but there is a ~26 min hole and the era
ends there.

It ends at the right place anyway, because **two topology changes landed
immediately after and neither belongs in the same measurement**:

| When (UTC) | Change |
|---|---|
| 2026-09-21 09:32Z | production `IW2OHX-13` promoted leaf → **FlexNet router** |
| 2026-09-21 10:01Z | the **`IR2UFV ↔ IW2OHX-4` peering re-enabled** (disabled 09-20 to narrow this run to `-14`/`-12`) |

So the router era has its own captures, armed 2026-09-21 ~10:07Z:

| Path | Covers |
|---|---|
| `/tmp/rtr-mon-14`, `-12`, `-4` | each IR2UFV link, `udp port 10075`, `-W 8 -C 10` |
| `/tmp/prod-router-watch/link.pcap*` | production's AXIP port, `udp port 10093` |
| `/tmp/prod-router-watch/fl.log` | production's `FL` transit section, every 5 min |

**Do not compare a router-era capture against the `tfix` ones for the `3+`
question.** `IR2UFV` gained a third peer and production became a transit
node in between, so advertisement volume, `learned[]` size and the cost
landscape all moved for reasons unrelated to the `3+` answer.

### What the restored `-4` peering did, because it is large

`IW2OHX-4` has no direct FlexNet link to `-14`, and both LinBPQ nodes
advertise ~206 destinations to it at cost 3 while `IW2OHX-12`'s rtt is 151.
`-4`'s chosen next hop, before → after:

| Next hop | before | after |
|---|---|---|
| `IR2UFV` | (peering down) | **157** |
| `IW2OHX-13` | 66 | 51 |
| `IW2OHX-12` | 138 | **0** |

`-4` now reaches essentially nothing through PC/Flexnet. Both LinBPQ nodes
were verified to actually carry it — pinned connects 3 and 4 hops beyond
each of them succeed, and **IR2UFV's L2 forwarding counters left `0/0/0`
for the first time in its life** (`extended=15 contracted=16 declined=0`),
because the restored peering finally made it somebody's best path. That
also retires the standing note that §6/transit "can't be tested because
IR2UFV never wins on cost".

Two consequences to weigh: `-4`'s reachability now depends on our two
nodes rather than on PC/Flexnet, and `-4` is a RAM-only TNC4e that flaps
on its own (an open item — see the 09-19 investigation).

## Production went to v2.2.1, and why that is safe

Production `IW2OHX-13` is a leaf: explicit `FLEXNETTRANSIT NO`, `DIGIFLAG=0`,
and it has **no PC/Flexnet peer** (its FlexNet neighbours are `IW2OHX-14` and
`IW2OHX-4`, both (X)Net). With transit off the advertisement walk has one
entry — its own — so the whole of v2.2.1's advertisement-plane behaviour is
inert there. What production actually gains by moving off v2.1.42 is
**v2.2.0**: packed route advertisements and the established-guard that stops
a spurious re-INIT when BPQ recycles a peer's `LINKTABLE` slot.
