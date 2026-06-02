# IR2UFV ↔ IW2OHX-12 (PC/Flexnet) — v2.1.35 wire capture

**Date:** 2026-06-02 16:33–16:38 CEST
**Capture:** `research/ir2ufv-pcf-v2.1.35-2026-06-02-eth0.pcap`
(5 min, eth0, UDP port 10075, host 192.168.1.201, 44 packets)
**Capture host:** iw2ohx-gw (192.168.1.202) — IR2UFV second linbpq
instance at `/home/bpq-ufv/linbpq`
**Companion:** earlier `-2026-06-02.pcap` was on `-i any` and only
caught 18 packets; superseded by the eth0 capture.

**Baseline `L *` from PCF side (user-supplied, 16:25 CEST):**
```
IR2UFV  0-8  ( 299/5)    53m,38s P 5
                 600 180 279 280 279 279 280 279 279 277 280
```

## Packet-size distribution (eth0 capture, 5 min)

| UDP len | Dir       | Count | What it is |
|---------|-----------|------:|------------|
| 25      | both ways | 11+11 | AX.25 supervisory (RR) — 17 B AX.25 payload |
| 267     | us → PCF  | 10    | our universal KA, 241-byte INFO `'2'+240 spaces` |
| 31      | PCF → us  | 10    | PCF type-1 LT, 5-byte INFO `"1293\r"` |
| 29      | us → PCF  | 1     | our type-1 LT, 3-byte INFO `"15\r"` |
| 227     | PCF → us  | 1     | PCF KA, 201-byte INFO `'2'+199 spaces+CR` |

Cadence:
- our KAs at ~30 s intervals (matches `FLEXNET_KA_PCF` 29 s threshold)
- PCF LT-like 31-B frames also at ~30 s intervals
- our LT at 320 s (single fire in 300 s window)
- PCF KA at ~5 min (sparse)

## Byte-level decode

### OUR outbound 267-B KA (representative)

```
DEST  : IW2OHX-12 (eol=0, H=1)
SRC   : IR2UFV-0  (eol=1, H=0)
CTL   : 0x36 = I-frame N(R)=1 N(S)=3 P/F=1
PID   : 0xCE                                     ← FlexNet ✓
INFO  : 241 bytes = '2' + 240 × ' '              ← universal v2.1.13 KA shape
FCS   : 3a 8a
```

Subsequent KAs in the capture show CTL = 0x36 → 0x58 → 0x7a → 0x9c →
0xbe → 0xd0 … — N(R)/N(S) walking forward correctly, P/F set on each.
`flex_send_frame → C_Q_ADD(TX_Q) → SDETX` is producing the I-frame
shape the source-study predicted.

### OUR outbound 29-B LT (single in 5 min)

```
CTL   : 0x7a = I-frame N(R)=3 N(S)=5 P/F=1
PID   : 0xCE
INFO  : "15\r"                                   ← FLEXNET_WIRE_LT = 5 ✓
```

Matches the v2.1.28 tuning of `FLEXNET_WIRE_LT` from 2 → 5.

### PCF inbound 227-B KA

```
DEST  : IR2UFV-0  (eol=0, H=1)
SRC   : IW2OHX-12 (eol=1, H=0)
CTL   : 0xea = I-frame N(R)=7 N(S)=5 P/F=0
PID   : 0xF0                                     ← !!! NetROM "no L3"
INFO  : 201 bytes = '2' + 199 × ' ' + '\r'       ← FlexNet KA shape
FCS   : 82 5b
```

### PCF inbound 31-B frame (10× in capture, every ~30 s)

```
CTL   : 0xe8, 0x0a, 0x2c, 0x4e, 0x60 …           ← numbered I-frame, walking
PID   : 0xF0                                     ← !!! NetROM "no L3"
INFO  : "1293\r"                                 ← FlexNet type-1 LT value=293
```

Value 293 in INFO matches the live ring (`L *` row shows samples
277-281, smoothed ~280, current snapshot 299). PCF is telling us
*its* smoothed RTT to us — that's the spec semantic for type-1 LT
inbound.

**FCS verification (clinches the byte-offset analysis):** running
CRC-CCITT (init 0xffff, poly 0x8408, final XOR 0xffff) over the
first 21 bytes of one of these frames returns exactly the 2 FCS
bytes on the wire. So PID *is* at offset 15 and *is* 0xF0; the
analysis isn't off-by-one.

```
Frame  : 92a464aa8cace0 92ae649e90b079 e8 f0 31 32 39 33 0d ef b5
         └──DEST IR2UFV─┘└──SRC OHX-12─┘│  │  └──"1293\r"──┘ └FCS┘
                                       CTL PID
Computed FCS over [0..20] = 0xb5ef  ←  matches wire little-endian
```

## The headline anomaly

**PCF→us inbound I-frames carry PID = 0xF0, not PID = 0xCE.**

The INFO content is FlexNet-shaped (`'2' + spaces` KA, `'1' + digits
+ CR` LT) but the L2 PID byte is 0xF0 ("no layer 3 protocol",
historically used for NetROM nodes and user text).

This was **not** the case in the 2026-05-25 era (`soak-1h.pcap`
captured under v2.1.10): every PCF→us frame in that capture carried
PID = 0xCE. Comparison from the same Python decoder:

| Capture | Date     | linbpq-flexnet ver | PCF→us PID(s) |
|---------|----------|--------------------|---------------|
| soak-1h | 05-25    | v2.1.10            | CE × every I-frame |
| eth0    | 06-02    | v2.1.35            | F0 × every I-frame |

Something in the v2.1.10 → v2.1.35 line caused PCF to demote our
peer entry from FlexNet PID (CE) to NetROM PID (F0). Candidate
triggers (none verified yet):

1. **v2.1.27** — we now drop non-CE/CF PIDs on FlexNet-flagged
   links (L2Code.c:3149). If PCF ever sent us a PID=F0 banner and
   we DM/dropped it without explanation, PCF may have flipped our
   protocol class.
2. **v2.1.25 / v2.1.26 adoption hooks** — SABM-accept adoption
   could be re-initialising session state in a way PCF interprets
   as "this peer doesn't speak FlexNet".
3. **v2.1.32 transit-OFF** — periodic record re-advertisement was
   removed; PCF's record-stream parser may class us as inactive on
   the FlexNet side and downgrade.
4. **PCF-internal demotion on cost saturation** — if PCF's ring
   stays at ~280 long enough, the per-peer protocol class may
   demote.

## What this means for cost reduction

Because v2.1.27 drops PID=F0 at L2 (L2Code.c `default:` →
`if (LINK->FlexNetLink) ReleaseBuffer(Buffer); return;`):

- **We never process PCF's inbound LT** carrying its smoothed RTT
  to us. The FlexNet protocol's bidirectional smoothing convergence
  can't run — we're permanently one-sided.
- **We never process PCF's KA** (the 227-B sparse one), so
  `last_keepalive` on our side only updates from our own proactive
  KA emission, not from peer activity.
- **We never reply with STATUS_10** or any peer-driven response,
  so PCF's L * ring samples whatever default cadence PCF measures
  (probably RR-roundtrip + our outbound KA arrival).

The cost-reduction theory previously was "tune our outbound cadence
to land samples in PCF's window". That's still half-true, but the
**bigger** lever is now visible: get PCF to either (a) send us
PID=CE again, or (b) accept our handling of PID=F0 frames as
FlexNet content.

Option (b) is cheaper to test:

```c
// L2Code.c case default (after the FlexNetLink drop guard):
//   if (LINK->FlexNetLink) {
//       /* v2.2 — recover FlexNet content delivered with PID=F0.
//          PCF demotes the protocol class on some unknown trigger
//          but still emits CE-shaped INFO ('2'+spaces KA, '1'+digits
//          LT). Re-dispatch to the CE handler when INFO matches a
//          CE sub-PID. */
//       if (Length >= 2 && Info[0] == '2') {
//           /* KA */ FlexNet_ProcessCE(LINK, Buffer-as-CE);
//           return;
//       }
//       if (Length >= 3 && Info[0] == '1' && Info[Length-1] == '\r') {
//           /* type-1 LT */ FlexNet_ProcessCE(LINK, Buffer-as-CE);
//           return;
//       }
//       /* fall through to v2.1.27 drop */
//   }
```

(Pseudocode; the real change needs to reshape the Buffer like the
existing `case 0xce:` branch does before calling `FlexNet_ProcessCE`.)

## Next steps

1. **Investigate why PCF demoted us to PID=F0.** Compare against
   xnet-14's current relationship with PCF (xnet still receives
   PID=CE per the 06-02 pcap) and look for the inflection point
   in our v2.1.x line. Candidate diff bisection: v2.1.22 (adoption),
   v2.1.27 (drop), v2.1.28 (KA cadence), v2.1.32 (transit-OFF).
2. **Test PID=F0 → CE re-dispatch** on IR2UFV only. If PCF's L *
   cost falls from 280 toward single digits after that change,
   the cost-reduction story is resolved (and is unrelated to
   STATUS_10 framing).
3. **Update memory** to reflect that the cost-reduction blocker
   is most likely the v2.1.27 drop interacting with PCF's PID=F0
   demotion, not the framing of the `"10\r"` pong.

## Cross-references

- [status_10_framing_investigation_2026-06-02.md §"Source-code
  study"](./status_10_framing_investigation_2026-06-02.md#source-code-study-2026-06-02-update--supersedes-the-framing-hypothesis)
  — confirms our outbound framing is correct.
- [v2.1.27 fix](../../ROADMAP.md) — original rationale for the
  non-CE/CF drop (banner-echo → PCF DISC pattern from 2026-05-30
  wire study).
- `flexnet_axudp.log` shows `pid=CE` for PCF→us frames in the same
  timeframe — this is a known instrumentation mismatch; the log
  is shared with the production iw2ohx-13 linbpq and lines may
  cross-contaminate. The wire pcap is authoritative.
