# v2.6.0 QA pass 1 — link stability, reachability, consistency (2026-10-11)

Nodes: IW2OHX-13 (LinBPQ prod, silent), IR2UFV (LinBPQ test, flexdebug),
IW2OHX-12 (LinBPQ Windows, silent), IW2OHX-14 / IW2OHX-4 / IQ2LB ((X)Net).
All three LinBPQ nodes on v2.6.0 (b516c0f).

Polling while measuring: station-dashboard cron (15 min, all nodes) + this
pass (`qa_snap.py` every 20 min for 6 h from 07:15Z, ad-hoc MCP queries,
`conntest.py`). All LinBPQ telnet ports have `DisconnectOnClose=0`.

Tools in this directory: `qa_snap.py` (one JSON snapshot of FL/L + D on all
six nodes), `qa_diff.py` (cross-node D-table diff), `ufv_log_bench.py`
(IR2UFV trace benchmark), `conntest.py` (timed `C` with far-end banner as
proof). Raw IR2UFV trace slice (process start 2026-10-09 06:38 → 2026-10-11
09:17 local, 50.7 h) is NOT committed (37 MB) — on gw `/tmp/flexnet_axudp.log`.

## Benchmark

### Link stability (snapshot ~07:15Z)

| Link | FlexNet session (our FL) | L2 per peer's `L` | Note |
|---|---|---|---|
| IR2UFV ↔ -14 / -13 / -12 | 2d02 / 2d02 / 1d22 | = | = process uptimes; 0 unexplained restarts in 50.7 h |
| -13 → -14 | **0h49** | 2d10h | session re-created ~06:25Z, L2 never dropped |
| -12 → -14 | 1d12h | 1d22h | one mid-life re-creation |
| -12 → -4 | 13h44 | 13h44 | genuine L2 reconnect |
| -12 → IQ2LB 1k2 (port 3) | **0h13** (keeper opens=38) | 9h / 8h | |
| -12 → IQ2LB 9k6 (port 4) | 1h25 (opens=3) | 9h / 8h | |

(X)Net view of the LinBPQ nodes: -14 `Q/T 3 rtt 0/5 rr+ 0.0–0.1 %` on -13,
IR2UFV, -12 — healthy. IQ2LB ↔ -12 RF: `Q/T 32/16 rtt 58/5, 27/5`,
`rr+ 1.0 % / 7.7 %`, `txq 88/95` on one port.

### Reachability

* D-table sizes: -14 208, -13 213, IR2UFV 210, -12 211, IQ2LB 208, -4 203.
  The three LinBPQ nodes cover **100 %** of -14's table; -4 is missing 5 real
  destinations at the snapshot (DA5UDI-9, SV1DZI-10, VE3LNZ-2, W4OT ×2).
* Cost vs -14 (median ratio): -13 1.40, IR2UFV 1.26, -12 1.40; no outlier > 3×.
* Timed connects, success = far-end banner received:
  **19 / 21 first-try** (FlexNet-only and NET/ROM targets, from all three
  LinBPQ nodes and -4). Banner latency **0.1–0.5 s**, 1.7 s to the
  PC/Flexnet IK1NHL-4. Failures: IR1UAW-10 (3/3, all LinBPQ nodes — see I-1)
  and CQ0PPA-5 from -13 (76 s "Failure with", stale entry — see B-2).
* IR2UFV path probes: 3042 sent, **74 %** answered.
* -4 destination churn, 20 samples × 1 min: **10 destinations vanished for
  1–11 min and came back** (DB0LHR, DB0UHF, DB0ACA ×2, DB0AAT ×2, HB9AK ×3,
  IK6IHL) — explained by B-1.

## Bugs

### B-1 (HIGH) — learned-age prune withdraws live routes every hour

`flex_learned_age_scan()` drops any learned route not re-heard for
`FLEXNET_LEARNED_MAX_AGE` = 3600 s, then runs the withdrawal walk. Its
premise ("xnet's own ageing window < 480 s") is wrong: (X)Net sends a record
only when the cost changes, so a stable route is not re-heard for hours.

IR2UFV, 50.7 h: **7037 prunes (139/h)**, age at prune 3601–3630 s (i.e.
everything stable), 3509 withdrawals (60000) fired. To the non-source peer
(-13) that is **2318 false withdrawals, median outage 7.4 min, 25 % > 1 h**;
192 still withdrawn at the end of the slice. Most-pruned: IR8CSB, IW8PGT-8/-3,
IK6IHL, OE2XGR, HB9ON-2.

End-to-end proof on the live network: -12 runs the same timer on the same
-14 feed and is -4's only FlexNet source. IR2UFV pruned DB0LHR at 07:29:04Z →
-4 lost it at 07:29:10Z; DB0AAT 07:32–07:34 → 07:34:17; HB9AK ×3 07:34:34 →
07:35:18.

Fix direction: (a) mitigation — raise the age to many hours (cuts the churn
proportionally, keeps the 2026-09-17 IR2UFX safety net); (b) proper — when a
route goes quiet, ask the peer with `3+` and prune only what its answer
(closed by `3-`) omits. (b) needs a capture first: in this run **neither side
sent a `3+` in 50.7 h**, and whether (X)Net answers our `3+` mid-session is
untested. Do not reuse with PC/Flexnet semantics (quiesce gate) unverified.

### B-2 (MEDIUM) — the D table never ages; reaped sessions leave orphans

`FlexNetDests[].last_updated` is written but nothing expires on it. Two ways
an entry outlives its source:

1. The ghost reaper sets `via_session_idx = -1` but leaves the RTT finite
   (`FlexNet_CloseSession` sets INFINITY). After -13's -14 session was
   re-created at ~06:25Z, FL showed **Routes 80 of 213**, 102 after 31 min —
   orphans heal only when -14 happens to resend that destination, and
   stable ones never are (see B-1).
2. Routes -14 no longer holds stay forever: -13 has `CQ0PPA 5-5 4392`,
   `CQ0PPA 8-9 4788`, `N2MH 0-0 4995` via -14; -14 has no `D` entry for
   either. `C CQ0PPA-5` from -13: "Failure with" after 76 s.

Also: `D < IW2OHX-14` lists all 213 (matches via_callsign) while FL counts 80
(matches via_session_idx) — the two views disagree.

Fix direction: when a learned entry is pruned or a session is reaped,
recompute the dest from the remaining learned sources; INFINITY when none;
purge INFINITY rows after a hold time.

### B-3 (MEDIUM) — synthetic neighbour entry clobbers the neighbour's SSID range

`FlexNet_InitSession` inserts the neighbour as a direct destination with
`ssid_lo = ssid_hi = <link SSID>` → `IR2UFV 0-0`, beside IR2UFV's own
advertisement `IR2UFV 0-8` (IR2UFV only ever sends `3IR2UFV081`, 4563×).
Both ranges sit in the D tables of -12 and -13 and are re-advertised. Result
downstream: -4 holds `IR2UFV (0-0)` only (route via -12), IQ2LB flipped
between 0-0 and 0-8 between two samples; IR2UFV-8 is the BBS (`UFVBBS`).
Connect impact not reproduced — -4 still reached IR2UFV-8 (0.2 s) — so this
is table inconsistency, not yet an outage.

Fix direction: when the neighbour advertises a range containing its link
SSID, the advertised range replaces the synthetic one; never advertise the
synthetic row while a covering real one exists.

### B-4 (LOW) — `FlexNet_FindRoute` takes the first match, not the best

Linear scan returns the first finite entry whose range covers the SSID —
not the cheapest, and it will pick a B-2 orphan or a B-3 duplicate. Pick
min-RTT among matches.

### B-5 (OPEN, MEDIUM) — FlexNet sessions re-created under a live L2

-13→-14 and -12→-14 once each, -12→IQ2LB RF repeatedly (session 13 min /
1h25 vs L2 9 h / 8 h at IQ2LB, keeper `opens` unchanged during 31 min while
the session restarted). Each re-creation re-INITs the peer, re-seeds the
whole table (over 1k2 on port 3) and orphans destinations (B-2). Not seen on
IR2UFV (flexdebug) in 50.7 h: its 3 reaps are real (`L2STATE=0`, -12
restarts). The silent nodes give no reason.

Code-confirmed latent defect, impact on this symptom unproven: the reaper
runs inside `FlexNet_Timer()`, called from `L2TimerProc()` every **100 ms**,
so `FLEXNET_REAP_STRIKES 3` = **300 ms**, and it counts any
`L2STATE != 5` as bad — including **6 = REJ sent**, a connected sub-state
(L2Code.c:2734/2790, back to 5 at 3916). On an RF link with FRACK 10 s a
single lost frame holds state 6 for seconds.

Fix direction: treat 6 as healthy; make the hysteresis time-based (seconds,
not ticks). Before closing: add per-session restart-reason counters to `FL`
that also print on the silent build, or run -12 with flexdebug for a day.

## Improvements

* **I-1 NET/ROM always wins over FlexNet, with no fallback.** `CMDC00`
  tries APPL → NODES → FlexNet. IR1UAW-10 is in NODES (`UAWNET`); the
  NET/ROM CREQ to -14 is never answered (IR2UFV 07:36:17Z `pid=CF len=55`,
  no CACK), so all three LinBPQ nodes fail after 90 s of silence while
  `C 3 IR1UAW-10 V IW2OHX-14` (FlexNet/L2) gets the banner in 0.5 s and -14
  connects directly. Options: fall back to the FlexNet route on L4 connect
  timeout, or a `FLEXNETPREFER` switch.
* **-12 port 3 (1k2) carries a full second copy of the table to IQ2LB**,
  which already learns everything on the 9k6 port; every B-5 re-creation
  re-seeds ~200 records (~20 s airtime at 1200 bd). Try
  `FLEXNETLINK=IQ2LB F=` on port 3 (own destinations only).
* **Expose stability counters on the silent build** (session re-creations by
  reason, prunes, withdrawals sent) — today the two production nodes cannot
  explain their own restarts.
* -13 startup: `Telnet Server bind(sock) failed port 8772 Error 98` — the
  port is held by the IR2UFV instance. Give one instance a distinct value.

## Not ours, seen on the way

* -14 ↔ IW2OHX-3 (flexnetd): `Q/T 121 rtt 240/2` — degraded.
* -14 ↔ DK0WUE: reconnecting (52 s connect, `rtt 4/600`).
* -14 ↔ IW2OHX-15 (INP3) `rr+ 70 %` on a 2-min-old link; IQ2LB ↔ -15
  port 1 `rr+ 18.9 %`, `txq 93/94`.
* `IQ2LB 0-5 T≈2250` on IR2UFV/-13 is advertised by -14 (re-sent ~hourly).

## Retracted during the pass

* "-12 sends CE frames with PID F0" and "trace lines out of time order":
  both from an awk range that crossed day boundaries in a multi-day trace
  with time-only stamps. The current run has 0 CE-via-F0 and 20 785 PID=CE
  frames from -12. **Slice the trace from the last `PATH-CACHE-LOAD`.**
* "-12 NODES broadcast storm": 27 frames per 30-min interval, paced
  (54/h); port 3 has `QUALITY=0`.

## Fix status — branch `v2.6.1-qa-fixes` (2026-10-11, same day)

All five on IR2UFV (flexdebug) since 10:56 local; production `-13`/`-12`
still v2.6.0. Unit runners 14/14, each new one mutation-tested; build
warnings unchanged at 52.

| Bug | Commit | Fix | Live evidence |
|---|---|---|---|
| B-1 | `0ab6b0e` | learned routes have no lifetime (age prune removed) | 0 prunes after deploy (was 139/h) |
| B-2 | `cdb51d7` | `flex_dest_reelect()`: fail over / infinity when the source goes | FL Routes 1+55+154 = D total 210 |
| B-5 | `45e1b90` | L2STATE >= 5 is connected (6 = REJ outstanding); reaper grace 5 s, was 300 ms; FL `Sessions:` history | FL shows lifecycle; soak pending |
| B-3 | `0191edc` | neighbour entered at INIT as `<link SSID>-<INIT max>` | IR2UFV enters 12-12/13-13/14-14 at INIT; still offers `IW2OHX 14-14` to -12 |
| B-4 | `d56bcf8` | `C`/`D <call>` take the cheapest covering row | unit-tested |

### Two attempts that the wire overturned

**FLEXNETRESYNC (B-1 v1, `5c60b38`, removed in `0ab6b0e`).** A periodic
`3+` + own record + `3-`, sweeping what the answer omitted. Against
LinBPQ `-12`: full answer, `named=104/104`. Against (X)Net `-14`: the
first request was answered with a bare `3-` 4 ms later
(10:11:43.097 → .101), the second with two record frames (~40 of ~170,
many cost 0) and `3-` — a partial batch. Pushes arriving while the
request was open counted as "named": `83/167`, 0.3 % below the 50 %
threshold at which the quiet-window rule would have swept ~84 live
routes. Also: across IR2UFV's whole console history `-14` had sent `3+`
40 times and `3-` never. And the IR2UFX incident the age prune was
written for turns out to be a count-to-infinity phantom
(`research/local_calls_2026-09-28/FIELD_TEST.md`), not a silent drop.
Spec §8.2.6 records the (X)Net answer.

**"Never offer the invented neighbour entry" (B-3 v1, `f8a5696`,
reverted in `075f4aa`, never deployed).** Captures of `-14 → IR2UFV`
contain `IW2OHX 13-13` and `3-3` but never `IW2OHX 14-14`: (X)Net does
not advertise its own call to its neighbour. Not offering the entry we
create for a neighbour would have removed `IW2OHX-14` from everything
that learns it through us — `-4` learns it only via `-12`. The captures
also showed (X)Net's own rule (`IR2UFV 0-8` = link 0 + INIT max 8),
which is what v2 implements.

### B-3 seen live while it was being fixed

After IR2UFV's restart at 10:56, `-14` held a single `IR2UFV (0-0) T=2`
via `-12` (v2.6.0, still advertising the invented `0-0`) and **no 0-8**,
although IR2UFV is its neighbour and keeps sending `IR2UFV 0-8 1`.
`C IR2UFV-8` (the BBS) from `-14` then took **80 s** to set up
("Interlink setup"), against 0.2 s in the morning and 0.1 s for
`C IR2UFV`. Clears when `-12` runs the fix.

### B-6 — found by B-3's fix, fixed the same day (`a4a3a96`)

With `-4` finally holding `IR2UFV 0-8`, `C IR2UFV-8` from `-4` took the
FlexNet path and failed 3/3 (`link failure` after 14 s): `-12` forwarded
correctly (`IW7EAS-2>IR2UFV-8 via IW2OHX-4* IW2OHX-12* IR2UFV`), but
IR2UFV did not treat its own SSID 8 as its own — `L2FWD-DECLINE`, stock
digipeat, `no ARP match for 'IR2UFV-8'`, sent to `-14`. Fix:
`FlexNet_IsLocalCall()` also answers for SSIDs of our own range other than
the node call, so the v2.3 local-call path delivers them and replies go
`via IR2UFV*`. After deploy (12:07): `-4 → IR2UFV-8` 1.6 s then 0.1 s,
reply `IR2UFV-8>IW7EAS-2 via IR2UFV* IW2OHX-12 IW2OHX-4`. Affects only nodes
whose range is wider than their own SSID (production `-13`/`-12` are not).

Also seen: after `-12`'s restart a SABM collision left IR2UFV's link in
state 2 for 83 s — stock BPQ answers a SABM in state 2 (no session yet)
with UA and stays in state 2 (`L2Code.c` "repeat UA"), and
`ConnectFailedOrRefused()` returns without clearing a downlink that has no
circuit. Cleared by `-12`'s retry timeout. Not ours; candidate for a
keeper-side guard.
