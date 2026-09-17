# How (X)Net answers a 3-byte link-time frame

**2026-09-17, captured on IR2UFV's AXIP port.** Settles the question left
open by the linbpq↔linbpq teardown investigation.

## The exchange

We send link time as `"1" + value + "\r"` with `FLEXNET_WIRE_LT = 5`,
i.e. **`"15\r"` — three bytes**. What each peer sends back (5B/7B/8B
below include the 2-byte AXUDP CRC):

```
15.147  IR2UFV->IW2OHX-4    CE  5B '15\r..'      <- our LT
15.154  IW2OHX-4->IR2UFV    CE  7B '1600\r..'    <- answer, 7 ms
18.901  IR2UFV->IW2OHX-14   CE  5B '15\r..'      <- our LT
18.902  IW2OHX-14->IR2UFV   CE  5B '10\r..'      <- answer, 1 ms
        IW2OHX-12->IR2UFV   CE  8B '12348\r..'   <- PCF, its cost to us
```

**(X)Net answers a 3-byte LT, within 1-7 ms.** So our emitter is right:
three bytes is valid wire. The reply's length simply follows the
*value* — `10` for a link time of 0, `1600` for 600 ms, `12348` for
PC/Flexnet's 2348.

## The bug: single-digit link times are stolen by the status branches

`flex_parse_ce_frame` reaches `CE_FRAME_LINK_TIME` only for
`data[0] == '1' && len > 3`. The 3-byte forms are matched earlier:

```c
if (len == 3)
{
    if (data[0]=='1' && data[1]=='0' && data[2]=='\r')  return CE_FRAME_STATUS_10;
    if (data[0]=='1' && data[1]>='1' && data[1]<='9' && data[2]=='\r')
                                                        return CE_FRAME_STATUS_1N;
}
...
if (data[0] == '1' && len > 3)                          return CE_FRAME_LINK_TIME;
```

So **exactly the smallest link times — the ones a fast link reports —
are classified as a status frame and discarded.** Measured consequence:

| Peer | reply | parsed as | `lt_sample` folded |
|---|---|---|---|
| IW2OHX-4  | `1600\r`  | LINK_TIME    | **2** |
| IW2OHX-14 | `10\r`    | **STATUS_10** | **0** |
| IW2OHX-12 | `12348\r` | LINK_TIME    | **2** |

`lt_sample peer=IW2OHX-14` has **never** fired. We have never measured
our link time to that peer.

### Why that matters beyond tidiness

`our_link_time` is the second term in every transit cost we advertise
(`expected = learned_rtt + our_link_time`). With no sample ever folded,
it stays at the value `FlexNet_InitSession` seeds — `2` — so **every
cost we advertise through such a peer rests on a default rather than a
measurement.** `FL`'s `LT` column does not reveal this: it shows
`peer_link_time`, what the peer tells *us*.

It also explains why the `STATUS_1N` branch looked harmless. Its comment
records "Observed shapes so far: `12\r` from xnet peers. Semantic not
yet documented; treated as benign status notification." Those frames
were never a separate status family — they are link times of 2.

## Relevance to the linbpq↔linbpq teardown

Between two linbpq nodes both sides emit `"15\r"` and **both** misparse
it as `STATUS_1N`, so neither ever answers. Against (X)Net the exchange
completes in 1 ms; against another linbpq nothing comes back at all.
That is a concrete divergence from the reference implementation on the
exact link that dies after ~11 s — still **suspected, not proven**, as
the cause of the stalled transmit window at `L2Code.c:4028`.

## Recommended fix, and the care it needs

Accept a 3-byte `"1n\r"` as `CE_FRAME_LINK_TIME` with value `n`, and
keep the transmit floor at 1 so we never emit `"10\r"` ourselves —
`flex_link_time_sample` already clamps to `[1, 4095]` for that reason.

**This is a behaviour change toward live, currently-stable peers**, and
not a small one: the `LINK_TIME` path replies with our own LT, so we
would begin answering frames we now ignore. The rate limiters bound it
(`FLEXNET_LT_INTERVAL_XNET` 20 s, `..._PCF` 320 s), and it makes us
behave as (X)Net demonstrably does — but PC/Flexnet's cost ring is
sensitive to exactly this kind of cadence change, and
`project_pcf_cost_ring_inter_event` records that its cost is driven by
inter-event timing. It should go in with a PCF cost capture either side,
not as a drive-by.
