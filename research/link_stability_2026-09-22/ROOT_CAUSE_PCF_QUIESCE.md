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

(X)Net is unaffected: it sent **no `3+` at all** across the 20.9 h baseline
and takes our 2483 pushed frames without complaint. ⚠ It is not *incapable*
of sending one — `IW2OHX-14` sent a `3+` (`entries=0`) at session setup on
2026-09-22T07:56:52Z. That is why the gate is scoped with
`flex_peer_is_pcf()` rather than left peer-agnostic: on the transaction
alone, an (X)Net peer could arm it.

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

## What this fix costs, and what it leaves open

Stated plainly, because none of it is free:

1. **Our view at PC/Flexnet refreshes only per `3+`.** In this capture that
   interval alternated between ~90 min and ~10 min bursts, so a destination
   that dies can sit in PC/Flexnet's table for up to ~90 minutes before we
   can withdraw it. That is worse than the event-driven push it replaces —
   but the thing it replaces was ending the session every 30-40 s after each
   `3+`, and a reset flushes everything anyway. It is also what the protocol
   specifies. Nothing goes stale *beyond* the interval: the next answer is a
   `force=TRUE` walk over live `learned[]`, not a replay of a queue.

2. **Does PC/Flexnet age our destinations out between requests?** Not
   answered here, and not answerable from this capture — it would need a `D`
   query on `IW2OHX-12`, which is a chained telnet across the live mesh and
   therefore the exact poller the quiet run exists to remove. Check it once,
   deliberately, after the soak. The reason to expect "no": PC/Flexnet's own
   `3+` cycle *is* its refresh mechanism, and it sends us 162 record frames
   where we sent 5583.

3. **(X)Net pushes too, and apparently survives.** `IW2OHX-14` sent us 2483
   spontaneous record frames in the same 20.9 h, and it also peers with
   `IW2OHX-12`. We cannot see the `-12 ↔ -14` link from here, so whether
   PC/Flexnet treats (X)Net's pushes the same way is **open**. It does not
   affect this fix — the law on our own link is 30/30 — but it is the first
   thing to look at if the quiesce holds and the mechanism still feels
   under-explained. A capture of `-12 ↔ -14` would settle it.

4. **One `3+`-less teardown remains unexplained.** 1 of 32. Too few to
   characterise; watch whether it recurs on the new build.

## Test log

| when (UTC) | event |
|---|---|
| 07:36 | verification captures armed (`/tmp/q222-mon-{12,14}`) |
| 07:35, 07:43, 07:50, 07:56 | IR2UFV restarts — **ours**. The DM/SABM pairs at these times are deploys, not teardowns |
| 07:56 | v2.2.2-rc1 `flexdebug` live and stable; this is the build the soak measures |

⚠ The first three restarts include one where a plain `make` produced a
**non-debug** build: `FLEXNET_DEBUG` defaults to 0, so `FlexNet_Log` returned
immediately and `/tmp/flexnet_axudp.log` silently stopped at the old
instance's last line. The baseline capture was taken with the debug build, so
the soak uses `make flexdebug` to keep them comparable. A dead log looks
exactly like a quiet link.

---

# Soak results

## Cycle 1 — the fix works

| | old build, 20.9 h | v2.2.2-rc1 |
|---|---|---|
| `3+` → teardown | **30 / 30** | **0 / 1** |
| record frames after the closing `3-` | 1 (×10) or 2 (×20), never 0 | **0** |
| teardown rate | 32 in 20.91 h = **1.53/h** | 1 in 1.51 h = 0.66/h |
| session lifetime | median 889 s; only **2 of 32** reached 5445 s | **5445 s** |

The `3+` at 08:45:50Z was answered with 215 records in 14 frames over 61 s,
closed with `3-` at 08:47:01Z, and then **nothing** — the first
`frames after close = 0` in the whole dataset. No teardown.

Worth recording: the 120 s tick immediately *before* that `3+` emitted the
full killer signature and was harmless —

```
08:45:46  Out  3 records x1  IR2UFV08=1
08:45:46  Out  3-  RELEASE
08:45:46  Out  3 records x1  IW2OHX>>=2
```

— which is the 549-with-zero-teardowns finding caught live. A fix aimed at
that signature would have changed nothing.

## The teardown at 09:28:31Z is a different, known mechanism

Not the one fixed here, and the quiesce held correctly: for the two minutes
before it we sent only link-time frames, zero records. The session had run
**90 min 45 s**.

That is PC/Flexnet's **intrinsic AXIP link cycle**, RE'd in 2026-05: it
DISC/SABM-cycles AXIP peers on roughly this period, xnet/RF peers never, and
`flxnod32.dll`'s L2 timeout threshold is not reachable from our side.
`flexnetd` reached the same conclusion and the same posture (`route_advert=0`
for PCF) independently. One event is an identification, not a rate.

## A defect this exposed in the fix itself — fixed

PC/Flexnet's post-SABM keepalive was `1600\r`, its documented session seed,
so it rebuilt its FlexNet state. **Our** session survived the L2 cycle on the
same LINK pointer, so `FlexNet_InitSession` never ran and `pcf_quiesced`
stayed armed — and we sent the peer nothing at all for the rest of the cycle
while `FL` still claimed `Advert 210`. That is worse than the push the gate
replaced.

Fixed by `FlexNet_NotePeerL2Restart()`, called from the one site that
demonstrably fires (`L2Code.c`, SABM with no active LINK). It clears the
gate, clears `sent_routes` to re-arm the existing keepalive-gated re-seed,
and zeroes `advertised[]` so that walk re-sends the whole table rather than
diffing against a view the peer no longer holds.

Safe because the teardown only ever followed a `3+` **answer**; a post-SABM
seed dump never has — in the baseline every session opened with 1601-3259
records and none of those openings drew a teardown.


---

# v2.2.2 final — the AXIP-cycle path verified in the field

The quiesce itself was confirmed over three `3+` transactions. The *recovery*
path — release the gate and re-seed when PC/Flexnet restarts its session —
needed an AXIP cycle to exercise, and it took three attempts to get right.

| attempt | what happened |
|---|---|
| 11:08:32Z cycle | hook never fired. `memcmp(peer_callsign, ORIGIN, 7)` compares the SSID byte whole, and its C/H and end-of-address bits differ between `LINKCALL` and a frame's `ORIGIN`. Fixed with `flex_l2_same_call()`. |
| 12:41:59Z cycle | hook fired, but logged an empty callsign (`flex_sess_peer_call` reads `sess->LINK->LINKCALL`, which BPQ has already cleared), and the re-seed then **stalled at `INIT` with 7 of ~210 records for minutes** — it hung off `CE_FRAME_KEEPALIVE`, which PC/Flexnet sends only ~every 6 min (214 frames in 20.9 h) against a type-1 link-time every 29 s. |
| **14:22:55Z cycle** | **all correct.** Release logs the callsign; the new timer-driven `RESEED:` fires in the *same second*; link goes straight to `CONNECTED`. |

```
16:22:55 L2-SABM-NEW: IW2OHX-12 -> IR2UFV ctl=SABM
16:22:55 PCF-QUIESCE: released — IW2OHX-12 restarted its L2 session
16:22:55 RESEED: IW2OHX-12 established and unseeded — sending our table
```

**`Advert` settles at 105, not 210, and that is correct.** PC/Flexnet re-sends
its own 103 destinations on the new session (`Learned 103`) and split-horizon
excludes those from what we advertise back. Before the cycle it had told us
nothing (`Learned 1`), so we advertised all 210. Do not read the drop as a
truncated re-seed.

## The 5445 s lifetime, four for four

```
07:57:46Z -> 09:28:31Z = 5445 s
09:37:47Z -> 11:08:32Z = 5445 s
11:11:14Z -> 12:41:59Z = 5445 s
12:52:10Z -> 14:22:55Z = 5445 s
```

⚠ **Anchor the prediction on the last `L2-SABM-NEW`, not on when you
deployed.** The fourth was predicted at 14:18:23Z and landed 272 s later,
which briefly looked like the constant breaking — the session had actually
re-established at 12:52:10Z, 4.5 min after the 12:47:38Z deploy.
