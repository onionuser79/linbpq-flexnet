# v2.6.0-rc1 cross-port routing — IR2UFV field test, 2026-10-08

## Set-up

IR2UFV (LinBPQ, `flexdebug`, v2.6.0-rc1) has FlexNet neighbours on two
ports: AXUDP port 2 — IW2OHX-14 ((X)Net) and IW2OHX-12 (PC/Flexnet) — and
KISS port 3 — IW2OHX-13 (LinBPQ v2.5.0, production), over a socat pty
pair. IR2UFV's link to -13 is `F>` (private), -13's to IR2UFV `F>+`.
`FLEXNETCROSSPORT YES` added to IR2UFV.

IR2UFV's route to -13 is the KISS link (`D IW2OHX-13` →
`route: IR2UFV IW2OHX-13`). IR2UFV has no AXUDP `MAP` for -13, so
anything -13 answers can only reach IR2UFV over KISS.

## Test

On (X)Net IW2OHX-14 (telnet, as IW7EAS-1): `C IW2OHX-13 IR2UFV`, then `V`
on -13, then `B`. Driver: `xport_from_14.py` (variant of
`tools/l2_transit_test.py`). Before v2.6 this could not work: the SABM
arrives on port 2 and its destination is a neighbour on port 3.

Run 2 (after the fix below), AXUDP side, `tools/axudp_decode.py`:

```
09:42:55.129  -14 > IR2UFV  IW7EAS-1->IW2OHX-13 [IW2OHX-14* IR2UFV] SABM+
09:42:55.189  IR2UFV > -14  IW2OHX-13->IW7EAS-1 [IR2UFV* IW2OHX-14] UA+
09:42:55.200  IR2UFV > -14  IW2OHX-13->IW7EAS-1 [IR2UFV* IW2OHX-14] I00
09:42:55.201  IR2UFV > -14  IW2OHX-13->IW7EAS-1 [IR2UFV* IW2OHX-14] I10
09:42:55.201  -14 > IR2UFV  IW7EAS-1->IW2OHX-13 [IW2OHX-14* IR2UFV] RR1
09:42:55.201  -14 > IR2UFV  IW7EAS-1->IW2OHX-13 [IW2OHX-14* IR2UFV] RR2
09:43:20.141  -14 > IR2UFV  IW7EAS-1->IW2OHX-13 [IW2OHX-14* IR2UFV] I02
09:43:20.197  IR2UFV > -14  IW2OHX-13->IW7EAS-1 [IR2UFV* IW2OHX-14] RR1
09:43:20.219  IR2UFV > -14  IW2OHX-13->IW7EAS-1 [IR2UFV* IW2OHX-14] I21
09:43:20.219  -14 > IR2UFV  IW7EAS-1->IW2OHX-13 [IW2OHX-14* IR2UFV] RR3
09:43:30.437  -14 > IR2UFV  IW7EAS-1->IW2OHX-13 [IW2OHX-14* IR2UFV] I13
09:43:30.500  IR2UFV > -14  IW2OHX-13->IW7EAS-1 [IR2UFV* IW2OHX-14] RR2
09:43:30.553  IR2UFV > -14  IW2OHX-13->IW7EAS-1 [IR2UFV* IW2OHX-14] DISC+
09:43:30.553  -14 > IR2UFV  IW7EAS-1->IW2OHX-13 [IW2OHX-14* IR2UFV] UA+
```

IR2UFV's decision log, same window: 14 `L2FWD-XPORT` lines, one per frame
above — every `IW7EAS-1->IW2OHX-13` frame `port 2 -> 3 (circuit)`, every
`IW2OHX-13->IW7EAS-1` frame `port 3 -> 2 (returning)`. Forward frames
first log `L2FWD-ADJACENT ... neighbour on port 3`. `FL`: one circuit,
`cross-port ON: frames=13` after run 1. -13 answered `V` through the
circuit; SABM→UA 60 ms.

Run 1 (before the fix) was identical on the wire: 13 frames, 13 decisions.

## Finding: a neighbour offered a route to itself

After run 1, production -13's `D < IR2UFV` held one entry: **`IW2OHX
13-13  2005`** — its own callsign, learned from IR2UFV (2005 = cost + the
`+2000` of -13's `F>+`). IR2UFV had learned `IW2OHX 13-13` from -14 on
port 2 and, with cross-port on, offered it to -13 on port 3. Split
horizon only excludes routes learned *from* the peer, not routes *about*
it learned elsewhere.

Reference behaviour: (X)Net -14's table sent to IR2UFV (`D < IW2OHX-14`
on IR2UFV, 133 destinations) contains no `IR2UFV` record. (X)Net never
offers a node its own callsign.

Fix (in rc1 before tagging): `flex_expected_rtt()` returns infinity when
the destination is the target peer itself (`flex_dest_is_session_peer`).
After redeploying, IR2UFV advertised **185** records to -13 instead of
186, and -13's `D < IR2UFV` is empty. The PC/Flexnet peer IW2OHX-12 also
went 186 → 185: IR2UFV had been offering -12 its own callsign on the
**same** port all along, so the defect predates cross-port.

IR2UFV was switched back to `FLEXNETCROSSPORT NO` between the two runs
while this was fixed.

## Not ours: -13's path cache for IW2OHX-4

-13's `D IW2OHX-4` shows `route: IW2OHX-13 IR2UFV IW2OHX-13`. The entry in
-13's `flexnet_path_cache.dat` is `IW2OHX 4-4 … hops=IR2UFV IW2OHX-13`,
written **10:51:28 local, before IR2UFV ran rc1** (deployed 11:30). A
path answer for another SSID of the same base call appears to have been
stored for IW2OHX-4. Open item, v2.5 on production; the path cache
expires entries after 5 h.
