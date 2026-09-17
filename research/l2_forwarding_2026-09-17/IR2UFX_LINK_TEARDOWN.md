# Why a linbpq↔linbpq FlexNet link dies every ~11 s

**2026-09-17. Mechanism confirmed with an instrumented build, and it
rules out the fix that was about to be written.**

## The question

A second linbpq-flexnet instance (`IR2UFX`, `/home/bpq-test`, telnet
2727, AXIP 10077) peered **only** with IR2UFV would make transit tests
deterministic: anything reaching it must have crossed IR2UFV. But its
L2 link tore down ~11 s after every establishment — 35 DISCs, all sent
by IR2UFX — and BPQ logged no reason.

Ruled out first, by trying them: locked `ROUTES:` entries on both sides
(verified installed), `IDLETIME=0`, `NODESINTERVAL=1`, and removing the
BBS `APPLICATION`. NET/ROM was demonstrably fine — IR2UFX learned **34
nodes** through the link, `UFVBPQ:IR2UFV` among them — so this was never
a routing-relationship problem.

## Method

`IDLETIME` turned out to be an L4/BBS setting, not L2, so the guess had
to be replaced with evidence. Every site that sets `L2STATE = 4` was
instrumented — six in `L2Code.c`, three in `L4Code.c` — each printing
its own `file:line`, the peer callsign and `LINK->FlexNetLink`.

**Instrumentation went into the build tree only, never the overlay**, so
no probe could reach the public repo; `tools/`-side script reverts from
its own backups. One trap worth recording: `start-ir2ufx.sh` started
IR2UFX but never *copied* the binary, so the first instrumented run
produced no output at all and looked like a failed build.

## Result: `L2Code.c:4028`, every time

```
L2TEARDOWN-PROBE L2Code.c:4028 peer=IR2UFV flexnet=1 state=4
```

Four firings, one site, nothing else. That site is:

```c
//  Need to kill link if we are getting repeated RR(F) after timeout
//  (Indicating other station is seeing our RR(P) but not the resent I frame)
if (LINK->IFrameRetryCounter++ > LINK->LINKPORT->PORTN2)
{
    Debugprintf("Too many repeats of same I frame - closing connection");
    LINK->L2TIMER = 1;
    LINK->L2STATE = 4;          // DISCONNECTING
    return;
}
```

The wire confirms it — a byte-identical burst, retransmitted:

```
15:56:03  IR2UFX->IR2UFV  I(0x40) I(0x42) I(0x44) I(0x46) I(0x58) I(0x5A)   len 21,257,28,19,257,19
15:56:14  IR2UFX->IR2UFV  I(0x40) I(0x42) I(0x44) I(0x46) I(0x58) I(0x5A)   len 21,257,28,19,257,19
          IR2UFV->IR2UFX  RR(0xD1)   N(R)=6, F set
15:56:15  IR2UFX->IR2UFV  DISC
```

Same N(S) values, same lengths, 11 s apart. IR2UFX's transmit window
never advances, so it retries until `PORTN2` is exhausted and closes the
link itself.

## The fix under consideration would have been wrong

The plan was a `LINK->FlexNetLink` guard so BPQ would not tear down a
link FlexNet owns — which would have meant adding `L4Code.c` as a
**sixth overlay file**, widening the rebase surface `CLAUDE.md` pins at
five.

`flexnet=1` shows the link *was* marked FlexNet-owned, so the guard
would indeed have suppressed the teardown. It would also have left a
**permanently wedged link**: the window would still be stuck, no data
would move, and the one mechanism that currently recovers the situation
would be gone. `L2Code.c:4028` is not BPQ discarding a purposeless
link — it is BPQ correctly detecting a stalled sequence window. The bug
is upstream of it.

## Where the bug most likely is

The prime suspect is the linbpq↔linbpq defect found earlier the same
day: **linbpq cannot parse its own link-time frame.**
`flex_send_link_time` emits `"15\r"` (3 bytes, `FLEXNET_WIRE_LT = 5`),
but `flex_parse_ce_frame` classifies a 3-byte `"1n\r"` as
`CE_FRAME_STATUS_1N` and only reaches `CE_FRAME_LINK_TIME` for
`len > 3`. So neither side ever answers the other's LT, and
`lt_tx_pending` stays set forever.

Supporting it: the last CE frame before **every** DISC was that `1n`
status, and the condition cannot arise with any production peer, since
(X)Net and PC/Flexnet both send multi-digit LT values — which is exactly
why a link that has run for months to those peers falls over in 11 s to
another linbpq.

Not yet proven to be the cause, and it should not be assumed: the next
step is to confirm which frame in the retransmitted burst is not being
consumed, then decide whether the parser or the emitter is wrong. That
choice needs a capture of how (X)Net answers a 3-byte LT, because
changing the parser would alter behaviour toward live peers that are
currently stable.

## Meanwhile

IR2UFX is stopped and its `MAP`/`ROUTES` removed from IR2UFV, so nothing
flaps into the mesh. Its config survives at `/home/bpq-test/bpq32.cfg`.

Note the flapping caused far less mesh churn than the first attempt:
the poison hold-down added earlier held IR2UFX withdrawn for its 90 s
instead of letting the echo undo the withdrawal — the fix working on a
problem it was not written for.

**The transit objective does not depend on this rig.**
`C IQ2LB-6 IR2UFV` on IW2OHX-14 already transits IR2UFV with the
append/contract visible on the wire, and IQ2LB-6 is reachable only via
IR2UFV → IW2OHX-4. IR2UFX would make that repeatable, not newly
possible.
