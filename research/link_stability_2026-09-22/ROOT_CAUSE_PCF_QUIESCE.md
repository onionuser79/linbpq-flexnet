# The `IR2UFV ↔ IW2OHX-12` teardown — root cause, from the quiet capture

**Status: root cause identified, fix built as v2.2.2-rc1, deployed to IR2UFV
2026-09-22 09:37 CEST, under test.**

Open since 2026-09-18 and survived v2.2.0 and v2.2.1. This is the fifth
mechanism examined and the first that accounts for **every** teardown in the
capture rather than correlating with most of them.

## The evidence

One continuous 20.9 h capture, `rtr-mon-12/link.pcap0`, 25 206 packets,
2026-09-21 10:07Z → 2026-09-22 07:02Z. It is the first capture taken with
`station-dashboard`'s 15-minute telnet of all five nodes switched off, so
for the first time the measurement is not dominated by our own polling.

| | |
|---|---|
| inbound `3+` full-table requests | 30 |
| independent teardowns | 32 |
| `3+` followed by a teardown within 120 s | **30 / 30** |
| teardowns with no `3+` before them | 1 / 32 |

### 1. PC/Flexnet reacts synchronously to a frame, it does not time out

Delay from each candidate trigger to the teardown, over the 32 independent
events:

| trigger | median | stdev |
|---|---|---|
| **our last compact-record frame** | **0.00 s** | **1.91 s** |
| our last `3-` | 2.61 s | 18.96 s |
| our last link-time frame | 11.92 s | 5.89 s |
| the inbound `3+` | 39.45 s | 1009 s |

30 of 32 teardowns land within **0.06 s** of one of our record frames. The
L2 exchange is healthy right up to the DISC — every I-frame acked in ~20 ms,
no retries, no REJ. PC/Flexnet acks our frame and hangs up in the same
millisecond.

### 2. It is a hard limit of two frames after the batch closes

For each `3+`, counting our record frames emitted after the `3-` that closed
the answer:

| record frames after the close | transactions |
|---|---|
| 1 | 10 |
| 2 | 20 |
| 0, or 3 or more | **0** |

Thirty out of thirty. PC/Flexnet accepts at most two further record frames
after a completed `3+` exchange and then tears the session down.

### 3. It is not the content, and not the `3-` placement

- `IW2OHX-14 = 2` went out **614 times harmlessly and 14 times fatally**;
  `IR2UFV 0-8 = 1`, 617 vs 11. The killer frames are ordinary: 96 % carry a
  single record, and their cost distribution sits inside the population's.
- The obvious suspect — a record arriving *after* we closed a batch with
  `3-`, which is what rc3's EOB quiet window was written to prevent — is
  **not** the mechanism. Outside a `3+` transaction that exact shape occurs
  **549 times with zero teardowns**:

| | n | teardown within 10 s |
|---|---|---|
| unsolicited `3-` then a record within 5 s | 549 | **0** |
| solicited (after a `3+`), record after `3-` | 56 | 20 |
| solicited, clean close | 23 | 8 |

  Solicited kills at 35 % whether or not a record follows the `3-`; being
  solicited is the whole signal.

### 4. Why: we are the only one pushing

`PROTOCOL_SPEC.md` §2.6 has routes exchanged **inside** a `3+`…`3-`
transaction at cycle boundaries. PC/Flexnet obeys that literally. Over the
same capture:

| | records frames sent |
|---|---|
| IR2UFV → IW2OHX-12 | **5583** |
| IW2OHX-12 → IR2UFV | 162 |

162 ≈ 30 transactions × ~5 frames: PC/Flexnet sends essentially nothing
outside a transaction. The event-driven push introduced in v2.2 rc4 is the
outlier, and PC/Flexnet tolerates it only until it has done a `3+` exchange —
after which it treats an unsolicited record as a protocol error.

(X)Net is unaffected and sent **no `3+` at all** in 20.9 h; it takes our
2483 pushed frames without complaint.

## The fix — `FLEXNETPCFQUIESCE` (default YES)

After answering a PC/Flexnet peer's `3+` and emitting the closing `3-`, send
that peer no further compact records until its next `3+`.

| site | change |
|---|---|
| `struct FLEXNET_SESSION` | `pcf_quiesced`, `quiesced_since` (both in `asmstrucs.h` and the `FLEXNET_DEST_DEFINED` fallback copy) |
| `CE_FRAME_STATUS_PLUS` handler | an inbound `3+` clears the flag — the answer is solicited and must not be gated |
| `flex_advertise_drain()` EOB block | emitting the closing `3-` arms the flag for a PCF peer |
| `flex_advertise_check()` | returns early while armed, so nothing is queued |
| `flex_advertise_drain()` | skips the record loop while armed; an owed `3-` is still delivered |
| `FlexNet_Timer()` §5.5 120 s tick | skips `flex_send_own_routes()` + `flex_advertise_neighbours()` while armed |

Nothing is lost by not queueing: the walk answering the next `3+` runs with
`force=TRUE` and re-sends the whole table from live `learned[]` state.

Scope is the PCF family only (`flex_peer_is_pcf`). (X)Net never sends `3+`,
so it would never arm this — the explicit gate means the known-good
`IW2OHX-14` link cannot change in this release.

The flag is cleared by the `memset` in `FlexNet_InitSession`, so it cannot
survive a reconnect.

## What this predicts, and how to falsify it

If the mechanism is right, on IR2UFV after the cutover:

1. Each `3+` is answered in full, then `PCF-QUIESCE: armed` appears.
2. **No teardown follows.** 30/30 became 0/N.
3. The next `3+` logs `PCF-QUIESCE: released` and is answered in full again.
4. Record frames to `-12` drop from ~5583/21 h to roughly one batch per `3+`.
5. `IW2OHX-14` is unchanged.

Any teardown following a `3+` on the new build falsifies it outright.

## Counting rules that still apply

- Teardowns arrive in **pairs 60 s apart**; the second is PC/Flexnet's
  fresh-session seed. Count each pair as one event.
- A quiet hour proves nothing — confirm a `3+` actually arrived. In this
  capture the `3+` interval alternated between ~90 min and ~10 min bursts.
- PC/Flexnet's `600` is its session seed, not a verdict on what preceded it.

## Reproducing the analysis

`tools/flexnet_transaction_audit.py` is the decisive one — it prints the
per-transaction frame-after-close histogram in section 2.

```bash
python3 tools/flexnet_transaction_audit.py \
    research/link_stability_2026-09-20/quiet-run-2026-09-21/rtr-mon-12/link.pcap0 \
    192.168.1.202 192.168.1.201
```
