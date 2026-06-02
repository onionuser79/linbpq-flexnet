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

## Cross-references

- [feedback_status_10_pong_doesnt_help_cost](../../../memory/feedback_status_10_pong_doesnt_help_cost.md) — short rule: don't re-add the standalone pong without solving framing.
- [feedback_pcf_lt_rate_limit_floor](../../../memory/feedback_pcf_lt_rate_limit_floor.md) — the negative-delta wrap mechanism.
- [project_linbpq_v2_1_32_release](../../../memory/project_linbpq_v2_1_32_release.md) — current stable floor.
- [reference_pcflexnet_v4_re](../../../memory/reference_pcflexnet_v4_re.md) — earlier RE notes (cadence figure of "16 s" between PCF KAs turns out to be peer-specific; ~2 s is what the wire shows for a healthy xnet peer).
