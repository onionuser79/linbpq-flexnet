# CE type-6 is a hop-accumulating traversal, not a question

**2026-09-18, captured on PC/Flexnet IW2OHX-12.** This settles how
FlexNet path queries actually work, and it is not what our responder was
built around.

## Method

`D <dest>` typed on IW2OHX-4 while `pktmon` captured flexkrnl's AXIP
ports on the IW2OHX-12 host (93/94/95, plus 10075 for our own link).
Two destinations, chosen because one renders a route line on -4 and the
other does not:

* `IQ2LB-6` — renders: `*** route: IW2OHX-4 IW2OHX-12 IQ2LB-6`
* `IR3UGM-0` — renders nothing, despite `T=7`

Tooling: `tools/pcf_transit_capture.ps1`, `tools/flexnet_transit_decode.py`.
Capture kept on disk beside this file but deliberately NOT committed:
`research/**/*.pcap` and `*.pcapng` are gitignored, because this is a
public repo and a binary capture is exactly the kind of file that gets
swept in by accident.

## The frames

```
'6' 0x21 "    0" "IW2OHX-4 IW2OHX-12 IQ2LB-6"           -4 -> -12  request
'7' 0x22 "    0" "IW2OHX-4 IW2OHX-12 IQ2LB-6"           -12 -> -4  reply

'6' 0x21 "    0" "IW2OHX-4 IW2OHX-12 IR3UGM"            -4 -> -12  request
'6' 0x22 "    0" "IW2OHX-4 IW2OHX-12 IW2OHX-14 IR3UGM"  -12 -> ??  FORWARDED
```

## What that means

**The request already carries the path.** A type-6 is not "tell me how to
reach X" — it is a chain under construction: `[asker, ...hops so far...,
target]`. The asker seeds it with itself, its chosen next hop, and the
target.

**A node that can finish it replies.** IQ2LB-6 is adjacent to -12, so the
chain was already complete and -12 returned it as type-7.

**A node that cannot finish it forwards.** For IR3UGM, -12 did **not**
reply. It inserted its own next hop toward the target — `IW2OHX-14` —
**before the target**, bumped the leading byte, and sent the type-6 on.
The node finally adjacent to the target answers, and the type-7 travels
back down the accumulated chain.

**The byte after the type is a hop counter.** `0x21 -> 0x22` on both the
reply and the forwarded request, i.e. incremented once per node that
handled the frame. `0x21` is the first hop, so the encoding is
`0x20 + hops`.

**The 5-char field accumulates on the way back.** A fresh request carries
`"    0"`. Replies arriving back at an originator carry a value:
`"   51"`, `"   41"`, `"   45"` in frames replying to our own probes.
Consistent with total path cost/time filled in as the reply returns.

## Why IR3UGM renders nothing on -4

-12 forwarded the request toward -14 and the traversal never came back.
So the missing route line is **not** -4 failing to accept an answer, and
not our bug — for that destination -4 never asks us at all. A traversal
that dies somewhere upstream simply produces silence.

## What this means for linbpq-flexnet

Our responder answers a type-6 **from our own path cache** and never
forwards. That is a shortcut, not a bug, and as of v2.2.0-rc5 it
demonstrably works — with replies anchored on the asker, -4 renders full
chains through us:

```
*** route: IW2OHX-4 IR2UFV IW2OHX-14 HB9ON-15 VE3MCH-8 VA3BAL-8 VA3BAL-1
*** route: IW2OHX-4 IR2UFV IW2OHX-14 HB9ON-15 HB9ON-2
```

The gap is the case where our cache is cold or the chain is too long to
answer. There we stay silent; a real router **forwards the traversal**.
Now fully specified by the capture:

1. take the inbound chain `[asker, ..., target]`
2. insert our next hop toward the target immediately **before** the
   target
3. increment the hop-counter byte
4. send the type-6 to that next hop

Note this also sidesteps the 8-digi answer limit for *discovery*: the
traversal accumulates the chain hop by hop, so no single node has to
express a path it cannot carry. Our `PATH-REQ-TOOLONG` guard stays
correct for *answering*, but forwarding is what a long path needs.

Worth noting what forwarding costs: each traversal is a frame per hop,
and the hop counter is the only loop bound visible on the wire. A relay
implementation needs that counter respected and a cap of its own.
