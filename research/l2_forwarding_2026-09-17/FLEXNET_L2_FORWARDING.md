# FlexNet L2 forwarding — what a real router actually does

**2026-09-17. Captured on the transit node itself.** This answers the
question `flexnetd/PROTOCOL_SPEC.md` §5.1 left open and, in one respect,
answered wrongly.

## Method

`iw2ohx-gw` is **not** in the path between the (X)Net nodes and
PC/Flexnet IW2OHX-12 — `tcpdump` on gw for either leg captures zero
frames. (X)Net's own `MONITOR` is an *event* monitor, not a frame
monitor: on an AXIP port it prints `broadcas:Tx 254 byte with no driver`
rather than frames. So neither of the obvious vantage points works.

The capture therefore had to be taken **on IW2OHX-12 itself**, which is
also the best possible vantage point: it is the node that forwards for
IW2OHX-4 ↔ IW2OHX-14, so one capture holds its **input and its output**
for the same user frame. On that host the PC/Flexnet process holds
UDP 93, 94 and 95 (one per FlexNet peer link, per `Get-NetUDPEndpoint`),
and `pktmon` — built into Windows, the only capture tool there — filters
on exactly those three ports. Tooling:
`tools/pcf_transit_capture.ps1` and `tools/flexnet_transit_decode.py`.

A user on IW2OHX-4 then connected to **IGATE**, which is reachable only
through IW2OHX-14, i.e. two hops beyond the transit node.

Port 95 is the IW2OHX-4 link and port 93 the IW2OHX-14 link, so "95 →"
is the transit node's input and "93 →" its output.

## The mechanism: symmetric digi-chain rewriting

```
t+0.000  95  IW7EAS-2->IGATE   IW2OHX-4* IW2OHX-12                SABM   <- in
t+0.011  93  IW7EAS-2->IGATE   IW2OHX-4* IW2OHX-12* IW2OHX-14     SABM   -> out
t+0.055  93  IGATE->IW7EAS-2   IW2OHX-14* IW2OHX-12 IW2OHX-4      UA     <- in
t+0.167  95  IGATE->IW7EAS-2   IW2OHX-12* IW2OHX-4                UA     -> out
```

**Forward path.** The frame arrives with the originator's two-digi chain
`IW2OHX-4* IW2OHX-12`. The transit node sets **its own H-bit** and
**appends the next hop** — `IW2OHX-14` — as a new, unrepeated digi. The
chain grows by exactly one entry, the node's own next hop.

**Reverse path.** The reply arrives from IW2OHX-14 with the reversed
three-digi chain `IW2OHX-14* IW2OHX-12 IW2OHX-4`. The transit node
**removes the entry it had added** and sets its own H-bit, emitting
`IW2OHX-12* IW2OHX-4`.

**The originator therefore only ever sees the chain it sent.** Extension
and contraction are symmetric, which is precisely what keeps AX.25 V2's
"the UA's digi list is the reverse of the SABM's" invariant intact.

Confirmed for **every frame type** in the session — SABM, UA, I, RR,
DISC, DM — and across two independent connects. It is the frame's
addressing that is rewritten; there is no encapsulation and no change of
`src`/`dst`.

The last two bytes of the AXUDP payload differ between the two sides
(`'BYE\r+c'` inbound vs `'BYE\rPb'` outbound) — the AXUDP CRC, recomputed
because the header changed. The frame really is rebuilt hop by hop, not
tunnelled.

## PROTOCOL_SPEC §5.1 needs correcting

§5.1 currently states:

> Any node that **adds** its own callsign to the digi chain on the
> forward path breaks this invariant: the UA returns through the extended
> chain and arrives at the originator with one more digi than was sent,
> and AX.25 V2 rejects the UA. This rules out a transit forwarding
> pattern where an intermediate FlexNet node … tries to relay by
> prepending its own neighbour-callsign as an extra L2 digi.

**That conclusion is wrong, and it is the reason this was never
implemented.** Extension only breaks V2 if the node fails to *contract*
on the reverse path. PC/Flexnet extends *and* contracts, so the
originator's view is consistent and V2 is satisfied. linbpq-flexnet's
v1.9.4 attempt did the first half and not the second, hit the predicted
symptom, and the conclusion drawn — that the whole pattern is illegal —
was too strong. The pattern is legal; the implementation was incomplete.

This also explains the one-hop case working today: with the destination
adjacent to us there is no next hop to append, so plain digipeating is
already the correct behaviour and no rewriting is needed.

## Not NetROM L3

In the same capture, PID=CF (NetROM) frames appear **only** on port 93
and never on port 95, and none belong to this user session. The user
session is carried entirely as **rewritten AX.25** — so §4.3's premise
that multi-hop transit arrives as a NetROM L4 CREQ does not describe
(X)Net↔PC/Flexnet traffic at all. CREQ remains plausible for a
**BPQ/linbpq** peer, which genuinely does use NetROM L4; that is a
separate, untested path.

## What implementing this requires

1. **Forward**: on a frame whose pending digi is us and whose
   destination is reachable via another FlexNet neighbour — set our
   H-bit, append the next-hop callsign unrepeated, re-emit on that
   neighbour's link. AX.25 allows 8 digis, so the chain length bounds
   FlexNet path length; each transit node adds exactly one.
2. **Reverse**: recognise the returning frame and remove the digi we
   added. The hop we appended arrives as the **leading, already-repeated**
   entry of the reversed chain.
3. **State**: we cannot tell "the digi I appended" from "a digi the
   originator supplied" by inspection — both look identical on the
   reverse path. That needs per-session state keyed on
   `(src, dst, port)`, recording which next hop we appended. This is the
   per-circuit state the ROADMAP milestone calls for, and it is the only
   genuinely new bookkeeping the mechanism needs.
4. **Safety**: a TTL analogue. AX.25 has no hop counter, so the digi
   count is the only natural bound — refuse to append beyond a
   configured maximum, and never append a call already present in the
   chain (which is also a cheap loop check).

## Reproducing

```bash
# on the transit node (Windows, elevated)
powershell -NoProfile -File C:\temp\pcf_transit_capture.ps1 -Seconds 50 -Tag igate

# meanwhile, from anywhere
#   connect a user on IW2OHX-4 to a destination beyond IW2OHX-14
# then
python3 tools/flexnet_transit_decode.py cap.pcapng --follow IGATE
```

`flexnet_transit_decode.py` reads pcapng as well as pcap, dedupes the
5-8 copies `pktmon` makes of every packet (one per monitored component),
and groups frames by UDP port so the two sides of the node sit side by
side.

---

# Part 2 — Implemented, and what the live node shows

**Same day, later.** The mechanism above was implemented as
`FLEXNETL2TRANSIT` and is running on IR2UFV. This part records the
implementation, what has been verified, and what has *not*.

## Where it hooks

One 6-line hook in `L2Code.c`, immediately before the stock
`Digipeat()`:

```c
ptr = FlexNet_L2Transit(PORT, Buffer, ptr);
if (ptr == NULL) { ReleaseBuffer(Buffer); return; }
Digipeat(PORT, Buffer, ptr, 0, 0);
```

`FlexNet_L2Transit()` returns the (possibly rewritten) digi pointer, or
`NULL` when it has consumed the frame itself. Everything else lives in
`FlexNetCode.c`; the hook site is the only new line in an upstream file,
which keeps the rebase surface unchanged.

The decision is taken only when the pending digi is us. Three outcomes:

| Outcome | Condition | Counter |
|---|---|---|
| **extend** | destination reachable via another FlexNet neighbour | `extended` |
| **contract** | frame matches reverse-path state — remove the digi we added | `contracted` |
| **decline** | anything else: chain full, next hop already in chain, no state, no route | `declined` |

## The state, and why it is unavoidable

Part 2 of the mechanism needs to distinguish "the digi I appended" from
"a digi the originator supplied". On the reverse path they are
indistinguishable by inspection — this is not an implementation
shortcut, it is a property of the wire format. So a transit node
*must* keep state.

`FLEXNET_MAX_L2_TRANSIT 64` entries keyed on `(src, dst, port)`,
recording the appended next hop, idle-aged at
`FLEXNET_L2_TRANSIT_IDLE 900`. 64 concurrent transit circuits is well
beyond what this node will ever carry; the table is fixed-size on
purpose so a flood cannot grow it.

Safety bounds, both from the Part 1 analysis:

* `FLEXNET_L2_MAX_DIGIS 8` — AX.25's own limit is the only available
  TTL analogue, and each transit node spends exactly one entry.
* never append a call already in the chain — a cheap loop check that
  costs one pass over at most 8 addresses.
* `FLEXNET_L2_MAX_FRAME 330` — refuse to rewrite a frame that could not
  survive the extra address.

## Every decline logs

The first test produced no `L2FWD` line at all, which is
indistinguishable from the hook never having run — the same silent
failure this project has repeatedly been bitten by. Every path through
`FlexNet_L2Transit()` now logs its decision and its reason. A silent
`FlexNet_L2Transit()` means the hook did not fire; it never means
"declined".

## Verified

**End to end, deliberately:** `IW2OHX-14 → IR2UFV → IW2OHX-4 →
IQ2LB-6`, with the append on the forward path and the contraction on the
reverse both observed on the wire — the Part 1 mechanism reproduced by
our own code.

**End to end, unprompted** — which is the stronger evidence. With all
instrumentation reverted and no test traffic being generated, the
counters kept climbing on their own:

```
L2 forwarding ON: extended=37 contracted=72 declined=17
```

driven by real third-party traffic:

```
L2FWD IW7TY-15->IW2OHX-13 via IW2OHX-4 (appended, digis now 3)
```

That also retires an earlier mystery: the `peer=IW7TY` links seen while
chasing the IR2UFX teardown were never phantoms. IW7TY-15 is a real
station whose traffic transits this node.

## Not verified — the append/contract asymmetry

`contracted` (72) runs at roughly twice `extended` (37). A transit node
should extend and contract in step, so this wants an explanation.

**RESOLVED 2026-09-18.** The per-window deltas settle it: every clean
sampling window shows extend and contract moving in lockstep —
`extended+5 contracted+5`, `extended+4 contracted+4`, `extended+0
contracted+0` — and live readings the same day gave 41/38 and then 9/9.

The windows that looked skewed are the ones containing a process
restart, which zeroes both counters: `extended+-48 contracted+-45` and
`extended+-9 contracted+-9` are negative deltas, i.e. the counter went
backwards. So the cumulative 37/72 was never a 2:1 imbalance — it was a
snapshot spanning a restart, in which contractions of chains appended by
the *previous* process lifetime had no matching append in the current
one. That is what the hypothesis guessed, and it is now measured rather
than assumed.

Practical rule: **compare deltas, never cumulative counters**, and treat
any negative delta as a restart marker rather than data.

## Unrelated, still open — the linbpq↔linbpq L2 *link*

A second linbpq instance (IR2UFX) peered only with IR2UFV tears its link
down repeatedly at `L2Code.c:4028` ("Too many repeats of same I frame"),
`WS=5 NS=6 OWS=5 retry=8 N2=5`. Ruled out: L4 session teardown, locked
`ROUTES`, `IDLETIME`, `NODESINTERVAL`, the BBS `APPLICATION`, the
NET/ROM relationship, LT misparse, and RR(F) rejection
(`L2WIN-BADNR = 0`). The retried burst contains two 259-byte frames
(~241 B info — the FlexNet keepalive) on a `PACLEN=236` port, which is
the most promising lead.

This is the L2 **link** between two linbpq instances, *not* the L2
**forwarding** plane. The distinction matters: the live xnet and PCF
peers have stayed CONNECTED throughout, and forwarding works across
them. See `IR2UFX_LINK_TEARDOWN.md`.

## Monitoring left running

* `tools/quad-watch.py` — samples IR2UFV, IW2OHX-14, IW2OHX-4 and
  (hourly, chained through -14) IW2OHX-12 within seconds of each other
  and cross-checks their tables. A distance-vector inconsistency is a
  *disagreement between two tables* and is invisible from either end
  alone, which is why the single-sided `pcf-watch.sh` could not find
  one. Alerts A1/A3/A4/A6/A7/A8/A9/A10/A11/A12 in
  `/tmp/quad-watch/alerts.log`.
* rotating `tcpdump` on gw, `udp port 10075 and not udp port 10093`
  (the exclusion keeps the production -13 instance out), 8 x 20 MB ring
  in `/tmp/ir2ufv-wire/` — so any alert can be traced to frames.
