# v2.3.0-rc1 local APPLICATION calls — field test on IR2UFV

2026-09-28, IR2UFV (test instance, AXIP port 10075), build `v2.3.0-rc1`
flexdebug. Peers: (X)Net `IW2OHX-14`, PC/Flexnet `IW2OHX-12`. Feature from
[issue #1](https://github.com/onionuser79/linbpq-flexnet/issues/1).

Config added for the test:

```
APPLICATION 2,UFVT,INFO,IR2UFX,UFVTST,0
FLEXNETLOCAL IR2UFX
```

`IR2UFX` does not share IR2UFV's base call, so `FLEXNETSSIDRANGE` could not
have advertised it. Qual 0 keeps it out of NET/ROM NODES.

## Result

| Check | Outcome |
|---|---|
| Own-record frame | `3IR2UFV081 IR2UFX001 \r` — ONE `'3'`, two records, one CR |
| (X)Net `-14` table | `IR2UFX 0-0  1` |
| PC/Flexnet `-12` table | `IR2UFX 0-0  1` |
| Type-7 answer (`D IR2UFX` on `-14`) | `route: IW2OHX-14 IR2UFV IR2UFX`; our log: `PATH-REP-TX … target=IR2UFX hops=3 [IW2OHX-14 IR2UFV IR2UFX]` |
| `C IR2UFX` from `-14` (1 hop) | `link setup (14)... *** connected to IR2UFX`, application prompt `UFVBPQ:IR2UFV}` |
| `C IR2UFX` from `-12` (2 hops, `-12 → -14 → IR2UFV`) | `*** connected to IR2UFX`, same application |
| `FL` | `IR2UFX  advertised`, `echo-skips=0` |

## The L2 shape — captured, as designed

From `ir2ufx_frames.txt` (decoded from the AXIP capture, UTC):

```
IW7EAS-1->IR2UFX [IW2OHX-14* IR2UFV] SABM+        in  from -14
IR2UFX->IW7EAS-1 [IR2UFV* IW2OHX-14] UA+          out
IR2UFX->IW7EAS-1 [IR2UFV* IW2OHX-14] I00..I50     out
IW7EAS-1->IR2UFX [IW2OHX-14* IR2UFV] RR5 / I06    in
IR2UFX->IW7EAS-1 [IR2UFV* IW2OHX-14] DISC+ … UA+

IW7EAS-1->IR2UFX [IW2OHX-14* IW2OHX-12* IR2UFV] SABM+   in  from -12
IR2UFX->IW7EAS-1 [IR2UFV* IW2OHX-12 IW2OHX-14] UA+      out
```

Both peers act on our type-7 chain literally: the SABM names us as the
**last unrepeated digi**, with the local call as DEST. Stock L2Code would
digipeat that straight back out. The new hook (`FlexNet_IsLocalCall()`)
marks our H-bit and delivers it to the `APPLICATION` match instead. Every
frame we send back carries **`IR2UFV*`** — H-bit set — as the first entry
of the reversed chain. That is the wire image a real digipeater produces,
and it is what the outbound connect has done since v2.1.8
(`via MYCALL*`). Without `FlexNet_MarkLocalDigi()` BPQ would have sent
`via IR2UFV` unrepeated (its address reversal clears every H-bit), which
the peer reads as a frame still owed to us.

User I-frames arriving after the SABM (`[IW2OHX-14* IR2UFV] I06`) go
through the existing v1.2 path — an active LINK exists for the
(ORIGIN, DEST) pair, so our digi entry is marked and the frame delivered.
No change was needed there.

## A mistake worth keeping: the first test call was in use

The first attempt used `IW2OHX-9`. `-14`'s FlexNet table had no `IW2OHX 9-9`,
and the advertisement worked (`IW2OHX 9-9  1`, `route: IW2OHX-14 IR2UFV
IW2OHX-9`). But `C IW2OHX-9` from `-14` answered with `*** Connected to CONV`
and no frame for `IW2OHX-9` reached IR2UFV. **`IW2OHX-9` is `CNVMI`, the BPQ
Chat on `IW2OHX-15`, reached over NET/ROM** — `-14` preferred its NET/ROM
interlink. The node was advertising a call it does not own. It was withdrawn
within four minutes (cfg restored, restart).

- **Check NET/ROM `NODES` as well as FlexNet `D` before choosing a local
  call.** An empty FlexNet row is not a free callsign. This is now in the
  README.
- **Withdrawal is not instant at PC/Flexnet.** After the restart `-14`
  dropped `IW2OHX 9-9` at once (link reset). `-12`, which had learned it
  directly from us, still listed it at cost 1. It was gone by the next
  check, under five minutes later. Nothing in our table can withdraw a call
  the node no longer knows about, so retiring a local call relies on the
  peer ageing it out.
- The `INIT` byte (`max_ssid`, declared as 8 from `FLEXNETSSIDRANGE 0-8`)
  does not clamp local calls: the first attempt's `IW2OHX-9` — SSID 9,
  above `max_ssid` — was installed as `IW2OHX 9-9` by both `-14` and `-12`,
  and `IR2UFX 0-0` likewise as sent.

## Not covered here

- `FLEXNETLOCALAPPS YES` was not used live; its walk and dedupe are
  unit-tested (`tools/unit/test_local_calls.c`).
- **The independent field test on SR4DON**, offered to and accepted by the
  requester. Its peers (SR6DWH-11, SR1DSZ) are other implementations, which
  is exactly what this test bed lacks.
- A PC/Flexnet `3+` exchange with the new frame, for the v2.2.2 teardown
  regression — see the soak below.

## Release — and what the soak did not cover

A 2 h capture for a `3+` teardown census was started at 13:05Z and
**stopped after 8 minutes**: Marco asked for GA the same afternoon, and the
release restarts would have invalidated it anyway. So **no PC/Flexnet `3+`
exchange was observed with a local call in our frame.** The argument that
this is safe is structural, not measured: the local calls ride in the frame
we already sent, so the number of record frames after a `3-` is unchanged,
and v2.2.2 showed PC/Flexnet reacts to frame count, not content. Unit-tested:
worst case (17 records) is one frame.

GA v2.3.0 at 13:13Z (IR2UFV, flexdebug) and 13:13:50Z (production IW2OHX-13,
silent), with the `IR2UFX` block removed from IR2UFV's cfg. Neither node
advertises a local call, so on the wire both send exactly v2.2.4's frames.

After the removal the IR2UFX route lingered one more hop out. `-14` dropped
its own copy with the link reset, but `D IR2UFX *` on `-14` showed it still
held **the copy `-12` had advertised to it (cost 3)**, while `-12` itself
already answered `no route to IR2UFX`, and every other `-14` neighbour listed
it withdrawn. IR2UFV then relearned it from `-14` at T=6 — as an ordinary
transit route, not a local one. PC/Flexnet advertises inside `3+`
transactions, so its withdrawal to `-14` waits for the next one; until then
`-14`'s entry is stale. Nothing loops: our nodes only learned it from `-14`.
Twelve minutes after the removal (13:26Z) `-12` had withdrawn it too
(`IW2OHX-12 -10`), and so had both our nodes (`IR2UFV -12`, `IW2OHX-13 -12`)
— but the route was **climbing through the wider mesh**: `-14` now held it
at T=8 via `HB9ON-15 8`, with `IQ2LB 2042`. That is count-to-infinity among
other nodes' tables, the same shape as the 2026-09-17 IR2UFX phantom, and it
ends when the cost reaches infinity. We cannot stop it (we no longer
advertise the call and never re-learn it as local); we can only avoid
feeding it, which both nodes' negative entries show they do not.

**Lesson for the README:** retiring a local call is not instant anywhere in a
FlexNet mesh. Before withdrawing one, expect minutes to tens of minutes of a
dying route at climbing cost on distant nodes.
