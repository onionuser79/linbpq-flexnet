# Link stability investigation — 2026-09-18/19

**Outcome: two defects found and fixed, both ours, shipped in v2.2.0.
A third cause is PC/Flexnet's own teardown timer and is not ours.**

This is the readable summary. The measurement detail is in
[`PACKED_ADVERTISEMENTS.md`](PACKED_ADVERTISEMENTS.md); the earlier
teardown-direction census is in
[`../link_stability_2026-09-18/TEARDOWN_DIRECTION.md`](../link_stability_2026-09-18/TEARDOWN_DIRECTION.md).

## The problem

IR2UFV's three FlexNet links kept resetting. The destination table was
therefore untrustworthy: destinations came and went, and a connect
worked or didn't depending on when you tried it.

## How it was settled

A 22 h AXUDP capture of all three links plus a 17 h `linkstab` run, then
two fixes measured one at a time against that baseline.

The single most useful decision was to take **direction from the IP
header, not the callsigns**, and to count only non-digipeated frames.
With transit frames included, `-14` read as 69 outbound SABMs; excluding
them, 2.

## What was wrong

### 1. We hung up on a peer that was only slow — config, fixed 2026-09-18

`FRACK=3000 × RETRIES=5` gave **15 s of patience** against a peer that
stalled and returned **59 s** later on the same N(S) — it had never lost
the session. `RETRIES` 5 → 25 (75 s).

Verified on the wire: our teardowns to `-4` went **38 / 4.9 h → 1 / 17.2 h**.

### 2. One route record per AX.25 I-frame — code, fixed in v2.2.0

The compact CE format is one `'3'` per **frame**, then N records. Every
peer in the mesh does that. We sent one record *per frame*: **15 bytes
of a 236-byte PACLEN**, measured at exactly **1.00 records/frame on all
three links across 22 h**, while (X)Net filled to 248 B and PC/Flexnet
to 205 B.

Because a token bought a *record*, the drain to PC/Flexnet was 12
records/min against a queue fed at 26.8/min — oversubscribed 2.2×, so it
**never emptied** (median depth 72, non-empty **80 % of the run**), and a
~210-destination re-dump took **17.6 minutes**.

A token now buys a **frame**, filled to a 200-byte budget. The I-frame
rate is unchanged, which is the safety argument: the rc1 flood that
broke PC/Flexnet was ~50 I-frames in under 2 s — a frames/sec failure,
not bytes/frame.

> The rule was already written down in `ROADMAP.md` item 5 — but it had
> only ever been applied to the *analysis tool*. The same note describes
> "our own single-record emissions" as an aside. It sat there for four
> months as an observation instead of a bug.

### 3. Unsolicited re-INIT on a healthy link — code, fixed in v2.2.0

`linkstab` reported `SESSION_RESTART IW2OHX-14: 40:13 → 00:37` while the
capture showed **no L2 event at all** — no DISC, no DM, no SABM. What it
showed instead was two outbound INITs on a live link.

`FlexNet_Timer`'s proactive init scan re-handshakes any link with
`L2STATE==5` and `FlexNetLink` cleared, and BPQ clears that flag during
internal maintenance without putting anything on the wire. v2.1.15
documented exactly this and added an established-guard — to the
**same-LINK** path only. The same-callsign/new-LINK path never got it,
and reset the session and sent INIT.

That matters because **a re-INIT reseeds the peer's link-cost ring with
a `600` outlier**, which is precisely what made us look expensive.

A genuine reconnect cannot reach that path: link loss runs
`FlexNet_CloseSession` first, so it lands on the allocate-a-slot path
and INITs correctly. Reaching it with an established session means only
the LINKTABLE pointer moved.

### 4. PC/Flexnet's own 60 s teardown tick — NOT ours

Every `-12` DISC lands an **exact multiple of 60 s** after our INIT
(120, 180, 2040, 5160, 7044, 12000 …), in a rigid cycle:
`DISC` → `SABM` same second → 60 s → `DISC` → 180 s → `SABM`. PC/Flexnet
evaluates something once a minute and cycles the link. No timer of ours
changes it.

## Results

| | before | v2.2.0 |
|---|---|---|
| records per frame | **1.00** | 3.4–5.2 steady, **14.3** at cold start |
| queue to PCF, non-empty | **80 %** | **6 %** |
| queue to PCF, median / max | 72 / 199 | **0** / 56 |
| `QUEUE_DEEP` events | 405 | **0** |
| `PCF_OVERRUN` events | 307 | **0** |
| unsolicited re-INITs | 3–4 per link per 0.8 h | **0** |
| seed after link-up | 183 queued at 100 s, 111 at 11 min | **0 queued, 202 advertised at 100 s** |

**PC/Flexnet's cost for us**, read from its `L *` — the far-end view of
no longer reseeding its ring:

```
2026-09-18:   883/5   600 600 4095 1 1 1
v2.2.0 +9m:  1565/5   600 4095 1
v2.2.0 +35m:  588/5   600 4095 1 1 1 1 1 1
v2.2.0 +64m:  336/5   600 4095 1 1 1 1 1 1 1 1 1 1 1 1
```

`1` is the healthy sample — what IW2OHX-14 shows (`1/1`, sixteen 1s, up
5 days). Every ring entry we have contributed since the restart is a `1`.

## What is NOT fixed

**IW2OHX-12 still cycles at roughly the old rate** — 2 peer teardowns in
1.77 h (1.13/h) against a 1.22/h baseline. The volume and re-INIT
defects were real and are gone, but they were not what makes PC/Flexnet
hang up. Cause 4 above is still there, and it is not ours to change.

**IW2OHX-4 got worse**, 5.7/h against 1.51/h — and it is not the
packing:

* none of its `DM`s follows a large frame; they follow silence and our
  RR polls, or 14–24 byte frames;
* the largest frame we send it is 198 B, and it sends *us* up to 247 B;
* **it is flapping against PC/Flexnet too**, on a link where nothing
  changed (PCF's `L *`: `IW2OHX 4-4 … 5m, 6s`, ring restarting);
* its own advertisements to us collapsed to 9 frames an hour, max 25 B.

`-4` is the RAM-only TNC4e that was physically reset this week. Its
churn is its own; what changed is that it no longer costs us the routing
table or a 17-minute re-dump. **Under separate investigation.**

One outbound teardown to `-4` in the v2.2.0 window (0.94/h vs 0.06/h
over the previous 17 h post-`RETRIES`). n=1 on a visibly unwell peer —
no conclusion drawn, but worth re-checking on a longer soak.

## Method traps worth keeping

* **Pin `axudp_teardown.py --local-ip`.** It defaults to the most
  frequent *source*, so where the peer out-talks us it adopts the peer's
  address and reports **every direction backwards**.
* **Count only non-digipeated frames** (`--link-only`).
* **Never trust a before/after taken across a link reset.** Check the
  process pid and link uptimes first; read a negative counter delta as a
  restart marker, not data.
* `pkill -f /home/bpq-ufv/linbpq` typed inline over ssh matches the ssh
  command line itself. Put it in a script file. IR2UFV runs as **root** —
  stop and start both need `sudo`.
* `pgrep -f linkstab` inline self-matches too; use `ps -C python3 | grep`.
* When a wire-format bug turns up in a tool, **check the emitter and the
  parser for the same assumption** before closing it.
