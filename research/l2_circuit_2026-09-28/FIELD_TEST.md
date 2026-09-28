# L2 transit circuit table — field test of v2.2.4-rc1

2026-09-28, IR2UFV (test instance, `flexdebug`). Production IW2OHX-13 was
not changed (v2.2.3 throughout). Tested build: v2.2.4-rc1 (`fec14d2`), which
pins the next hop per circuit, ties a slot's life to the AX.25 teardown and
drops frames that loop back. Design and unit tests: `FlexNetCode.c`
`FlexNet_L2Transit()`, `tools/unit/test_l2_circuit.c`.

## Why a topology change was needed

IR2UFV's normal FlexNet peers are only `IW2OHX-14` ((X)Net) and `IW2OHX-12`
(PC/Flexnet). Every session we can drive enters the network at `-14`, and
captures show both (X)Net and PC/Flexnet keep the entry node in the digi
chain — even a connect issued on `-12`, reached via `-14`, arrives as:

```
IW7EAS-1->IW2OHX-13 [IW2OHX-14* IW2OHX-12* IR2UFV] SABM+        declined_via_14.txt
```

IR2UFV's route to `-13` is via `-14`, already in the chain, so the loop guard
declines and the stock digipeat runs (`L2FWD-DECLINE ... next hop already in
chain`, 14/14). That is correct behaviour — appending `-14` would have sent
the frame back where it came from — but it exercises only the decline path.

For a loop-free transit, `IW2OHX-4` was re-peered with IR2UFV for the test
window only (port 4 AXUDP on `-4`, the `MAP` + locked route on IR2UFV), then
torn down again. `-4`'s own link to `-12` was not touched.

## Result — transit_via_4.txt

`C IW2OHX-13 IR2UFV` issued on `IW2OHX-4`: IR2UFV sees `[IW2OHX-4* IR2UFV]`,
its route to `-13` is via `-14`, which is not in the chain.

| Direction | In at IR2UFV | Out of IR2UFV |
|---|---|---|
| forward (SABM, I, RR, UA) | `[IW2OHX-4* IR2UFV]` | `[IW2OHX-4* IR2UFV* IW2OHX-14]` — hop **appended** |
| reverse (UA, I, RR, DISC) | `[IW2OHX-14* IR2UFV IW2OHX-4]` | `[IR2UFV* IW2OHX-4]` — hop **removed** |

The originator only ever sees the chain it sent, reversed — AX.25 V2's
invariant holds end to end. The session reached `-13`'s prompt, a command
round-tripped, and `-13` closed it (DISC from the far side, UA from ours).

| Check | Seen |
|---|---|
| every forward frame used the pinned hop | `L2FWD ... via IW2OHX-14 (pinned hop)` on every frame after the SABM |
| every reverse frame contracted | 7/7, `declined=0` |
| teardown closes the circuit | `circuits=1` after the UA, **`circuits=0` ~120 s later** (the LINGER) |
| new SABM after a completed teardown re-resolves | `L2FWD-REPIN: ... (new connection after teardown)` on the second connect; `repinned=0` because it resolved to the same hop, as it should |
| counters after two sessions | `extended=12 contracted=14 declined=0 circuits=1 repinned=0 looped=0 evicted=0` |

## Not covered by this test

- **A route change mid-circuit on the wire.** That is the defect the pin
  exists for, and nothing on the live network could be made to re-route
  `-13` inside a 30 s session. It is covered by the unit test
  `test_route_change_mid_circuit` on real frame bytes, not by a capture.
- **A long-idle open circuit** (> 900 s) and **table-full eviction** — unit
  tests only.
- **The loop drop** (`L2FWD-LOOP`) — never triggered; with split-horizon in
  front of it, a loop needs another router that does not check its own chain.

Reproduce: `tools/l2_transit_test.py <pw>` from iw2ohx-gw with a capture on
`udp port 10075`; decode with `tools/axudp_decode.py <pcap> IW2OHX-13`.
