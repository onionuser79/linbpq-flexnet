# STATUS_10 framing investigation — why our pong saturates PCF, xnet's doesn't

**Date:** 2026-06-02
**Versions tested:** linbpq-flexnet v2.1.33 (KA-receipt-only pong), v2.1.34 (proactive 2-second pong)
**Outcome:** both reverted; the framing of our STATUS_10 frame differs from xnet-14's in a way that triggers PC/Flexnet's negative-delta wrap.
**Pcap reference:** `research/xnet14-pcf-2026-06-02.pcapng` (300 s capture on iw2ohx-bpq, 1112 packets, UDP/93 filter).

## Background

Production node IW2OHX-13 and test bed IR2UFV both run v2.1.32 — the stable floor on PC/Flexnet's `L *` table with cost **279/5** (16-sample ring at 277-281 ticks, no L2 cycling). xnet-14 on the same PC/Flexnet hub holds cost **1/1** with a 16-sample ring all at value 1, stable for 5+ days. The objective of this investigation was to identify what xnet-14 does that we don't, so we could close the cost gap while keeping the v2.1.32 stability.

## Wire evidence

5-minute pktmon capture of UDP/93 on the PC/Flexnet host (iw2ohx-bpq, OpenVPN HAMNET interface 44.134.27.17 ↔ xnet-14 at 44.134.24.4) shows the active-probe contract running at **~2-second cadence**, not the 16-second cadence the existing reverse-engineering notes had recorded:

| Direction | UDP length | Count in 300 s | Rate | Role |
|---|---|---|---|---|
| PCF → xnet-14 | 227 B | 152 | 0.51/s | keepalive `'2' + 199 spaces + CR` |
| PCF → xnet-14 | 25 B | 416 | 1.39/s | RR ack |
| xnet-14 → PCF | 29 B | 156 | 0.52/s | `"10\r"` pong |
| xnet-14 → PCF | 25 B | 18 | 0.06/s | RR ack |

The xnet-14 → PCF 29-byte frame contains the canonical FlexNet STATUS_10 payload. The exact wire bytes (AX.25 with FCS):

```
92 ae 64 9e 90 b0 f8     destination = IW2OHX-12  (PCF, SSID 12)
92 ae 64 9e 90 b0 7d     source      = IW2OHX-14  (SSID 14)
a8                       control byte
ce                       PID = FlexNet CE
31 30 0d                 info = '1' '0' '\r'  ("10\r")
32 5a                    FCS (2 bytes)
```

The control byte `0xa8` decodes as an AX.25 **I-frame** with N(R)=5 and N(S)=4 — the frame is part of a numbered sequence, and the FCS bytes are present. The pong carries L2 sequence information; PC/Flexnet ack's each pong with a numbered RR.

## Our equivalent

Our `flex_send_frame` (the path STATUS_10 takes in v2.1.33/v2.1.34) emits frames over BPQ's standard AXIP transport. The IR2UFV ↔ PCF capture from earlier in the day (v2.1.32 baseline) shows our 21-byte frame format for short payloads is structurally different — the control byte and FCS positioning don't match xnet-14's I-frame pattern.

When v2.1.34 sent `"10\r"` proactively at 2-second cadence (matching xnet's rate exactly), PC/Flexnet's `L *` saturated within **32 seconds** of the link going active:

```
IR2UFV  0-8  (4095/4095)   2m, 7s   P 5
       4095 4095 4095 4095 4095 4095 4095 4095 4095 4095 4095 4095 4095 4095 4095 4095
```

Every sample wrapped to 4095. The shape matches the negative-delta wrap [feedback_pcf_lt_rate_limit_floor](../../../memory/feedback_pcf_lt_rate_limit_floor.md) documents for the v2.1.0–v2.1.12 era: PC/Flexnet parsed our `"10\r"` as a type-1 link-time reply with value=10, ran the `link.ts = now + (smoothed+4) × 32` check, found our frame arrived ~2 s into a window that should have been 12.8–320 s, computed `delta = now - link.ts = NEGATIVE`, wrapped to the 12-bit cap.

xnet-14 sends "10\r" every 2 seconds without wrapping. The simplest explanation consistent with the wire evidence is that **PC/Flexnet dispatches by AX.25 control byte first**: a numbered I-frame ends up in the STATUS_10 path (sample the round-trip, store in ring, no `link.ts` check), and a non-numbered frame ends up in the LT path (apply `link.ts`, wrap on negative delta).

## Why v2.1.33 also failed (different reason)

v2.1.33 emitted `"10\r"` only on PC/Flexnet KA receipt. Because PC/Flexnet had us flagged "non-probable" (we'd never sent the expected pong), it KA'd us once every 5 min instead of once every 2 s. v2.1.33 therefore sent 1 STATUS_10 per 5 min vs xnet-14's 1 every 2 s — not enough wire frequency to populate PC/Flexnet's 16-slot ring with anything other than the regular KA-cadence inter-arrival samples (~280 ticks). Cost stayed at the v2.1.32 baseline. Same framing issue applied but the per-frame wrap was lost in the noise of the longer cadence.

## What would have to be true for a cost-1 fix

To replicate xnet-14's behaviour we'd need to emit `"10\r"` such that:

1. The AX.25 control byte is an I-frame with sequence numbers consistent with the BPQ session state on that link. `flex_send_frame` doesn't currently produce sequenced I-frames for short status payloads.
2. The frame carries the AX.25 FCS bytes the wire expects.
3. The N(R)/N(S) sequence stays in lockstep with PC/Flexnet's view of the L2 state — diverging would itself trigger a DISC.

Doing this from `flex_send_frame` would mean either:
- Adding a new emission path that hooks into BPQ's existing L2 sequence state for the LINK (uses `LINK->TXLASTACK`, `LINK->VS`, `LINK->VR`), or
- Sending the pong as if it were a normal data frame through BPQ's I-frame queue (`C_Q_ADD`), letting BPQ handle the sequence numbering automatically.

The second option is structurally less invasive but adds I-queue overhead and may not be fast enough to fire within the few-ms round-trip window the active-probe contract requires.

## Why this isn't being shipped

The two paths above need design work and meaningful soak time, and the v2.1.32 trade-off (cost 279/5 stable, no cycling) is operationally acceptable. The pcap (`xnet14-pcf-2026-06-02.pcapng`) and this note are the entry point for a future session that wants to revisit cost reduction with the framing problem clearly understood.

## Source-code study (2026-06-02 update) — supersedes the framing hypothesis

A read-only walk of the linbpq-flexnet + BPQ TX pipeline contradicts the "control-byte / FCS positioning" hypothesis above. The relevant call chain:

```
flex_send_frame(LINK, PID=0xCE, data, len)               FlexNetCode.c:3972
   │   sets Msg->PID = pid; memcpy(Msg->L2DATA, data, len);
   │   Msg->LENGTH = len + MSGHDDRLEN + 1
   ▼
C_Q_ADD(&LINK->TX_Q, Msg)                                FlexNetCode.c:3985
   │   no PID-based branching: CE frames take the same path as F0
   ▼
SDETX(LINK)                                              L2Code.c:3386 / 3450+
   │   while (LINK->TX_Q && LINK->FRAMES[SDTSLOT] == NULL):
   │     - Q_REM(&LINK->TX_Q)
   │     - compression branch (line 3466) is SKIPPED for PID 0xCE
   │       (compression only fires for PID 0xF0 and length > 20)
   │     - LINK->FRAMES[LINK->SDTSLOT] = Msg
   │   while ((LINK->L2FLAGS & POLLSENT) == 0):
   │     SETUPADDRESSES → writes DEST + SRC + digis
   │     CTL = (LINK->LINKNR << 5) | (LINK->LINKNS << 1)    line 3641-3642
   │           ↑ this is a NUMBERED I-FRAME control byte
   │     LINK->LINKNS++ (mod 8)                              line 3644-3645
   │     P-bit set on window-edge frames                     line 3658-3668
   │     copy PID + INFO after CTL
   │     PUT_ON_PORT_Q(PORT, Buffer)                         line 3693
   ▼
ExtProc case 2 (AXIP TX)                                 bpqaxip.c:641+
   │   compute_crc(&buff->DEST[0], txlen - 2)               line 647
   │   crc ^= 0xffff                                         line 648
   │   append crc little-endian at buff->DEST[txlen-2..-1]   line 650-651
   ▼
SendFrame → sendto(buff, txlen)                          bpqaxip.c:791+
```

**Verification against the wire.** Decoded IR2UFV→IW2OHX-12 frame from
`research/ir2ufv-pcf12-2026-05-25/soak-1h.pcap`, frame #4
(udp.length = 29 → 21-byte AX.25 payload):

```
92 ae 64 9e 90 b0 f8     DEST = IW2OHX-12
92 a4 64 aa 8c ac 61     SRC  = IR2UFV (SSID 0, end-of-list bit)
da                       CTL  = 1101 1010 = I-frame N(R)=6 N(S)=5 P=1
ce                       PID  = FlexNet CE
31 32 0d                 INFO = "12\r"  ← LT type-1, value=2 (v2.1.10 era)
34 5d                    FCS
```

This is structurally **identical** to xnet-14's STATUS_10 from
`xnet14-pcf-2026-06-02.pcapng` frame #177 — same length, same numbered
I-frame control byte shape (only N(R)/N(S) differ, as expected),
same PID, same 3-byte INFO + 2-byte FCS layout. The only on-wire
difference is the INFO digit: our v2.1.10 frames carried `"12\r"`
(LT value=2); v2.1.32 frames carry `"15\r"` (LT value=5, after
v2.1.28 raised `FLEXNET_WIRE_LT`). xnet sends `"10\r"` — value=0
is reserved as the STATUS_10 sub-PID.

**Conclusion.** Our short CE frames already are numbered I-frames
with proper N(R)/N(S) sequencing and AX.25 FCS appended by bpqaxip.c.
There is no UI-frame bypass, no missing FCS, no different CTL
positioning. The "structurally different" framing claim from §"Our
equivalent" above was based on a misread of the wire bytes and is
withdrawn.

**Implication for the 4095 wrap.** PC/Flexnet's dispatch can't be
distinguishing xnet-14 from us on AX.25 control byte alone — the
bytes are the same shape. The more likely dispatch is on the **INFO
bytes** (the 1-character FlexNet CE sub-PID):

- `'1' '0' '\r'` → STATUS_10 path: PCF samples the round-trip
  KA→reply directly into its 16-slot ring; xnet's 2-s cadence
  produces samples of ~1 tick → cost row stable at 1.
- `'1' '<1-9>' '\r'` → LT type-1, value = `<digit>`: PCF
  computes `link.ts = now + (smoothed_pcf + 4) × 32` ticks and
  samples the delta on the NEXT LT arrival; replies arriving
  inside the window underflow → wrap to 4095. This is the
  failure mode v2.1.13 fixed by rate-limiting our LT to ≥ 320 s.

If that re-classification is correct, then v2.1.33/v2.1.34's 4095
saturation was **not** caused by AX.25 framing at all. The most
plausible alternative explanations to test next:

1. **PCF's `link.ts` window is shared across all type-1 sub-PIDs**
   (STATUS_10 and LT type-1 with value 1-9). PCF maintains one
   `last_event_tick` per peer and any type-1 frame samples
   against it; xnet survives because at 2-s cadence its
   inter-arrival ticks (~20) are above PCF's saturation threshold,
   not because PCF dispatches xnet's `"10\r"` to a different
   handler. Our v2.1.33/34 emitted `"10\r"` proactively at the
   same 2-s rate but PCF's `smoothed` for us was already ~280
   (from the prior LT cycle), so its `link.ts` window had grown
   to `(280+4)×32 ≈ 9088 ticks ≈ 909 s` — our `"10\r"` arriving
   2 s into that window produced a huge negative delta → wrap.
   xnet doesn't hit this because its `smoothed` is 1 → window
   `(1+4)×32 = 160 ticks = 16 s`, so 2-s arrivals are inside
   the window but the wrap path is bounded.
   
   This hypothesis predicts that a v2.2-experimental build that
   **first lets PCF's `smoothed` decay to single digits** (by
   keeping current v2.1.32 behaviour for one ring cycle) and
   **then** opens the `"10\r"` faucet would succeed where v2.1.34
   failed. Convergence path: 280 → LT replies trim it →
   eventually small enough that 2-s STATUS_10 arrivals land
   on the right side of the window.

2. **PCF treats the very first `"10\r"` after a long quiet
   period as an LT (sub-PID dispatch is sequence-sensitive).**
   Less likely but checkable by counting frame boundaries on
   the wire.

3. **The `"10\r"` we emitted in v2.1.33/34 had different upstream
   context** (P-bit set unexpectedly, wrong N(R)) — would have
   shown up in r2/wire diffs we didn't take.

## What this changes in the roadmap

- The "two paths" sketched in §"What would have to be true" are
  no longer the right framing. Both presupposed our frames lack
  I-frame numbering — they don't.
- A future cost-reduction experiment should:
  (a) take a **fresh** pcap of the current v2.1.32 IR2UFV→PCF
      dialog to confirm the wire matches the source-code
      expectation,
  (b) build an experimental variant that emits STATUS_10
      `"10\r"` at xnet's cadence **but** only after PCF's ring
      has converged to small values,
  (c) instrument with N(R)/N(S) + tick logging at the moment
      of each STATUS_10 send,
  (d) deploy to IR2UFV only; production iw2ohx-13 stays on
      v2.1.35.

Until those experiments run, [feedback_status_10_pong_doesnt_help_cost](../../../memory/feedback_status_10_pong_doesnt_help_cost.md) still applies — don't re-enable the proactive `"10\r"` on the production node.

## Cross-references

- [feedback_status_10_pong_doesnt_help_cost](../../../memory/feedback_status_10_pong_doesnt_help_cost.md) — short rule: don't re-add the standalone pong without solving framing.
- [feedback_pcf_lt_rate_limit_floor](../../../memory/feedback_pcf_lt_rate_limit_floor.md) — the negative-delta wrap mechanism.
- [project_linbpq_v2_1_32_release](../../../memory/project_linbpq_v2_1_32_release.md) — current stable floor.
- [reference_pcflexnet_v4_re](../../../memory/reference_pcflexnet_v4_re.md) — earlier RE notes (cadence figure of "16 s" between PCF KAs turns out to be peer-specific; ~2 s is what the wire shows for a healthy xnet peer).
