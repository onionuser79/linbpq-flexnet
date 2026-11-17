# WAMPES <-> IR2UFV FlexNet interop, 2026-11-17

Test link: WAMPES IW2OHX-2 (gw, upstream HEAD d6512c2, runtime-only `ufv`
iface 127.0.0.1:10098, `flexnet filter ir2ufv in all advert yes feed yes`,
`flexnet link add ir2ufv`) <-> IR2UFV (`MAP IW2OHX-2 127.0.0.1 UDP 10098 F>`,
private). Capture: gw `/tmp/wampes-flex-20261117-1457.pcap` (lo, udp 10098).
Logger: gw `/tmp/wampes-flex-watch.log` (WAMPES `flexnet link` every 60 s).

## Defect (v2.10.0)

WAMPES polls every 300 s with PC/Flexnet's 201-byte keepalive and times
the poll against the next type-1. IR2UFV classed it PCF (CR-terminated KA)
and sent type-1 at most every 320 s (in practice 329 s, on the 29 s KA grid):

| WAMPES poll | IR2UFV next type-1 | WAMPES Local | Smooth |
|---|---|---|---|
| 15:02:32 | 15:03:01 | 285 | 40 |
| 15:07:32 | 15:08:30 | 575 | 103 |
| 15:12:32 | 15:13:59 | 865 | 187 |
| (15:2x)  |          | 1444 | 402 |

PC/Flexnet, same KA bytes, every ~16 s: research/xnet14-pcf-2026-06-02.pcapng.

## Fix (v2.10.1-rc1, deployed IR2UFV 15:28:50)

CR-terminated KA >= 200 s after the previous one = slow poller: type-1
answered at once with own measurement, our KA every 300 s, no PCF handling.

```
15:38:42.912 WAMPES 201 '2  ...'     <- poll (2nd of session: flag set)
15:38:42.937 IR2UFV   3 '15\r'       <- answered in 25 ms (no sample yet -> 5)
15:43:42.912 WAMPES 201 '2  ...'
15:43:42.941 IR2UFV   3 '11\r'       <- 29 ms, own measurement 1
```

WAMPES `flexnet link` from 15:39: Remote 1, Local 1, Smooth 5 -> 4.
IR2UFV `FL +`: `IW2OHX-2  wampes`, Q/T 1, rtt 1/1. Our KA cadence to it went
29 s -> 300 s after 15:38:42. Per cycle 2-3 extra 3-byte type-1 frames
(WAMPES answers our KA with '1', we answer that '1'); WAMPES never answers a
'1', so no loop.

## Side finding

After the IR2UFV restart (15:28:50) the link stayed half-open: WAMPES kept
its AX.25 link "Connected" and IR2UFV (AXUDP, waits to be called) had none,
>= 4.5 min until `flexnet link delete/add ir2ufv` in WAMPES. Same family as
the ROADMAP SABM-collision item.

## Not tested (topology)

Cost-0 withdrawals from WAMPES and WAMPES's reading of our 60000: with one
partner WAMPES advertises nothing, and IR2UFV's -14 link is F> (no subnet).
