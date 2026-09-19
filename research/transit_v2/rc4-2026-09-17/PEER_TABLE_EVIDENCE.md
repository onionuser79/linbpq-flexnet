# rc4 transit re-advertisement — verified from the peers' own tables

**2026-09-17, IR2UFV running `v2.2.0-rc4` (flexdebug, `FLEXNETTRANSIT YES`).**
Queried through `pr-digi-gw`, ~15 min after the session came up.

This is the observation rc1, rc2 and rc3 never produced: the peers have
installed routes **through** IR2UFV. `D < CALL` on an (X)Net node lists
the destinations it reaches via that neighbour, so this is the peer's own
routing decision, not our claim about what we sent.

## `D < IR2UFV` on IW2OHX-14

```
HB9AK   1-1   1113  HB9AK  13-13  1112  IR2UFV  0-8      1  IW2OHX  4-4      3
```

- `IR2UFV 0-8 1` — our own record, SSID range intact, cost 1.
- `IW2OHX 4-4 3` — **IW2OHX-4 reached via IR2UFV.** We learned -4 as a
  direct neighbour and re-advertised it to -14 at `learned(1) + link_rtt(2)`.
- `HB9AK` at 1112-1113 — transit, at a cost that loses to -14's own path.

Only four rows, and that is the correct outcome: -14 is the hub that
taught us most of the table, so for those destinations its own path
always beats ours. Split-horizon means we never offered them back.

## `D < IR2UFV` on IW2OHX-4 — ~110 destinations

```
DB0GW   0-9      9  DB0IUZ  4-15    10  DB0LJ   0-15    10  DB0OVN  0-15    10
DB0RES  0-9      9  DB0RES 10-10     8  DB0WAL  0-6     10  DB0WAL  7-12    11
DB0WTS  4-15    11  DK0WUE  0-13     7  F3KT    0-10     8  HB9AJ   4-4   3096
HB9AK  14-14  2053  HB9ON   2-2     30  HB9ON   3-3     34  HB9ON   4-4     30
HB9ON   6-6     30  HB9ON   8-8     34  HB9ON  10-10     7  HB9ON  14-14     7
HB9ON  15-15     6  I0OJJ   3-3      8  IGATE   0-15     6  IGATEB  0-15     6
IK1NHL  2-3      9  IK1NHL  4-15    11  IK6IHL  6-6     38  IR2UFV  0-8      1
IR6AQS  0-0     38  IR8CSB  0-0     34  IW2OHX  3-3      6  IW2OHX 14-14     3
IW6NDX  0-14     8  IW8PGT  3-3     34  IW8PGT  8-8     34  IW8PGT 14-14     8
IW8PGT 15-15     7  K1YMI   0-15   169  K1YON   1-4     23  K2PUT   1-4     26
N2KGC   1-14    10  N4FLA   0-4    309  N9LYA   1-14    21  NC2C    1-9     71
OK0NAG  0-5      9  PE1FAM  0-0      8  PE1NNZ  1-1      8  PI1CDR  0-0     38
PI1ZTM  0-0     34  PI1ZTM  4-4      7  PI8ZTM  0-0     34  SV1CMG  1-1     38
SV1CMG  3-3      8  SV1CMG  4-4     38  SV1CMG  5-5     38  SV1CMG  6-6     38
SV1CMG 14-14    38  SV1CMG 15-15    38  SV1DZI  4-4     34  SV1DZI  5-5     34
SV1DZI 10-10     7  SV1DZI 13-13    34  SV1DZI 14-14    34  SV1DZI 15-15    34
SV1HCC  5-5     38  SV1HCC  6-6     38  SV1HCC 10-10    38  SV1HCC 11-11    38
SV1HCC 14-14    38  VA3BAL  1-1     39  VA3BAL  2-2     39  VA3BAL  5-5     39
VA3BAL  7-7     39  VA3BAL  8-8      8  VA3BAL  9-9     39  VA3BAL 10-10    39
VA3BAL 12-12    39  VA3BAL 13-13    39  VA3PJB  2-2     36  VA3PJZ  6-6     36
VE3LNZ  1-1     43  VE3LNZ  2-2     43  VE3LNZ  3-3     43  VE3LNZ  5-5     43
VE3LNZ  8-8      8  VE3LNZ  9-9     43  VE3LNZ 10-10    43  VE3LNZ 11-11    43
VE3LNZ 12-12    43  VE3MCH  5-5     36  VE3MCH  9-9     36  VE3MCH 10-10    36
VE3MUS  2-2     42  VE3MUS  5-5     42  VE3MUS  7-7     42  VE3MUS  8-8      8
VE3MUS  9-9     42  VE3MUS 10-10    42  VE3TOK  0-0     36  VE3TOK  1-1     36
VE3TOK 10-10    36  VE3TOK 11-11    36  VE3TOK 12-12    36  W1EDH   0-14   114
W2KPQ   0-14   145  W2KPQ   8-8     23  W4MLB   1-1     27  W4MLB   2-14    24
W4OT    1-1    129  W4OT    2-14   101
```

German (DB0\*, DK0WUE), Dutch (PE1\*, PI\*), Greek (SV1\*), Swiss
(HB9\*), Czech (OK0NAG), French (F3KT), Canadian (VA3\*, VE3\*), US
(K\*, N\*, W\*), the IGATE pair — the world IR2UFV learned from -14,
now reachable from -4 through us. Plus `IW2OHX 14-14 3`, the
re-advertised direct neighbour.

## Why the two peers differ so much

-14 is the hub and -4 the sparser side. IR2UFV learned ~126
destinations from -14 and ~119 from -4; split-horizon sends each set
only to the *other* peer. -14 already holds a better path to everything
it taught us, so our records lose on cost and only 4 rows survive in its
table. -4 does not, so ours win and ~110 rows install. **IR2UFV is now a
real transit path between IW2OHX-4 and the wider FlexNet world** — which
is the entire point of G1, and it is visible from outside the node.

## What this does and does not prove

Proves: G1 works on the wire, at the right costs, with split-horizon
holding, with the SSID range preserved, and with **no `?` indirect
prefix** anywhere in either table (§5.8).

Does not prove: G2/G3 CREQ forwarding (§6 — still never observed
firing), poison-reverse (§5.7 — the reaper hook landed the same day and
has not yet been exercised), or PCF safety over 24 h (§10.3, the hard
gate). PCF's reported link time to us was climbing during this window —
19 s pre-deploy, 156 s at 11:02 — which needs to come back down before
Phase 3 can be called anything but open.

---

## PC/Flexnet IW2OHX-12's own view — the Phase 3 gate, and it is OPEN

`L` on IW2OHX-12 at 11:15, ~26 min after IR2UFV came up on rc4:

```
IR2UFV  0-8    783/5    P 5
IQ2LB   0-5  (  32/50)  P 0
IW2OHX 14-14     1/1    P 3
IW2OHX  4-4      1/1    P 4
IQ2LB   6-6      1      P 2 @
IQ2LB   0-5     15/25   P 1
```

`D IR2UFV` on -12 resolves cleanly — `*** IR2UFV (0-8) T=1`, `route:
IW2OHX-12 IR2UFV` — so our own record arrived intact with its SSID
range.

But **cost 783** is the worst link in PCF's table, against `1/1` for its
two (X)Net peers. For reference, `ROADMAP.md` records ~**180/5** for a
`FLEXNETTRANSIT=OFF` node and ~**2/5** for rc2 with transit on, and
**4095** is the saturated state that broke rc1-rc3 and needs a manual
reset at the PC/Flexnet end. 783 is a long way from saturated and a
long way from healthy.

### Honest reading, including a measurement gap

**A PCF-side baseline was not captured before deploying.** The
pre-deploy reading taken was IR2UFV's *own* `FL` LT column (19 s), not
PCF's `L` row, so 783 has nothing from the same vantage point to be
compared against — only the ROADMAP's historical figures, which come
from different versions. That is a gap in the method, not something to
argue around: the right pre-deploy snapshot is `L` on **both** ends.

What can be said:

- It is **falling**, not climbing: IR2UFV's view of PCF's reported link
  time went 156 s → 117 s → 94 s across the window.
- IR2UFV was **restarted twice inside 25 minutes**, and each restart
  sends a fresh CE INIT. v2.1.13's finding is that PCF treats every
  INIT as a new peer and reseeds its 16-slot cost ring, taking ~5 min
  to converge back — two reseeds is consistent with a ring still
  holding outliers.
- PCF's cost ring measures **inter-event timing**, so the number should
  fall while we emit and drift up when we go quiet. The queue to -12
  only emptied at ~11:12, so this reading sits at the transition
  between "draining at 1 record / 5 s" and "steady state at the 120 s
  neighbour refresh" — the two regimes that bracket the answer.
- **No L2 cycling at all:** the -12 session held for 21+ minutes of
  continuous transit emission, which is the behaviour rc1-rc3 could
  never achieve.

### What this means for Phase 3

**Do not read this as PCF being safe, and do not read it as PCF being
harmed.** It is unresolved, which is what the 24 h soak in §10.3 exists
to settle. The soak should start only from a quiet node that has not
been restarted for at least 30 minutes, with `L` captured on both ends
first, and it needs an explicit operator OK because the documented
failure mode requires manual intervention at the far end.
