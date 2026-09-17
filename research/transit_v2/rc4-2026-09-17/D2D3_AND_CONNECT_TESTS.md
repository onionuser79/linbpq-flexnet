# rc4 Phase 1 §10.1.c — D2/D3 poison-reverse + end-to-end connect test

**2026-09-17, IR2UFV on `v2.2.0-rc4` with the corrected reaper hook.**

## D2 / D3 — poison-reverse on peer loss: **both PASS**

Method: `iptables -I OUTPUT -d 192.168.1.203 -p udp --dport 10075 -j DROP`
for 240 s, cutting IR2UFV → IW2OHX-4 only. Production IW2OHX-13 is on
UDP 10093 and cannot be touched by that rule. PC/Flexnet was
deliberately not the target — a sustained burst there can require a
manual reset at the far end.

On the session going away, `flex_advertise_poison_session()` walked
**116 learned destinations**:

| | Count | Behaviour |
|---|---|---|
| **D3** — alternate path exists | **115** | `alt=IW2OHX-14` → **no withdrawal sent**, left for -14's own next change event |
| **D2** — no alternate | **1** | `IW2OHX-4/4 alt=(none, poisoning)` → withdrawn with RTT=60000 |

```
FlexNet: POISON peer-down=IW2OHX-4 dest=IW2OHX-4/4   alt=(none, poisoning)
FlexNet: POISON peer-down=IW2OHX-4 dest=HB9ON-15/15  alt=IW2OHX-14
FlexNet: POISON peer-down=IW2OHX-4 dest=IGATE-0/15   alt=IW2OHX-14
…
FlexNet: peer IW2OHX-4 down — 116 learned routes, 1 withdrawn, 115 covered by another peer
```

### Two things this proves that inspection could not

**1. The §5.7 deviation was necessary, not stylistic.** §5.7 step 1 says
to walk "each entry that *isn't* a direct neighbour". The single
destination with no alternate here **is** the direct neighbour —
`IW2OHX-4` itself. Following the spec literally would have sent
**zero** withdrawals, leaving IW2OHX-14 and IW2OHX-12 believing
IW2OHX-4 was still reachable through IR2UFV: a black hole, from the
mechanism whose entire job is to prevent one.

**2. The reaper hook was the whole ball game.** POISON had never fired
once before this change, across rc3 and rc4, because the walk was
hooked on `FlexNet_CloseSession` while real peers die through
`FlexNet_Timer`'s ghost reaper.

### The wire result that matters most

The pcap over the drop and recovery (`ir2ufv-d2d3.pcap`, 988 datagrams,
474 CE frames):

| Peer | Records | Poison | Median inter-record gap | Bucket period |
|---|---|---|---|---|
| IW2OHX-14 (xnet) | 189 | **57** | **2.03 s** | 2 s |
| IW2OHX-12 (PCF) | 88 | **1** | **4.98 s** | 5 s |
| IW2OHX-4 (down) | 90 | 0 | 2.02 s | 2 s |

**A mass-withdrawal event — the single worst case for emission volume —
drained at exactly the per-peer bucket rate.** This is precisely what
destroyed rc1: 326-403 back-to-back records saturated PC/Flexnet's RTT
at 4095 and put it into a DISC loop needing manual recovery. Here the
same class of event produced 57 withdrawals to the xnet peer at 1 per
2 s and 1 to the PCF peer, with no session loss on either.

**Why 57 withdrawals to -14 when the log says only 1 had "no
alternate".** The log's `alt=` is a *global* reachability check; the
per-peer decision in `flex_advertise_check` is stricter and correctly
so. For a destination whose only non-(-14) source was -4, advertising
to -14 must now be withdrawn, because split-horizon bars us from
re-using -14's own route. Continuing to advertise it would mean
offering -14 its own path back — a loop. Both numbers are right; they
answer different questions.

Recovery was clean: `session started … SEED peer=IW2OHX-4 entries=129
queued=128` — the session-up seed re-offering the full view.

## End-to-end connect test — **succeeds, but does not transit IR2UFV**

Method: a user telnets to IW2OHX-14, logs in, and connects to
`IW2OHX-4` — a destination IR2UFV advertises — while IR2UFV's AXIP port
is captured and its `CF-TRANSIT-FWD` / `CF-NOT-L3RTT` counters are
sampled either side.

```
-14| link setup (1)...
-14| *** connected to IW2OHX-4
-14| IW2OHX-4 - (X)NET/TNC4e V1.39 Digipeater Bollate (MI) JN45NN
```

**The connection succeeded.** The far end answered interactively
(`bbs is not active.` to a bare CR), so this was a live circuit, not a
half-open one.

**It did not go through IR2UFV.** `CF-TRANSIT-FWD` delta **0**,
`CF-NOT-L3RTT` delta **0** — IR2UFV saw no CF frame at all during the
connect. `link setup (1)` names -14's link 1, which its `L` table shows
as `IW2OHX-12`. The path was **-14 → -12 → -4**.

### Why, and what it takes to change

IR2UFV currently **ties or loses on cost for every destination**, and
(X)Net keeps the incumbent on a tie:

| Query | Result |
|---|---|
| `D IW2OHX-4` on -14 | `T=3`, `route: IW2OHX-14 IW2OHX-12 IW2OHX-4` |
| `D < IR2UFV` on -14 | `IW2OHX 4-4 3` — same cost, via us |
| `D IGATE` on -4 | `T=4`, `route: IW2OHX-4 IW2OHX-12 IW2OHX-14 IGATE` |
| `D < IR2UFV` on -4 | `IGATE 0-15 6` — we are 2 dearer |

PC/Flexnet IW2OHX-12 is the better-connected hub between -4 and -14
(cost 1/1 to each), so every path through it beats or matches ours.
Cross-referencing -4's full `D` against `D < IR2UFV` shows a large
tied block (the SV1\*/VA3\*/VE3\* routes at 36-43, `IW2OHX 14-14` at 3,
`DB0GW`/`DB0RES` at 9) and **no destination where IR2UFV strictly
wins**.

**So §6 CREQ forwarding remains unexercised, and this is a topology
fact, not a code fault.** The hook is demonstrably reached — 15 inbound
CF frames in an earlier window, every one logged `CF-NOT-L3RTT` and
correctly declined as locally-addressed. What has never arrived is a
frame for a third party.

Making one arrive needs IR2UFV to be somebody's *best* path, which
means one of:

1. **A brief routing change on a live shared hub** — `ro fl del <port>
   iw2ohx-12` on -14, then restore. -14 ↔ -12 carries 1.1M/3.5M bytes
   and serves other operators; removing it would flap 7 destinations
   mesh-wide. **Operator's call, not to be done unilaterally.**
2. **A controlled test pair** — give IR2UFV a peer that has no other
   route to somewhere, so the tie cannot arise.
3. **Waiting for a natural -12 outage** and having the capture armed.

Note that option 1 is the lever RFC §"Operational Lessons" already
names for exactly this purpose; the objection is its blast radius on a
shared network, not its correctness.

## B-test status after this run

| Test | Result |
|---|---|
| B1, B2 | PASS |
| B3 — 120 s neighbour cadence | **PASS** — max inter-record gap 119.99 / 120.01 s on all three peers |
| B5 — bucket cadence | **PASS** — median 4.98 s (PCF, period 5) and 2.02-2.03 s (xnet, period 2), held even through the 57-record poison burst |
| B6 — split-horizon | **PASS** — zero real violations; the one SUSPECT (`K2PUT-1/4`) was disproved by IR2UFV's own `route: IR2UFV IW2OHX-4 K2PUT` |
| B7 — jitter ratio | Measured, not a gate — see RFC §13.3 |
| B8 — RTT=0 skip | **PASS** — `rtt0-skips` counter live (7 after restart), zero `exp=0 … FIRED` |
| D1 — `3+` walk | PASS — observed repeatedly, incl. `entries=127 queued=124` |
| D2 — poison when no alternate | **PASS** |
| D3 — no poison when alternate exists | **PASS** (115 of 116) |
| T40-T43 — CREQ forwarding | **Blocked on topology** — see above |

---

## IR2UFX controlled test pair — built, and it found a different bug

Built as `/home/bpq-test/` (NODECALL `IR2UFX`, telnet 2727, AXIP UDP
10077) peering **only** with IR2UFV, so -14/-4/-12 would have no
alternate route and a connect would have to transit us. Callsign chosen
distinct from IR2UFV because §6 gates on `memcmp(l3_dst, MYCALL, 6)`.

### What worked

The FlexNet layer did exactly what rc4 says it should:

```
FlexNet: learned IR2UFX (0-0) RTT=1 from session 3
FlexNet: ADVERT-CHECK peer=IW2OHX-14 dest=IR2UFX-0/0 exp=3 last=-1 delta=3 FIRED
FlexNet: ADVERT-CHECK peer=IW2OHX-4  dest=IR2UFX-0/0 exp=3 last=-1 delta=3 FIRED
FlexNet: ADVERT-CHECK peer=IW2OHX-12 dest=IR2UFX-0/0 exp=3 last=-1 delta=3 FIRED
```

…and IR2UFV then seeded IR2UFX with its full ~120-destination view
through the xnet-family bucket, while IR2UFX correctly learned
`IR2UFV (0-8)`, `IW2OHX (12-12) RTT=2`, `IW2OHX (14-14) RTT=3`.

Every time the link dropped, poison-reverse fired correctly —
`POISON peer-down=IR2UFX dest=IR2UFX-0/0 alt=(none, poisoning)` followed
by `ADVERT-CHECK peer=IW2OHX-14 dest=IR2UFX-0/0 exp=60000 last=3
delta=59997 FIRED`. More §5.7 validation, unintentionally.

### What didn't: the L2 link cycles every ~11 s

IR2UFX **sends the DISC itself**, deterministically, ~11 s after each
session comes up:

```
CE-STATUS-1n: from=IR2UFX digit=5
AXUDP-TX: IR2UFV -> IR2UFX ctl=RR(0xD1)
AXUDP-TX: IR2UFX -> IR2UFV ctl=DISC(0x53)
AXUDP-TX: IR2UFV -> IR2UFX ctl=UA(0x73)
```

Tried and did **not** fix it: locked `ROUTES:` entries on both sides
(verified installed — `2 IR2UFV 150 0!`), `IDLETIME=0`,
`NODESINTERVAL=1`, removing the BBS `APPLICATION` line. IR2UFX's `Nodes`
table stays empty, so no NET/ROM relationship ever forms before the
link dies. The instance was stopped and its `MAP`/`ROUTES` entries
removed from IR2UFV, because a peer flapping on an 11 s cycle
advertises and withdraws a destination to three live mesh peers every
11 s — unacceptable churn on a shared network. The config survives at
`/home/bpq-test/bpq32.cfg` and `bpq32.cfg.with-ir2ufx-testrig` on
IR2UFV for a second attempt.

### The bug this exposed: linbpq cannot parse its own link-time frame

`flex_send_link_time` builds the LT frame with
`FLEXNET_WIRE_LT = 5` → `flex_build_link_time` emits **`"15\r"`, three
bytes**. But `flex_parse_ce_frame` classifies a 3-byte `"1n\r"`
(n = 1..9) as `CE_FRAME_STATUS_1N`, and only reaches
`CE_FRAME_LINK_TIME` for `data[0]=='1' && len > 3`.

So between two linbpq-flexnet nodes the LT handshake **cannot
complete**: the receiver never calls `flex_link_time_sample()` (it is
only called from the `CE_FRAME_LINK_TIME` case), never replies with its
own LT, `lt_tx_pending` stays `TRUE` forever, and `our_link_time` stays
at its seeded 2 — so every transit cost we compute through such a peer
is based on a default, not a measurement.

This is the mirror of the trap in `CLAUDE.md` hard rule 5 — "a
TX-side-correct format has shipped with an accidentally-correct parser
before". It has survived because **linbpq has never peered with
linbpq**: every real peer is (X)Net or PC/Flexnet, both of which send
multi-digit LT values (`len > 3`) and evidently accept our 3-byte one.

**Do not just widen the parser.** The `STATUS_1N` branch exists because
xnet is observed sending `"12\r"`, whose semantic the code notes as
undocumented and treats as a benign status. Reclassifying 3-byte
`"1n\r"` as LINK_TIME would make us fold xnet's `"12\r"` as a link-time
reply and answer it — a behaviour change against stable live peers, on
a guess. What settles it is a capture of how xnet responds to a 3-byte
LT, which is now worth doing for its own sake: if xnet ignores ours
too, then `FLEXNET_WIRE_LT = 5` means we have never had a working LT
round-trip with *any* peer, and the `our_link_time` feeding every
transit cost is always the seeded default. `FL` showing `LT 60s` for
-14 is `peer_link_time` — what the peer tells *us* — not our own
measurement, so it does not disprove this.
