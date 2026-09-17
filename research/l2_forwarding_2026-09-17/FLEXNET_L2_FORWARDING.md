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
