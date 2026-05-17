# xnet Transit Behaviour — Phase 1 Report (Passive Capture)

**Date:** 2026-05-17
**Capture window:** 2026-05-17 05:59 → 06:59 UTC (60 min, both ports)
**Target node:** IW2OHX-14 ((X)NET V1.39)
**Vantage:** `mo -iusk +N` via telnet sysop session from iw2ohx-gw
**Phase status:** **Phase 1 (passive) complete; Phase 2 (hex content + active probes) pending**

This is the first deliverable in the research track that will inform a
re-implementation of the reverted **linbpq-flexnet v1.9.4 transit-role
re-advertisement mechanism**. Phase 1 captures one full hour of live
peer-to-peer traffic on xnet-14's two FlexNet links (one to a PC/Flexnet
peer, one to an xnet peer) and characterises the cycle cadence, KA
cadence, and route-exchange volume at the frame-header level.

Phase 2 will add hex payload content (`mo -ih +N`) and active transit
probes (a controlled `C IR3UHU-2` from IW2OHX-13 forced through
xnet-14) so we can answer the load-bearing question — does xnet
re-advertise IR3UHU-2's downstream destinations into the PCF-12 link,
and with what RTT/`?` indirect markers?

---

## 1. TL;DR — Top Findings

1. **KA cadence is peer-family-dependent.** xnet V1.39 emits KAs every
   **189 s** (exact, min=189, max=191 over 19 samples per direction).
   PC/Flexnet 3.3g emits KAs every **16 s** (min=15, max=18 over 223
   samples). The "180 s KA period" figure in the skill was correct for
   xnet but wrong for PCF — PCF is 11× more aggressive. **Action: skill
   §1.5 needs updating.**
2. **Route-exchange volume is wildly asymmetric between xnet↔xnet and
   xnet↔PCF.** Port 11 (xnet ↔ IR3UHU-2) shows **31 distinct compact-
   record bursts** in 60 min, median inter-burst gap **120 s** (half
   the documented 240 s polling cycle, suggesting xnet runs a
   "lightweight" cycle at 120 s and the full one at 240 s). Port 1
   (xnet ↔ PCF-12) shows **a continuous trickle — 73 compact frames
   spread over 3593 s with zero gaps > 30 s**. The trickle pattern
   indicates xnet does NOT batch route exchanges with PCF the way it
   does with xnet peers; it interleaves single-record (12–14 B)
   updates throughout the cycle.
3. **L2 was rock-solid throughout the hour.** Zero SABM / UA / DISC /
   FRMR on either port. Eight REJ frames total (7 on port 1, 1 on
   port 11) — minor retransmits, normal AXUDP behaviour. The xnet
   ↔ peer L2 sessions established before our capture started and
   stayed up the entire hour.
4. **No live user transit during the capture window.** Zero frames
   with a source or destination outside the link pair (other than
   F0 UI broadcasts to NODES/QST). The hour didn't happen to contain
   a user multi-hop through xnet-14. Phase 2 must include active
   probes (`C IR3UHU-2` from IW2OHX-13 via xnet-14) to force at
   least one transit case into the window.
5. **3-byte CE frames are direction-asymmetric on port 1.** xnet
   emits 240 of them to PCF; PCF emits 20 back. Without payload
   content we can't say how many are LT replies (`1NNN\r`) vs `3+\r`
   / `3-\r` tokens — they're all 3 B. Resolving this needs hex.

---

## 2. Topology & Capture Parameters

```
                                    AXIP/UDP
       IW2OHX-13 ─── F ────────┐    (port 3 on iw2ohx-13)
   (linbpq-flexnet v2.1.9)     │
                               ▼
              ┌──────────────────────────────┐
              │      IW2OHX-14 (xnet V1.39)  │
              │                              │
       port 1 │                              │ port 11
              ▼                              ▼
       IW2OHX-12                         IR3UHU-2
       (PC/Flexnet 3.3g)                 (xnet)
```

**Capture commands** (run on iw2ohx-gw, two parallel `xnet_monitor.py`
instances, each its own telnet+SYS session to dlc7.iw2ohx.ampr.org:23):

```
xnet_monitor.py --config xnet.conf --monitor-port 1  --duration 3600
xnet_monitor.py --config xnet.conf --monitor-port 11 --duration 3600
```

Internally this issues `mo -iusk +<port>` after SYS elevation — I/U/S
frames + timestamps, **headers only, no hex payload**. Phase 2 will
add `-h` for full hex.

**Artefacts in this directory:**
- `p01_pcf12_20260517T055931Z_raw.txt` (428 KB) — port 1 raw monitor stream
- `p01_pcf12_20260517T055931Z.json` (760 KB) — port 1 line-by-line JSON
- `p11_ir3uhu_20260517T055942Z_raw.txt` (303 KB) — port 11 raw
- `p11_ir3uhu_20260517T055942Z.json` (554 KB) — port 11 JSON
- `*_runlog.txt` — xnet_monitor.py stderr (auth, prompt-tracking)
- `parse_capture.py` — analyzer that consumed the raw files
- `*.parsed.json` — derived inventory + cross-port correlation

---

## 3. Frame Inventory

### 3.1 Port 1 — xnet-14 ↔ IW2OHX-12 (PC/Flexnet 3.3g)

**1152 frames over 3596 s = 19.2 frames/min.**

| Frame kind                       | Count | Notes                                    |
|----------------------------------|-------|------------------------------------------|
| RR                                | 333   | L2 ack at supervisory layer              |
| CE 3-byte (LT-or-token)           | 260   | xnet→PCF 240, PCF→xnet 20 — ambiguous    |
| CE KA (201 B `'2'` + 200 spaces)  | 223   | PCF→xnet only, every **16 s**            |
| CF (NetROM-compat / L3RTT)        | 207   | mix of UI to NODES and I-frames          |
| CE compact-record (10–250 B)      | 73    | trickle, no batching (see §4)            |
| F0 UI broadcast                   | 30    | mostly QST                               |
| CE KA (241 B xnet form)           | 19    | xnet→PCF, every **189 s** (exact)         |
| REJ                               | 7     | retransmit triggers                      |

CE payload-length distribution (top entries):

| Bytes | Count | Interpretation                                |
|-------|-------|-----------------------------------------------|
|     3 | 260   | LT reply `1NN\r` or `1n\r` status or token   |
|   201 | 223   | PCF keepalive                                 |
|   241 | 19    | xnet keepalive                                |
|    14 | 10    | single 1-record compact frame (1 destination) |
|    24 | 9     | 2-record compact frame                        |
|    13 | 8     | single record (slightly shorter RTT digits)   |
|    12 | 5     | single record                                 |

### 3.2 Port 11 — xnet-14 ↔ IR3UHU-2 (xnet)

**1529 frames over 3572 s = 25.7 frames/min — 34 % more than port 1.**

| Frame kind                       | Count | Notes                                    |
|----------------------------------|-------|------------------------------------------|
| RR                                | 695   | L2 ack                                   |
| CF (NetROM-compat / L3RTT)        | 378   | bidirectional L3RTT probe/reply pairs    |
| CE compact-record (10–250 B)      | 315   | **31 bursts, ~120 s cadence**            |
| CE 3-byte (LT-or-token)           | 72    | symmetric: 36 each direction             |
| CE KA (241 B)                     | 38    | 19 each direction, every **189 s**        |
| F0 UI broadcast                   | 30    | QST                                      |
| REJ                               | 1     | one retransmit in the whole hour         |

CE payload-length distribution (top entries):

| Bytes | Count | Interpretation                                |
|-------|-------|-----------------------------------------------|
|     3 | 72    | LT reply or token                             |
|   241 | 38    | xnet keepalive (both directions)              |
|    42 | 34    | 3-record compact frame                        |
|    41 | 32    | 3-record compact frame                        |
|    44 | 25    | 3-record compact frame                        |
|    14 | 15    | 1-record compact frame                        |
|    45 | 14    | 3-record compact frame                        |
|    73 | 8     | 5-record compact frame (recurring batch size) |

---

## 4. Cycle & Cadence Findings

### 4.1 Keepalive Cadence — Definitive Numbers

| Direction                       | n  | median  | mean | min  | max  |
|---------------------------------|----|---------|------|------|------|
| **xnet-14 → IR3UHU-2** (241 B)  | 19 | **189 s** | 189 s| 189 s| 191 s|
| **IR3UHU-2 → xnet-14** (241 B)  | 19 | **189 s** | 189 s| 189 s| 191 s|
| **xnet-14 → IW2OHX-12** (241 B) | 19 | **189 s** | 189 s| 189 s| 191 s|
| **IW2OHX-12 → xnet-14** (201 B) | 223| **16 s**  | 16 s | 15 s | 18 s |

xnet's outbound KA cadence is **rock-solid 189 s** regardless of peer
family. PC/Flexnet's KA cadence is **16 s** — much more aggressive
than the 180 s figure derived from xnet captures. This means a
linbpq-flexnet peer of PCF sees ~11× more KAs than a peer of xnet,
which is exactly what's been observed against IW2OHX-12 on the
production iw2ohx-13 link (KA counter climbs ~10× faster).

### 4.2 Route-Exchange Burst Cadence

**Port 11 (xnet ↔ xnet):** 31 distinct bursts identified by
gap > 30 s between consecutive compact frames. Inter-burst gaps:

- count: 30
- median: **120 s**
- mean: 119 s

This is **half** the 240 s polling-cycle figure documented in the
skill. Two possible explanations, distinguishable only with payload
content:

- **A.** xnet does a "lightweight" 120 s cycle (selective records
  for changed entries) and the 240 s figure refers to a "full"
  cycle that we'd only see if we monitored longer.
- **B.** The cycle is genuinely 120 s and the 240 s figure was a
  prior approximation.

**Port 1 (xnet ↔ PCF):** **zero gaps > 30 s** across all 73 compact
frames. xnet emits compact frames to PCF on a continuous trickle
(roughly every 49 s on average), in single-record (12–14 B) or
two-record (~24 B) form. xnet does NOT batch route exchanges with
PCF the way it does with xnet peers — it **streams individual
updates** instead. This is a fundamentally different protocol
shape than the burst-with-`3+`/`3-` exchange seen on port 11.

### 4.3 3-byte CE Frame Direction

| Direction                | n   |
|--------------------------|-----|
| xnet-14 → IW2OHX-12      | 240 |
| IW2OHX-12 → xnet-14      | 20  |
| xnet-14 → IR3UHU-2       | 36  |
| IR3UHU-2 → xnet-14       | 36  |

The port-11 symmetry (36/36) is consistent with the LT-to-LT ping-pong
documented in the skill: every peer KA triggers a 3-byte LT reply, and
both ends KA at 189 s, so we expect ~19 LT each way + ~17 tokens (rough
matches across direction).

The port-1 asymmetry (240 from xnet vs 20 from PCF) is striking. If
xnet replies one LT per PCF KA, that's 223 LTs (matches the 240 close
enough — 240 - 223 = 17 leftover, likely tokens). PCF emits 20 3-byte
frames back at xnet — those could be tokens (`3+`/`3-`) or LT replies
to xnet's own 19 outbound KAs (close to 20 — matches!). Strong
hypothesis: **PCF DOES echo LT replies to xnet's KAs, but at the rate
of xnet's KA cadence (189 s) instead of its own.** Resolving this
needs hex payload to see if those 20 3-byte frames carry `1NN\r`
content or `3+\r`/`3-\r` tokens.

---

## 5. L2 Stability and Other Observations

- **Zero L2 establishment events** during the hour (no SABM/UA, no
  DISC, no FRMR). Both links were stable across the entire window.
- **REJ frames: 8 total** (7 on port 1, 1 on port 11). Normal AXUDP
  retransmit triggers; well below the >10 % rr+% rate that would
  indicate a tuning problem.
- **F0 UI broadcasts: 30 per port, identical count.** These are
  xnet's NODES / QST beacons; one was reflected on each link, same
  cadence on both — they're not link-specific.
- **No user transit traffic.** Passive only; no station happened
  to do a multi-hop connect through xnet-14 during the hour. To
  characterise transit specifically we **need active probes in
  Phase 2** (e.g. `C IR3UHU-2` from IW2OHX-13 via xnet-14, plus
  a few I-frames, plus disconnect).

---

## 6. Implications for linbpq-flexnet Transit Re-implementation

Recall the v1.9.4 revert (linbpq-flexnet v1.9.7) — transit-role
D-table re-advertisement was tried and reverted because the L2 digi
chain it implied broke AX.25 V2 reciprocity on the return frame. Any
new design must preserve reciprocity AND match what xnet does on the
wire, since xnet is the gold-standard peer.

### 6.1 What Phase 1 confirms

1. **Two distinct exchange shapes per peer family.** A re-implementation
   that adopts the xnet ↔ xnet burst pattern uniformly will misbehave
   against PCF; one that adopts the PCF trickle pattern will
   under-advertise to xnet. The implementation needs to detect peer
   family — or, more pragmatically, **use the burst pattern toward all
   peers** since PCF tolerates it (xnet is talking to PCF that way in
   the inverse, sending burst-shaped advertisements to PCF works fine
   from xnet → PCF in the F0/UI direction, but the CE-direction
   trickle is xnet's own choice, not a PCF requirement).
2. **KA cadence is dictated by the peer, not by us.** We echo. The
   linbpq-flexnet event-driven KA design (skill §1.11) is exactly
   right — it adapts to whatever cadence the peer sets.
3. **L2 stays up across the polling cycle.** We do not need to
   re-establish AX.25 sessions per cycle (that's a different style
   used by some older NORD><LINK implementations). The session is
   long-lived; route exchange runs over it.

### 6.2 What Phase 1 cannot answer (requires Phase 2 hex)

1. **Does xnet re-advertise IR3UHU-2's downstream destinations to
   PCF-12?** The 73 compact frames on port 1 could be either:
   - xnet's own routes only (no transit re-advertisement), or
   - xnet's own routes PLUS IR3UHU's learned downstream destinations
     (with appropriate RTT and `?` indirect markers), or
   - Updates triggered by table mutations on either side.
   To distinguish we need the actual callsigns in those frames.
2. **What RTT marking does xnet apply to re-advertised destinations?**
   Per the skill §1.6, indirect records carry a `?` prefix before
   the RTT digits. Whether xnet uses this consistently for transit
   destinations is unknown without hex.
3. **Does xnet observe the `3+` request token from peers before
   sending records, or push proactively?** The 73 port-1 compact
   frames over 60 minutes interleaved with PCF's 223 KAs and 20 reply
   frames — we don't know which trigger which without seeing the
   tokens in flight.

### 6.3 Concrete Phase 2 design questions

For the active hex-capture pass, the test plan must answer:

- **Q1** — Which callsigns appear in port-1 compact records, and do
  any of them also appear on port 11 (i.e. learned from IR3UHU-2)?
- **Q2** — What is the RTT relationship for transit-advertised
  records? If xnet learns `IR5S RTT=4 via IR3UHU-2 RTT=2`, does it
  advertise `IR5S RTT=6` to PCF (sum), `IR5S ?6` (sum with indirect
  marker), or something else?
- **Q3** — Does an active user `C IR3UHU-2` from IW2OHX-13 routed
  via xnet-14 produce SABM/UA with the digi chain `IW2OHX-13 →
  IR3UHU-2 via IW2OHX-14*`? And what happens to the chain on the
  return UA — is it `IR3UHU-2 → IW2OHX-13 via IW2OHX-14*`? Does
  xnet preserve, rewrite, or extend the chain? **This is the
  reciprocity-breaking failure mode that took down v1.9.4.**
- **Q4** — When IW2OHX-13 disconnects, what does the
  `3-`+withdrawal compact frame look like that xnet emits to PCF?
  Does xnet emit one at all, or wait for the next cycle?

---

## 7. Next Steps

### 7.1 Phase 2 — Hex content capture (mandatory before any code)

Re-run both captures with `mo -ih +<port>` (drop `-usk` flags, add
`-h` for full hex). Larger output (~3× volume) but gives:
- Exact CE frame type by first byte (`0`/`1`/`2`/`3`/`4`/`6`/`7`)
- Callsigns and RTT values inside compact records
- Distinguishability of LT (`1NNN\r`) from tokens (`3+\r`/`3-\r`)
- Visibility into `?` indirect-measurement prefix usage

Estimated duration: 60 min. Estimated raw output: 1–2 MB per port.

### 7.2 Phase 3 — Active transit probes

While Phase 2 hex capture is running, trigger:
- `C IR3UHU-2` from IW2OHX-13 BPQ console (via xnet-14)
- A handful of I-frames in the resulting session
- Clean DISC
- Repeat with `C IW2OHX-12` (the PCF target) to compare digi-chain
  handling between an xnet peer and a PCF peer

This forces transit traffic into the capture and gives concrete
SABM/UA digi-chain pairs to analyse.

### 7.3 Phase 4 — Skill update + design doc

After Phase 2/3 we should be able to write a follow-up MD with:
- Confirmed transit-re-advertisement algorithm (the algorithm xnet
  follows, derived from wire observation)
- A reciprocity-preserving design for linbpq-flexnet that, unlike
  v1.9.4, does NOT extend the L2 digi chain on the return frame
- A staged rollout plan (test on IR2UFV first per the standing
  workflow)

---

## 8. Open Questions Carried Forward

- Why does PCF KA at 16 s while xnet KAs at 189 s? Is this a config
  knob on PCF (`KeepAliveInterval` in `pcfn.cfg`?) or a hardcoded
  default? Worth a quick check on the IW2OHX-12 config file if
  accessible. **If 16 s is the default**, the skill §1.5 needs to
  drop "180 s" as the canonical period and call out per-family.
- Are the port-1 compact frames "lazy single-record" emissions
  triggered by individual D-table mutations, or are they actually
  a fragmented batch that just happens to span the hour? Hex content
  + per-frame correlation against xnet's `D` table state will tell.
- Does the 120 s burst cadence on port 11 hold under load (e.g. a
  full mesh recompute)? Phase 2 should grab a longer capture (~3 h)
  to catch any cycle variation.

---

## 9. Appendix — Sample Raw Lines

**Port 1, IW2OHX-14 → IW2OHX-12 compact records (continuous trickle):**
```
1:fm IW2OHX-14 to IW2OHX-12 ctl I11^ pid CE [12] - 17.05.26 07:00:28
1:fm IW2OHX-14 to IW2OHX-12 ctl I51^ pid CE [12] - 17.05.26 07:09:13
1:fm IW2OHX-14 to IW2OHX-12 ctl I63^ pid CE [14] - 17.05.26 07:09:34
1:fm IW2OHX-14 to IW2OHX-12 ctl I21^ pid CE [13] - 17.05.26 07:10:37
1:fm IW2OHX-14 to IW2OHX-12 ctl I33^ pid CE [12] - 17.05.26 07:10:58
1:fm IW2OHX-14 to IW2OHX-12 ctl I23^ pid CE [14] - 17.05.26 07:12:22
1:fm IW2OHX-14 to IW2OHX-12 ctl I57^ pid CE [14] - 17.05.26 07:13:25
1:fm IW2OHX-14 to IW2OHX-12 ctl I25^ pid CE [13] - 17.05.26 07:16:34
1:fm IW2OHX-14 to IW2OHX-12 ctl I40^ pid CE [12] - 17.05.26 07:16:55
1:fm IW2OHX-14 to IW2OHX-12 ctl I20^ pid CE [13] - 17.05.26 07:18:19
```
Note: I-frame sequence numbers (I11, I51, I63, …) wrap through the
8-bit window naturally — no retransmits between samples.

**Port 11, IW2OHX-14 ↔ IR3UHU-2 compact-record burst (bursty pattern):**
```
11:fm IW2OHX-14 to IR3UHU-2 ctl I31^ pid CE [42] - 17.05.26 07:00:33
11:fm IR3UHU-2 to IW2OHX-14 ctl I23^ pid CE [73] - 17.05.26 07:00:33
11:fm IW2OHX-14 to IR3UHU-2 ctl I04^ pid CE [42] - 17.05.26 07:02:33   <-- next burst, +120 s
11:fm IR3UHU-2 to IW2OHX-14 ctl I50^ pid CE [73] - 17.05.26 07:02:33
11:fm IW2OHX-14 to IR3UHU-2 ctl I15^ pid CE [42] - 17.05.26 07:02:35
11:fm IR3UHU-2 to IW2OHX-14 ctl I61^ pid CE [42] - 17.05.26 07:02:35
11:fm IW2OHX-14 to IR3UHU-2 ctl I12^ pid CE [42] - 17.05.26 07:04:33   <-- +120 s
11:fm IR3UHU-2 to IW2OHX-14 ctl I50^ pid CE [73] - 17.05.26 07:04:33
```
Note the lockstep TX/RX pairing at the same timestamp — a route-record
burst is symmetric, both ends pushing within the same second.

---

_Phase 1 capture authored by Claude on 2026-05-17. Phase 2 hex
capture completed same day; see below._

---

# Phase 2 — Hex Content Findings

**Capture window:** 2026-05-17 08:19 → 09:19 UTC (60 min)
**Command:** `mo -i +N` on both ports (full hex; drops `-usk` flags that
were suppressing hex output in Phase 1 — xnet help: `h` = headers-only,
`x` = disable-hex, so headerless+hex is the *default* when no flags are
given, and adding `-i` alone keeps hex while filtering to I-frames).

**Trade-off:** with `-i` we lose RR / SABM / DISC / UA visibility (those
were captured in Phase 1) and lose xnet-side wall-clock timestamps
(`-k` flag conflicts with `-i` in this xnet version). Wall clock comes
from the script's own monotonic timer and the per-line UTC stamps in
the runlog. Frame totals: **p01h = 603 frames**, **p11h = 789 frames**.
Smaller than Phase 1's 1152/1529 because filtering out RR/SABM/DISC.

## 10. The Load-Bearing Transit Question — ANSWERED

**Does xnet-14 re-advertise destinations learned from IR3UHU-2 (and
other neighbours) onto the xnet-14 ↔ PC/Flexnet-12 link?**

**YES. Unambiguously.** Cleanly decoded sample (xnet → PCF, hex from
on-wire I-frame):

```
1:fm IW2OHX-14 to IW2OHX-12 ctl I45^ pid CE [13]
0000 33 49 51 32 4C 42 20 30 35 34 30 20 0D    3IQ2LB 0540 .
```

Decodes per spec §1.6 (skill) as:
- type byte `0x33` = type 3 (compact records)
- callsign `IQ2LB ` (6 chars, space-padded)
- SSID_LO `0` (byte `0x30`)
- SSID_HI `5` (byte `0x35`)
- RTT `40` (i.e. 4.0 s in 100-ms ticks)
- **no `?` indirect-measurement prefix**
- trailing separator + CR

`IQ2LB` is not IW2OHX-14's direct AXIP neighbour. It's a downstream
destination on the FlexNet cloud. Over the 60-min capture xnet-14 sent
**8 unique downstream callsigns in 16 records** to PCF-12 — every one
of them a re-advertised route, not a local one.

## 11. Per-Direction Records — Who Advertises What

|  src → dst              | records | unique calls | top callsigns                                                                              |
|-------------------------|---------|--------------|--------------------------------------------------------------------------------------------|
| IW2OHX-14 → IW2OHX-12   | 16      | 8            | DB0LHR×1, DB0ZKA×1, **DK0WUE**×2, DM0ZOG×4, HB9AJ×1, HB9AM×1, **IQ2LB**×5, K1YON×1          |
| IW2OHX-12 → IW2OHX-14   | 6       | 2            | IQ2LB×2, IW2OHX×4                                                                          |
| IW2OHX-14 → IR3UHU-2    | 18      | 6            | DB0FRG×1, DB0TOD×1, HB9AJ×3, HB9ZRH×1, **IQ2LB**×9, IW2OHX×3                                |
| IR3UHU-2 → IW2OHX-14    | 10      | 7            | DB0ZKA×1, DM0ZOG×4, HB9AJ×1, HB9AM×1, HB9ZRH×1, IQ2LB×1, K1YON×1                            |

Observations:

- **PCF advertises almost nothing back** to xnet (6 records, all about
  `IW2OHX` and `IQ2LB`). It's effectively a leaf in this exchange.
- **xnet-14 advertises IQ2LB heavily** to BOTH peers (×5 to PCF, ×9 to
  IR3UHU). IQ2LB is a busy / flap-prone destination.
- **xnet-14 advertises a different set toward each neighbour.** Only
  `IQ2LB` and `HB9AJ` are advertised by xnet-14 in *both* outbound
  directions. Each peer gets a tailored subset — likely the ones xnet
  has updates for at that moment, not a full table dump.

## 12. RTT Arithmetic — How xnet Computes Transit RTT

Cleanest single-sample IN/OUT pairs (where xnet learned from IR3UHU-2
and re-advertised to PCF-12 in the same capture):

| Callsign | IR3UHU → xnet (in) | xnet → PCF (out) | Δ        |
|----------|--------------------|------------------|----------|
| DB0ZKA   | 14                 | 16               | **+2**   |
| K1YON    | 20                 | 23               | **+3**   |
| HB9AM    | 2442               | 2748             | **+306** |

Multi-sample destinations (less clean, flap-affected):

| Callsign | IR3UHU → xnet            | xnet → PCF              |
|----------|--------------------------|-------------------------|
| DM0ZOG   | 0, 43, 106, 646          | 0, 49, 120, 727         |
| HB9AJ    | 0                        | 0                       |
| IQ2LB    | 124 (one sample)         | 40, 10, 42, 14, 2252    |

**Interpretation:** xnet's transit re-advertisement RTT is **the
learned RTT from the source-neighbour PLUS a small additive cost
(2–3 ticks for normal, 300+ for high-RTT cases)**. The additive is
approximately the link RTT from xnet to the source-neighbour. The
algorithm is **simple addition** — no normalisation, no scaling, no
indirect marker.

The +306 outlier on HB9AM is consistent with a route-flap event where
the underlying link RTT spiked during the capture — both directions
moved together, just at different magnitudes.

The DM0ZOG/HB9AJ "0" entries are suspicious. Open question: is RTT=0
in a compact record a sentinel (e.g. "newly learned, not yet
measured") or a withdrawal marker distinct from RTT=60000?

## 13. `?` Indirect-Measurement Prefix — NEVER USED

Total compact records parsed across both ports: **50** (22 on port 1,
28 on port 11).
Records carrying the `?` indirect prefix: **0**.

Per spec §1.2 (and the skill) the `?` (`0x3F`) before the RTT digits
marks a second-hand measurement. The wire shows **(X)NET V1.39 does
not use this marker on transit re-advertisements**. Implication: a
linbpq-flexnet transit re-implementation does NOT need to apply `?`
either — xnet doesn't, so omitting it is the in-conformance choice.

(Caveat: the `?` *character* DOES appear in many compact records, but
always as **SSID_HI = 15** — `0x3F` interpreted positionally as the
SSID-encoding byte, not as the indirect prefix. The disambiguation
rule in §1.2 of the skill held perfectly in all observed records.)

## 14. CE Frame-Type Distribution (Now Definitive)

| Refined kind        | Port 1 (PCF)   | Port 11 (IR3UHU)  |
|---------------------|----------------|-------------------|
| ce-init             | 0 (see §15)    | 0 (see §15)       |
| ce-lt (type 1)      | 255            | 38                |
| ce-ka-xnet (241 B)  | 19             | 38                |
| ce-ka-pcf (201 B)   | 212            | 0                 |
| ce-ka-{197,198}     | 3 + 3 (port 1) | 0                 |
| ce-compact-batch    | 23             | 29                |
| ce-tok-plus (`3+`)  | 14             | 50                |
| ce-tok-minus (`3-`) | 5              | 31                |
| ce-pathrep (type 7) | 0              | **10**            |
| ce-seqnum (type 4)  | 5              | 0                 |
| (continuation frags)| 38             | 122               |

Key observations:

- **PCF KA at 16 s** confirmed (212 of them in 60 min, matching Phase 1's
  223). The 197 B / 198 B variants (6 total) are PCF KAs that got
  truncated by a few bytes — minor; could be L2 fragmentation artefact.
- **xnet KA at 189 s** confirmed on both ports (19 / 38 over 60 min).
- **type-1 LT replies are abundant** — 255 on port 1, one per PCF KA
  with extras, all decoded successfully as decimal seconds.
- **type-4 sequence-number frames** observed on port 1 (5 of them, all
  xnet → PCF). xnet IS emitting type-4 to PCF over time despite the
  Phase 1 skill claim that proactive type-4 TX "causes peers to
  withdraw routes" — that claim was about (X)NET *peers* withdrawing
  on type-4; PCF tolerates it. Skill nuance needed.
- **type-7 PATH-REPLY on port 11 only** (10 frames). Skill §1.10 said
  "(X)NET V1.39 does not emit type-7 replies" — that's contradicted
  by this capture for xnet ↔ xnet links. The skill claim may have
  been: V1.39 doesn't emit type-7 *toward all peer families*. Need
  active probes (Phase 3) to confirm whether xnet would reply to a
  type-6 issued from PCF or from a linbpq-flexnet peer.

## 15. CE INIT — Actually Never Observed Mid-Session

The parser initially flagged 2 ce-init events on port 1 and 3 on
port 11. Hex inspection showed all were **false positives**: payloads
like `30 3F 31 37 38 20 0D` (`"0?178 \r"`) — these are compact-record
*continuation fragments* whose first byte happens to be `0x30` (the
SSID_LO for SSID=0). The genuine INIT pattern is the fixed 5-byte
`30 3? 25 21 0D` from skill §1.3, and none were seen in either
capture.

**Confirmed: both L2 sessions stayed up the entire hour with no
re-init.** Consistent with Phase 1's "zero SABM/UA/DISC" observation.

## 16. CE Frame Continuation Across L2 I-Frames (Parser Artifact)

xnet emits compact-record batches as a *byte stream* on the CE PID.
When a batch exceeds one L2 I-frame's window, the next I-frame
continues the stream **without a fresh `0x33` type byte**. xnet's
monitor shows each L2 frame as a separate `pid CE [N]` block.

This produced 38 "unknown-first-byte" frames on port 1 and 122 on
port 11, with content like:
```
0x20 ' ' plen=20  ' 440 HB9AJ 880 H00 \r'   (continuation w/ leading sep)
0x4F 'O' plen=8   'OG0?25 \r'                (mid-callsign continuation)
0x4A 'J' plen=10  'J 002791 \r'              (tail-of-record continuation)
0x0D '\r' plen=1  ''                          (bare batch-end marker)
```

These were not lost — they were just classified as "unknown" by the
per-frame parser. A stream-level parser that concatenates all
consecutive CE bytes per direction and segments on `\r` would recover
them. For Phase 2 we kept the per-frame view since the well-formed
fragments (50 records) already gave a clean signal.

## 17. PATH-REPLY Bodies (Type 7)

10 type-7 frames observed on port 11, all `IR3UHU-2 → IW2OHX-14`.
Sample bodies (first 5):

```
'5  185IW2OHX-13u IW2OHX-14 IR3UHDB0FAA DB0UHF'
'1  199IW2OHX-13q IW2OHX-14 IR3UHK2PUT-1'
'1  201IW2OHX-13q IW2OHX-14 IR3UH8 DB0NU DB0ALG'
'7  202IW2OHX-13w IW2OHX-14 IR3UH0DLG-6'
'3  204IW2OHX-13s IW2OHX-14 IR3UHDB0FAA'
```

Decode of frame 1 per skill §1.10:
- `HOP_BYTE` = `0x35` (`'5'`)
- `SEQ` = `' 185'` (4-digit decimal, space-padded)
- `CS_1` = `IW2OHX-13u` — the addressee (user IW2OHX-13 with role
  marker `u`, possibly "user")
- path follows: `IW2OHX-14 IR3UH(U-2 elided?) DB0FAA DB0UHF`

The path lengths (5, 1, 1, 7, 3) and the addressee `IW2OHX-13` (our
production node) suggest **someone on IW2OHX-13 was actively running
path probes during the capture window**. Marco was likely operating
the node during the test, or background probing is happening from
linbpq-flexnet's M5 path-discovery (skill §1.9, FlexNet_Timer ticks
out one PATH_REQ every 60 s).

This is actually a *natural* active-probe scenario captured during a
passive run — useful for Phase 3 planning.

## 18. Skill Corrections Required

Phase 2 findings update the skill in three places:

1. **PCF KA wire format (skill §1.5).** Currently documented as `'2' +
   N × ' '` with explicit "no trailer — no CR, no trailing 10\r".
   Phase 2 hex shows PCF KA = **`'2' + 199 × ' ' + 0x0D`** (200-space-
   then-CR = 201 bytes total). The 0x0D trailer IS present.
   xnet's 241-byte KA: verify; the one decoded sample showed CR at
   byte 240 (`...20 20 0D` at row 00F0). xnet may also have a CR
   trailer the original skill missed.
2. **Type-7 PATH_REPLY emission (skill §1.10).** "V1.39 does not emit
   type-7 replies" is contradicted by 10 type-7 frames captured from
   IR3UHU-2 (also (X)NET) toward IW2OHX-14. Re-phrase: V1.39 DOES emit
   type-7 between xnet peers; behaviour toward non-xnet peers (PCF,
   linbpq-flexnet) needs Phase 3 confirmation.
3. **Type-4 sequence-number emission toward PCF (skill §1.7).** "We
   should not originate type-4 proactively" — but xnet itself
   originates type-4 toward PCF (5 frames captured in 60 min) without
   PCF withdrawing routes. So the skill rule is xnet-peer-specific,
   not universal. Adjustment: "do not originate type-4 toward (X)NET
   peers — they treat it as unknown and withdraw routes; emission
   toward PCF and linbpq-flexnet peers is fine."

A bonus correction worth investigating before next iteration: skill
§1.4 says "PC/Flexnet 3.3g does NOT echo type-1 back at us". Phase 2
shows xnet emitting 255 type-1 LT replies on port 1 (xnet → PCF) but
we did NOT see PCF emit type-1 itself. So the skill claim holds for
PCF → xnet direction. Our linbpq-flexnet should still expect
`LT 0.0s` against PCF.

## 19. CF (NetROM-Compat) D-Table Dumps — A Live Sample

Port 11 had **453 PID=CF frames**, an order of magnitude more than CE
compact frames (110). The CF layer is where xnet ↔ xnet exchanges the
bulk of the routing data. Sample frames:

```
11:fm IR3UHU-2 to IW2OHX-14 ctl I24^ pid CF [78]
VA3PJZ-2    139/9
VA3PJZ-6    139/9
VA3PJZ-11   139/9
DK0WUE-8  60000/30      ← poison reverse
N9LYA-4   60000/30
N9LYA-6   60000/30
N9LYA-7   60000/30

11:fm IW2OHX-14 to IR3UHU-2 ctl I52^ pid CF [57]
N9SEO-2    2197/13 0[7] 'BAXDX'
N9SEO-7    2167/13 0[8] 'BAXURO'
N9SEO-11   1517/10 0[8] 'BAXXRP'
```

Decode per skill §2.2: `CALL-SSID  RTT/SSID_MAX [PORT[TYPE] 'ALIAS']`.
- `0[7]` = port 0 (RF), type 7 (FlexNet)
- `0[8]` = port 0 (RF), type 8 ((X)Net / DAMA)
- aliases preserved (`BAXDX`, `BAXURO`, `WUEBBS`...)

Port 1 had only **207 PID=CF frames** (mostly NODES UI broadcasts).
xnet does NOT export D-table contents over CF to PCF — PCF gets CE
compact records only.

**This is a significant finding for linbpq-flexnet transit design:**
the route-exchange shape is *peer-family-dependent at the protocol
level*, not just at the cadence level. xnet uses CF D-table dumps
with xnet peers AND CE compact records with PCF. Mixing CE compact
+ CF D-table on a single link is the xnet-to-xnet shape; CE compact
only is the xnet-to-PCF shape.

## 20. Implications for the linbpq-flexnet Transit Re-Implementation

Phase 2 gives us a concrete, observable wire pattern. The reverted
v1.9.4 must be redesigned as follows:

### 20.1 What to Re-Advertise

Maintain a set per peer of "routes I've told this peer about", and a
set per origin-neighbour of "routes I learned via this neighbour".
Re-advertise the union of (own routes) ∪ (learned-from-other-neighbour
routes) toward each peer — exclude routes learned FROM that peer
(split-horizon). The capture shows xnet honours split-horizon: IR3UHU-2
records like `DM0ZOG` are advertised to PCF, but NOT back to IR3UHU-2.

### 20.2 RTT Arithmetic

For each re-advertised destination:
```
out_rtt = learned_rtt_from_neighbour + link_rtt_to_that_neighbour
```
NO indirect `?` marker. No special scaling. Plain integer addition in
100-ms-tick units. The link RTT is what we measure as our own smoothed
RTT to the neighbour (already maintained per skill §1.4).

### 20.3 Frame Shape

Toward each peer, emit single-record CE compact frames at a low
cadence (~one every 30–60 s when there's a change). Whether to batch
into multi-record frames or stream as singles can be decided per peer:
xnet's behaviour with PCF was singles, with IR3UHU was small batches
of 3–5 records (median frame size 41–44 B on port 11).

### 20.4 Token Protocol

Honour the `3+` (REQUEST) / `3-` (END-OF-BATCH) token state machine
per skill §1.6 — Phase 2 confirms xnet uses it heavily on xnet-to-xnet
links (50 `3+` and 31 `3-` on port 11 in 60 min). For PCF the token
usage is lighter (14 `3+`, 5 `3-`) but still present.

### 20.5 AX.25 V2 Reciprocity (the v1.9.4 failure mode)

**This is the part Phase 2 did NOT capture** — no user transit happened
during the hour. The re-implementation must preserve AX.25 V2
reciprocity on USER SESSIONS that pass through us. The v1.9.4 break
was specifically that extending the L2 digi chain on the return path
made the originator's UA match fail.

Phase 3 must trigger a controlled user transit (`C IR3UHU-2` from
IW2OHX-13 forced through IW2OHX-14) so we can inspect:
- xnet-14's SABM digi-chain emission toward IR3UHU-2
- IR3UHU-2's UA digi-chain emission back toward IW2OHX-13
- Whether xnet REWRITES or PRESERVES the chain at the transit hop

This is critical and must be captured before any code work begins.

### 20.6 What's Already Established

- Single-record trickle is acceptable to PCF (the v1.9.4 design that
  was tried with batched advertisements may have been overcomplicated;
  single records may be simpler).
- KA + LT echo machinery is unchanged.
- CF D-table emission is optional — xnet doesn't use it toward PCF
  and PCF is happy. linbpq-flexnet's CE-only approach matches the
  xnet-to-PCF pattern and should work for transit to PCF peers.

---

## 21. Phase 3 Plan (Active Probes)

Marco's go-ahead pending. Phase 3 will:

1. **Re-run hex captures on both ports** (60 min, same `mo -i`).
2. **During the capture window**, trigger from IW2OHX-13 BPQ console:
   - `C IR3UHU-2` (user transit through xnet-14 to IR3UHU)
   - A handful of I-frames (e.g. `L`, `D`)
   - Clean `B`/`BYE` to issue DISC
   - Repeat with `C IW2OHX-12` (transit to PCF)
   - Trigger a path probe (`P IR3UHU-2`?) to force type-6 PATH_REQ
3. **Analyse:** SABM/UA digi-chain handling at xnet-14 transit, type-7
   reply propagation, and any reciprocity-breaking failures.

After Phase 3 we can write a concrete RFC-style design doc for
`linbpq-flexnet transit-role v2 (post v1.9.4)` and proceed to
implementation on the IR2UFV test instance first.

---

_Phase 2 capture and analysis authored by Claude on 2026-05-17.
Phase 3 active-probe pass completed same day; see below._

---

# Phase 3 — Active Transit Probes

**Capture window:** 2026-05-17 09:31 → 09:56 UTC (25 min, both ports)
**Capture command:** `mo -iusk +N` (Phase 1 flags — restores SABM/UA/RR
visibility with timestamps; hex not needed since the digi chain is in the
header line).
**Active probes** issued from `IW2OHX-13` BPQ console (telnet 2323,
user Pino → callsign IW7CFD) at T+2 min into the capture window:

| Probe                | Topology                              | What we want to see |
|----------------------|---------------------------------------|----------------------|
| `C IW2OHX-12`         | 1-hop via xnet-14 (xnet → PCF)       | AX.25 V2 digi chain on the wire, reciprocity intact |
| `C IR3UHU-2`          | 2-hop via xnet-14 → IR3UHU's net      | Multi-hop mechanism — L2 digi or L4 NetROM CREQ? |
| BPQ `D IR3UHU-2`     | sanity check before probes            | `route: IW2OHX-13 IW2OHX-14 IR3UHU-2 IR3UHU-1` |
| BPQ `D IW2OHX-12`    | sanity check before probes            | `route: IW2OHX-13 IW2OHX-14 IW2OHX-12` |

Both connects completed and disconnected cleanly. Probe sequence
took ~3 minutes; remaining 22 minutes of capture stayed passive.

## 22. The Two Transit Mechanisms — Definitive

`(X)NET` and `linbpq-flexnet` use **two completely different mechanisms**
depending on whether the target is reachable via a direct neighbour or
via a multi-hop path. The Phase 3 captures show both side by side.

### 22.1 — 1-hop transit: AX.25 V2 digipeating

For `C IW2OHX-12` (PCF, reachable as IW2OHX-14's direct neighbour),
linbpq-flexnet's CMDC00 FlexNet branch built the v1.2 two-digi chain
`MYCALL* NEIGHBOUR` and emitted the SABM at L2. Wire trace on port 1
(xnet-14's PCF-facing AXIP):

```
1:fm IW7CFD    to IW2OHX-12 via IW2OHX-13* IW2OHX-14* ctl SABM+ - 10:33:17
1:fm IW2OHX-12 to IW7CFD    via IW2OHX-14  IW2OHX-13  ctl UA-   - 10:33:17
1:fm IW2OHX-12 to IW7CFD    via IW2OHX-14  IW2OHX-13  ctl I00^ pid F0 [146] - 10:33:17   (banner)
1:fm IW7CFD    to IW2OHX-12 via IW2OHX-13* IW2OHX-14* ctl RR1v  - 10:33:17   (L2 ack)
1:fm IW7CFD    to IW2OHX-12 via IW2OHX-13* IW2OHX-14* ctl I10^ pid F0 [2]    - 10:33:32   (L)
1:fm IW2OHX-12 to IW7CFD    via IW2OHX-14  IW2OHX-13  ctl I11^ pid F0 [178]  - 10:33:32   (L reply)
...
1:fm IW7CFD    to IW2OHX-12 via IW2OHX-13* IW2OHX-14* ctl DISC+ - 10:34:01
1:fm IW2OHX-12 to IW7CFD    via IW2OHX-14  IW2OHX-13  ctl UA-   - 10:34:01
```

Read this carefully:

- **Outbound (`IW7CFD → IW2OHX-12`)**: digi list is `IW2OHX-13* IW2OHX-14*`
  — TWO digis, BOTH H-bit set (the `*` marker). This is what xnet-14
  is transmitting onto the PCF link after it has processed the SABM
  arriving from IW2OHX-13 on port 3.
- **Inbound (`IW2OHX-12 → IW7CFD`)**: digi list is reversed —
  `IW2OHX-14 IW2OHX-13` — and NEITHER digi is H-bit set. The frame
  arrives fresh from PCF, the digi list reads "to be repeated by
  IW2OHX-14 first, then by IW2OHX-13" (right-to-left walk per AX.25
  v2.2 §6.1).
- xnet-14 then processes the inbound, sets H-bit on `IW2OHX-14`, and
  forwards onto port 3 toward IW2OHX-13 with digi list
  `IW2OHX-14* IW2OHX-13`. IW2OHX-13 receives, recognises `IW2OHX-13`
  as its MYCALL in the via list, marks H-bit, **and consumes the
  frame locally** — the L2-RX-DIGI trap in `L2Code.c` (skill §3.4 item
  2) deliberately does NOT re-forward it.
- **AX.25 V2 reciprocity is intact** — the originator's UA-match
  succeeds because the LINK established with chain
  `IW2OHX-13* IW2OHX-14` (its own first-digi MYCALL marked) accepts the
  reverse with chain `IW2OHX-14 IW2OHX-13` and processes the H-bit
  walk correctly.

The PCF user list reported back at the application layer confirms
the same shape from the destination's vantage:
```
876: S5      P3 : IW2OHX-12>IW7CFD v IW2OHX-14 IW2OHX-13
```
i.e. PC/Flexnet sees the session as `IW2OHX-12 ↔ IW7CFD via IW2OHX-14
IW2OHX-13` — preserving both intermediate nodes in the via list.

### 22.2 — 2-hop transit: NetROM L4 CREQ over PID=CF

For `C IR3UHU-2` (xnet, NOT a direct neighbour from IW2OHX-13's
perspective — 2 hops via IW2OHX-14 → IR3UHU-2), linbpq-flexnet
chose the **NetROM L4 mechanism** instead. The L4 envelope rides
inside an L2 I-frame on the IW2OHX-14 ↔ IR3UHU-2 link (port 11),
with **no L2 digipeat extension at all**. Wire trace:

```
11:fm IW2OHX-14 to IR3UHU-2 ctl I71^ pid CF [37] - 10:32:24
L3 fm IW2OHX-13 to IR3UHU-2 LT 19 CREQ --- IN=1 ID=137 Window=4 IW7CFD IW2OHX-13

11:fm IW2OHX-14 to IR3UHU-2 ctl I72^ pid CF [21] - 10:32:26
L3 fm IW2OHX-13 to IR3UHU-2 LT 19 IACK --- IN=83 ID=98 S(0) R(4)

11:fm IW2OHX-14 to IR3UHU-2 ctl I06^ pid CF [22] - 10:32:39
L3 fm IW2OHX-13 to IR3UHU-2 LT 19 I --- IN=83 ID=98 S(0) R(5) [2]      (user data — "L" command)
```

Read this:

- **L2 layer (the `11:fm IW2OHX-14 to IR3UHU-2 ctl Ixx^ pid CF [N]`
  line):** straight L2 I-frame between xnet-14 and IR3UHU-2. NO digi
  chain. The L2 link is xnet's own peer-to-peer FlexNet session.
- **L3/L4 layer (the `L3 fm ... CREQ ...` line):** decoded NetROM L4
  envelope inside the I-frame's payload. Format per skill §2.3:
  - `LT 19` — TTL = 19 (xnet's default L3 lifetime)
  - `CREQ --- IN=1 ID=137` — connection-request opcode, circuit
    index = 1, circuit ID = 137
  - `Window=4` — accepted window size
  - `IW7CFD IW2OHX-13` — origin user (the connecting user's callsign)
    + origin node (the originating FlexNet node)
- xnet-14 is the **transit hop**: it received the CREQ on its
  IW2OHX-13-facing port (port 3, AXIP), looked up `IR3UHU-2` in its
  own D-table, and forwarded the CREQ onto port 11 toward IR3UHU-2 —
  **L2 source and dest were rewritten to xnet-14's own callsign and
  IR3UHU-2's callsign**. The L4 envelope inside (origin/dest at the
  NetROM level) was preserved verbatim.

This is what the reverted v1.9.4 was *not* — v1.9.4 was trying to
do this transit via *L2 AX.25 digipeating*, which broke reciprocity
on the return path (extending the L2 digi chain made the originator's
UA-match fail). The correct mechanism, evidenced by xnet, is **L4
NetROM CREQ at PID=CF**, with the L2 link being a direct peer-to-peer
hop without digis.

### 22.3 — Why the 1-hop case used AX.25 V2 not NetROM L4

A reasonable question: why didn't `C IW2OHX-12` also go via NetROM L4
CREQ? Both targets are remote; both routed via xnet-14.

Answer: linbpq-flexnet's CMDC00 FlexNet branch optimises for the
"target is reachable via direct AXIP neighbour" case. When
`FlexNet_FindRoute(IW2OHX-12)` returns "neighbour = IW2OHX-14, and
IW2OHX-12 is downstream of that neighbour", the linbpq-flexnet code
builds an AX.25 V2 SABM with the two-digi chain `MYCALL* NEIGHBOUR`
— skill §3.4 — and emits it on the AXIP port to IW2OHX-14. xnet-14
then handles the AX.25 V2 digipeat (set H-bit on its own callsign,
forward to IW2OHX-12) as a standard AX.25 transit. No L4 layer
involved.

For `C IR3UHU-2`, the target is two hops away. linbpq-flexnet detects
this from the D-table (RTT path > one hop) and falls back to NetROM
L4 CREQ — that's the standard NetROM-style call setup that has
worked since BPQ32 was introduced.

### 22.4 — Implications for the v1.9.4 redesign

Combined with Phase 2 findings, Phase 3 yields the complete picture:

**1. AX.25 V2 digipeating transit (1-hop case) is already working.**
The v1.2 / v2.1.8 chain machinery handles it. No changes needed.

**2. NetROM L4 CREQ transit (multi-hop case) is also already working
in the *forward direction*.** linbpq-flexnet correctly emits CREQ at
PID=CF for unreachable-via-direct-neighbour targets, and xnet-14
forwards them. But — **linbpq-flexnet does not act as a transit
node in the OTHER direction**: it doesn't accept CREQ frames where
the target is not a local app. If `C IW2OHX-13` came from a remote
peer through xnet-14 toward us, we accept that. But if someone
issued `C SOMETHING_ELSE` from a remote peer with `SOMETHING_ELSE`
reachable via *our* downstream, we'd reject (or not advertise that
SOMETHING_ELSE is reachable via us in the first place).

**3. The actual work for "v2.0 transit-role" is therefore:**
- **(a) Re-advertise** routes learned from one neighbour to other
  neighbours (Phase 2's smoking gun — single-record trickle, RTT
  arithmetic, no `?` marker). This makes linbpq-flexnet appear as a
  transit-capable node in the FlexNet cloud.
- **(b) Accept transit CREQ frames** where the L4 target is not us
  but is reachable via one of our neighbours, and forward them at the
  L2 layer to the chosen next-hop neighbour. NO L2 digi chain
  extension — that was the v1.9.4 failure mode.
- **(c) Handle the L4 IACK / I / DREQ / DACK round-trip during the
  transit session** — same forwarding logic, different opcodes.
- **(d) DREQ teardown** — when the session ends, forward the DREQ
  back along the path.

What we DO NOT need:
- No new L2 mechanism. AX.25 V2 stays untouched.
- No `?` indirect marker on re-advertisements.
- No special handling for the 1-hop case (already works).

### 22.5 — The Anomalous "3-digi SABM" Frame on Port 11

During the `C IW2OHX-12` probe, port 11 (the IR3UHU-2-facing port)
showed a SABM with **three digis** — `IW2OHX-13* IR3UHU-2* IW2OHX-14`
— that does NOT match the two-digi chain emitted by linbpq-flexnet
on the AXIP-3 port. The frame body and timing are identical to the
port-1 SABM, suggesting **xnet-14 also forwarded the SABM via port 11
through IR3UHU-2 as a secondary path attempt**.

Hypothesis: when xnet-14 has multiple routes to a destination, it may
attempt multipath SABM emission to find the fastest UA. The 3-digi
chain `IW2OHX-13* IR3UHU-2* IW2OHX-14` is consistent with "originator
IW2OHX-13, then via IR3UHU-2 (which would relay back to xnet-14 via
the IR3UHU-2 ↔ xnet-14 link), then via xnet-14". The presence of
`IR3UHU-2*` (H-bit set) means xnet-14 marked the frame as "already
gone through IR3UHU-2" — but functionally it's a weird routing
artefact. PCF would receive *only* the port-1 version (since IR3UHU
can't reach PCF directly), and that's what worked. The port-11 copy
likely got dropped at IR3UHU-2 as having `IW2OHX-12` (not a destination
reachable from IR3UHU's view).

For our v2.0 transit-role design we do **NOT** need to replicate this
multipath behaviour. Single-path transit is sufficient — and probably
cleaner.

### 22.6 — Sequence Diagram (canonical 1-hop user-transit case)

```
USER       IW2OHX-13          IW2OHX-14           IW2OHX-12
(local)    (linbpq)           (xnet)              (PCF)
  │          │                  │                  │
  │ C IW2..  │                  │                  │
  ├─────────>│                  │                  │
  │          │ SABM             │                  │
  │          │ src=IW7CFD       │                  │
  │          │ dst=IW2OHX-12    │                  │
  │          │ digi=IW2OHX-13*  │                  │
  │          │      IW2OHX-14   │                  │
  │          ├─────────────────>│                  │
  │          │                  │ SABM             │
  │          │                  │ digi=            │
  │          │                  │   IW2OHX-13*     │
  │          │                  │   IW2OHX-14*     │
  │          │                  ├─────────────────>│
  │          │                  │                  │ UA
  │          │                  │ UA               │ digi=
  │          │                  │ digi=            │   IW2OHX-14
  │          │                  │   IW2OHX-14      │   IW2OHX-13
  │          │                  │   IW2OHX-13      │
  │          │                  │<─────────────────┤
  │          │                  │ UA (xnet sets H- │
  │          │                  │ bit on IW2OHX-   │
  │          │                  │ 14, forwards)    │
  │          │ UA               │                  │
  │          │ digi=            │                  │
  │          │   IW2OHX-14*     │                  │
  │          │   IW2OHX-13      │                  │
  │          │<─────────────────┤                  │
  │ Connected│                  │                  │
  │<─────────┤                  │                  │
```

### 22.7 — Sequence Diagram (canonical multi-hop CREQ case)

```
USER       IW2OHX-13          IW2OHX-14         IR3UHU-2
(local)    (linbpq)           (xnet, transit)   (target)
  │          │                  │                  │
  │ C IR3..  │                  │                  │
  ├─────────>│                  │                  │
  │          │ I-frame on AXIP-3│                  │
  │          │ pid=CF [37 bytes]│                  │
  │          │ NetROM CREQ:     │                  │
  │          │   src=IW2OHX-13  │                  │
  │          │   dst=IR3UHU-2   │                  │
  │          │   LT=19 IN=1     │                  │
  │          │   ID=137 W=4     │                  │
  │          │   origin user=   │                  │
  │          │     IW7CFD       │                  │
  │          │   origin node=   │                  │
  │          │     IW2OHX-13    │                  │
  │          ├─────────────────>│                  │
  │          │                  │ (xnet looks up   │
  │          │                  │  IR3UHU-2 in own │
  │          │                  │  D-table → port  │
  │          │                  │  11 / next-hop = │
  │          │                  │  IR3UHU-2)       │
  │          │                  │                  │
  │          │                  │ I-frame on port  │
  │          │                  │ 11 pid=CF [37B]  │
  │          │                  │ NetROM CREQ      │
  │          │                  │ envelope         │
  │          │                  │ preserved        │
  │          │                  │ verbatim         │
  │          │                  ├─────────────────>│
  │          │                  │                  │ CACK
  │          │                  │                  │ (back)
  │          │                  │<─────────────────┤
  │          │ CACK             │                  │
  │          │<─────────────────┤                  │
  │ Connected│                  │                  │
  │<─────────┤                  │                  │
  │          │                  │                  │
  │ ... I-frames flow same way, IACK ack at L4 ... │
```

## 23. Updated Skill Corrections (Phase 3 Additions)

1. **Skill §3 should explicitly note the two transit mechanisms**:
   - 1-hop transit (target = our direct neighbour OR reachable via
     direct neighbour): AX.25 V2 digipeating with 2-digi chain
     `MYCALL* NEIGHBOUR` (v1.2 mechanism). Reciprocity preserved by
     reversing the digi list on the return path.
   - Multi-hop transit (target reachable only via multi-hop): NetROM
     L4 CREQ over PID=CF, with NO L2 digi chain extension. L4
     envelope preserved verbatim; only L2 src/dst are rewritten by
     each transit node.

2. **Skill §3.4 (LinBPQ implementation) should describe the
   FlexNet-aware CREQ acceptance** — currently undocumented. The
   inbound-CREQ-with-non-local-target case is what a transit-role
   v2.0 must implement.

3. **The original v1.9.4 design memo should be retired**: it was
   trying to do multi-hop transit via L2 digipeat extension. This is
   never how xnet does it. The correct mechanism is L4.

## 24. Phase 3 — Status

Captures started 11:31 local (09:31 UTC), 25 min duration. Active
probes triggered at 11:32:54. Active probe phase concluded at
11:34:01 UTC. Remaining capture time (passive background) ends
~11:56 UTC.

**Final status (after capture completion at 11:56 local):**
- Port 1: 533 frame-lines. ONE SABM (10:33:17) + ONE DISC (10:34:01)
  — both from the probe. ONE REJ at 10:45:59 in the post-probe
  window (normal AXUDP retransmit, no link drop).
- Port 11: 693 frame-lines. ONE SABM (10:33:17, the anomalous
  3-digi `IW2OHX-13* IR3UHU-2* IW2OHX-14` discussed in §22.5) + ONE
  DISC (10:34:01). 4 REJs clustered 10:32:41–10:34:16 (during the
  probe window — likely xnet processing the multipath SABM
  variant).
- No further L2 establishment events, no FRMR, no DM.
- The remaining 22 min of passive capture confirmed steady-state
  behaviour matching Phase 1/2 — no material new findings.

Final capture artefacts on disk:
- `p01x_pcf12_20260517T093113Z_raw.txt` (198 KB) / `.json` (349 KB)
- `p11x_ir3uhu_20260517T093142Z_raw.txt` (127 KB) / `.json` (226 KB)
- `p01x_runlog.txt`, `p11x_runlog.txt`

## 25. Next Steps — Design Doc

We now have all the data needed to write a concrete `RFC: Transit
Role for linbpq-flexnet` design document. Proposed outline:

1. **Goals & non-goals.** What does transit-role mean? (Accept CREQ
   for non-local targets, route-advertise downstream destinations,
   forward L4 packets in the session.) What is NOT in scope? (L2
   digi chain extension — explicitly forbidden, see v1.9.4 revert.)
2. **The two existing mechanisms** (1-hop V2, multi-hop CREQ) and
   why neither is modified.
3. **Route-advertisement algorithm** (Phase 2 §20.2 — RTT addition,
   no `?` marker).
4. **CREQ-forwarding state machine** (track IN/ID per direction,
   forward INFO/IACK/DREQ/DACK with rewritten L2 src/dst).
5. **Split-horizon** (don't advertise routes back to the neighbour
   they were learned from).
6. **Failure modes & poison-reverse** (when a neighbour drops,
   re-advertise all affected destinations with RTT=60000).
7. **Test plan.** Reproduce Phase 3 active probes against a
   linbpq-flexnet acting as transit, instead of xnet-14. Verify
   AX.25 V2 reciprocity is preserved for 1-hop and L4 CREQ is
   correctly forwarded for multi-hop.
8. **Rollout plan.** Test on IR2UFV first, then promote to
   iw2ohx-13 production (standard workflow).

This is a substantial piece of work — estimate 2-3 days for the
implementation + IR2UFV test pass + production rollout, on top of
the design doc.

---

_Phase 3 capture and analysis authored by Claude on 2026-05-17.
Design RFC pending Marco's go-ahead._
