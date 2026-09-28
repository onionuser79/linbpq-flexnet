# What eleven days of wire say about the rest of the L2-routing milestone

2026-09-28. Answers ROADMAP milestone items 2 (the second ingress shape) and
3 (the egress cross-check), and puts real numbers under item 4 (loop safety),
**from captures that were already on disk** — no new capture was needed.

Source: every AXUDP pcap on iw2ohx-gw from 2026-09-17 08:47Z to 2026-09-28
08:07Z — the `flexnet-quiet-run-2026-09-21/` set (incl. `prod-router-watch/`,
a full day with production IW2OHX-13 as a router carrying `-4`'s transit) and
the per-link captures of the 09-17 … 09-22 investigations. ~200 000 frames
arriving at the two LinBPQ instances. Tools: `tools/ingress_shapes.py`,
`tools/egress_shapes.py` (only frames whose destination IP is gw and whose
source is not — the peers listen on the same UDP port numbers, so the port
alone would count our own output too).

## Item 2 — the second ingress shape does not occur

The shape: a frame whose digi chain is **already consumed** while its DEST is
not one of ours, i.e. a peer expecting us to route on DEST alone.

| Shape of inbound frames | IR2UFV | IW2OHX-13 |
|---|---:|---:|
| A — pending digi is us (L2 transit or delivery via digi) | 3 683 | 90 |
| B — chain consumed, DEST ours | 164 282 | 8 084 |
| C' — chain consumed, DEST not ours, **UI** (NODES, APRS broadcasts) | 15 799 | 7 011 |
| **C — chain consumed, DEST not ours, connected mode** | **12** | **1** |

All 12 on IR2UFV are replies to this morning's test session, addressed to the
user logged into IR2UFV (`IZ3LSV-14->IW7CFD [IW2OHX-14*]`) — a delivery to a
local user, not a routing request. The one on production is a lone DM
(`IW2OHX-12->IW7EAS-1 [IW2OHX-14*]`, 2026-09-21), a stray connect refusal.

**Across eleven days, including a full day of production transit, no peer —
(X)Net or PC/Flexnet — ever handed us a connected-mode frame to route on DEST
alone.** They always name the next hop as a pending digi, which is the shape
`FlexNet_L2Transit()` already handles. Item 2 is **closed, not built**: code
for a shape the network does not send is unverifiable and cannot be right by
anything but luck. If it ever appears, `ingress_shapes.py` will count it.

## Item 3 — the egress shape is what (X)Net emits

Every frame of shape A was *built by the peer that sent it*, so the archive
holds each peer's egress shape. Roles: `S` = the sending peer, `U` = us,
`N1…` = other nodes, `*` = H bit.

| Sender | Chain as sent to us | Frames | What it is |
|---|---|---:|---|
| IW2OHX-14 (X)Net | `[N1* S* U]` e.g. `IW7ER-15->IW2OHX-4 [IW2OHX-3* IW2OHX-14* IR2UFV]` | 73 | **(X)Net transiting mid-chain**: received `[IW2OHX-3* IW2OHX-14]`, set its H bit, appended us |
| IW2OHX-4 (X)Net | `[N1* S* U]` e.g. `IW7FR-15->IQ2LB [IW2OHX-14* IW2OHX-4* IR2UFV]` | 53 | same, second (X)Net node |
| IW2OHX-12 PC/Flexnet | `[N1* S* U]` e.g. `IW7TY-15->DB0OVN [IW2OHX-14* IW2OHX-12* IR2UFV]` | 122 | same, PC/Flexnet — matches `l2_forwarding_2026-09-17/` |
| IW2OHX-14 (X)Net | `[S* U N1]` e.g. `DB0DLG-6->IW7EAS-2 [IW2OHX-14* IR2UFV IW2OHX-4]` | 322 | reverse frame of a circuit **we** extended — the entry we remove |

Two (X)Net nodes and PC/Flexnet, all transiting for third parties, emit
exactly what `flex_l2_append_digi()` emits: own H bit set, next hop appended
unrepeated, E bit moved. And the reverse frames they send back through a
circuit we extended carry our appended hop repeated immediately before us —
exactly what `flex_l2_is_our_hop()` removes. **Item 3 is closed: the egress
shape is cross-checked against real (X)Net transit, not inferred.**

## Item 4 — frame-plane loops are real, and the drop is safe

| Sender | Chain | Frames |
|---|---|---:|
| IW2OHX-4 (X)Net | `[U* S* U]` e.g. `IW7EAS->DB0FHN [IR2UFV* IW2OHX-4* IR2UFV]` | 68 |
| IW2OHX-12 PC/Flexnet | `[U* S* U]` e.g. `IW7EAS->VA3BAL-13 [IR2UFV* IW2OHX-12* IR2UFV]` | 18 |
| IW2OHX-14 (X)Net | `[U* S* U]` | 6 |
| IW2OHX-12 PC/Flexnet | `[S* N1* S* U]` — `-12` itself twice | 1 |

92 forward frames came back to us after one bounce: a peer routed a
destination via us while we routed it via that peer. Followed end to end
(`/tmp/chk-4.pcap` + `chk-14.pcap`, 2026-09-18 13:45Z), the
`IW7EAS->DB0FHN` circuit **never completed**: the SABM bounced off `-4`, the
chain grew until it left via `-14`, DB0FHN answered, but its UAs never reached
the originator — it re-sent the SABM every 1.9 s for 45 s, gave up with a
DISC, and the far side answered DM. **A looped circuit is already dead, so
v2.2.4-rc1's `L2FWD-LOOP` drop costs nothing and stops the ping-pong.** The
count-to-infinity containment (`flex_climb_is_loop()`) addresses the routing
cause; the drop stops its frame-plane symptom.

## A finding that is not on the milestone: (X)Net acknowledges per hop

From this morning's capture of `IW7CFD->IZ3LSV-14 [IW2OHX-14]` (IR2UFV user
→ (X)Net `-14` → `IR3UHU-2` → `IZ3LSV-14`):

| Our I-frame | `RR` "from IZ3LSV-14" | Far end's actual reply |
|---|---|---|
| I04 at 01.735 | 01.736 (**1 ms**) | I41 at 01.783 (48 ms) |
| I15 at 02.735 | 02.735 (**0 ms**) | I52 at 02.776 (41 ms) |
| I26 at 03.733 | 03.734 (**1 ms**) | I63 at 03.775 (42 ms) |

The acknowledgement arrives ~40 ms before the far end could have sent one,
addressed as if from the destination, via `[IW2OHX-14*]`. So captures show
**(X)Net terminates L2 at each hop and acknowledges locally**, while the SABM
is answered only after the onward link is up (UA at 38 ms, banner in the same
millisecond). PC/Flexnet's UA in `l2_forwarding_2026-09-17/` (+0.055 in,
+0.167 out) fits the same model. We digipeat end to end instead.

This is not a defect — end-to-end digipeating is legal AX.25 and the field
test works — but it means a user through us sees the whole path's round trip
in every T1, where a user through (X)Net sees one hop's. It matters most on
long or lossy paths. Recorded as a **candidate** in the ROADMAP; unlike items
2 and 3 it would be a large change (per-hop link state in the transit path),
and it needs its own capture of an I-frame loss to see what (X)Net does with a
retransmission before any of it is designed.
