# Range relay — live A/B on IR2UFV, 2026-11-03

**Result: PASS.** v2.10.0-rc1 (flexdebug, md5 `167ef5b5…`) forwards an
external that sits inside the node's own SSID range; v2.9.2 (`fa449d44…`)
does not.

## Why

Marco's model of `FLEXNETSSIDRANGE` (2026-11-03): the range **aggregates** —
the network sees one record, `IW2OHX 6-12` — and the node **dispatches** each
SSID to whatever answers it (an APPLICATION, or a station on a port). Wanted
for IW2OHX-12 so that -11 (SHELL on gw WAMPES) and the -15 service calls
(-7 AI, -8 BBS, -9 CONV, -10 RMS) can sit under one -12 range record.
Station analysis: iw2ohx-bpq-ops `flexnet-ssid-map.md` §6.

v2.9.2 refused this: a `FLEXNETEXTERNAL` inside the range was
`FLEX_LOCAL_OWNRANGE` (not advertised, not forwarded), and an SSID in range
is "us" (`FlexNet_IsLocalCall`) → delivered locally → nothing answers.

## Set-up

- IR2UFV: `FLEXNETSSIDRANGE 0-8` (unchanged), added on port 2
  `MAP IR2UFV-5 127.0.0.1 UDP 10199` and `FLEXNETEXTERNAL IR2UFV-5 2`.
  cfg backup `/home/bpq-ufv/bpq32.cfg.bak-20261103-rangeext`, binary backup
  `/home/bpq-ufv/linbpq.pre-v2.10.0-rc1-20261103-rangeext` (= v2.9.2).
  Helper `~iw2ohx/ufv-rangeext.sh cfg|restart|install|revert` on gw.
- The station: `tools/axudp_responder.py --call IR2UFV-5 --listen
  127.0.0.1:10199` — answers SABM with UA + a banner naming caller and path,
  DISC after 3 s, logs every frame. IR2UFV-5 checked free in FlexNet `D`
  and NET/ROM.

## A — v2.9.2

- `FL`: `IR2UFV-5  NOT advertised - FLEXNETEXTERNAL inside our own SSID range`.
- `C IR2UFV-5` from IW2OHX-14 (routed via -12): SABMs
  `IW7EAS-1 -> IR2UFV-5 IW2OHX-14* IW2OHX-12* IR2UFV` reach IR2UFV, logged
  `L2-SABM-NEW` (taken locally), never forwarded; responder log empty; -14
  `link setup (1)...` then gives up.

## B — v2.10.0-rc1, first cut (COVERED state only)

- `FL`: `covered by own SSID range [external, port 2]`; `D` footer
  `Local calls (cost 1): IR2UFV-5(p2,range)`.
- `C IR2UFV-5` from -14, now on its direct link: **(X)Net addressed the SSID
  as the neighbour itself** — `IW7EAS-1 -> IR2UFV-5 IW2OHX-14*`, no IR2UFV
  digi, no path query. Taken locally again (`L2-SABM-NEW`). The first cut
  only fixed the shape where the originator routes *through* us.
- **Wire finding** (now PROTOCOL_SPEC §7.6.6): an SSID of a neighbour's
  range is connected directly; the neighbour must accept and dispatch it.

## B' — v2.10.0-rc1 with the range relay (§10.7)

| Test | Result |
|---|---|
| `C IR2UFV-5` from -14 (direct link) | `*** connected to IR2UFV-5` + banner `caller IW7EAS-1, path IW2OHX-14,IR2UFV` |
| `C IR2UFV-5` from -4 (via -12, IR2UFV named as digi) | connected, `path IW2OHX-4,IW2OHX-12,IR2UFV` — plain external digipeat, no relay |
| `C IR2UFV-5` from IR2UFV's own console | `Connected to IR2UFV-5`, `path IR2UFV` |
| `C IR2UFV-8` from -14 (BBS APPLICATION inside the range) | BBS answers — unchanged |
| -14 / -4 `D` | only `IR2UFV 0-8`; no `IR2UFV 5-5` record |
| `FL` after the tests | `Range relay: out=3 back=3 dropped=0 circuits=1`; all three FlexNet links CONNECTED |

Trace of the -14 case (full lifecycle, `raw_evidence.txt`):

```
RANGE-RELAY-OUT : IW7EAS-1->IR2UFV-5 port 2 -> 2
AXUDP-TX        : IW7EAS-1 -> IR2UFV-5 IW2OHX-14* IR2UFV* SABM
responder       : RX ... via IW2OHX-14*,IR2UFV*  ->  TX UA + banner via [IR2UFV, IW2OHX-14]
RANGE-RELAY-BACK: IR2UFV-5->IW7EAS-1 port 2 -> 2
AXUDP-TX        : IR2UFV-5 -> IW7EAS-1 IW2OHX-14 UA     <- our entry removed
AXUDP-TX        : IR2UFV-5 -> IW7EAS-1 IW2OHX-14 I pid=F0
... RR out, DISC back, UA out — all relayed
```

## Unit

`tools/unit/test_range_relay.c` (30 checks) + `test_external.c` (76, new
`test_covered_by_range`). Mutations caught: not marking the added digi
repeated (2 failures); contracting a reply whose first digi is not ours (1);
`flex_local_find` ignoring COVERED (5). All 21 suites green. Warnings:
FlexNetCode.c 51, L2Code.c 80 — both unchanged.

## Not tested yet

- A covered external on a *different* port from the arrival port (the
  -12 plan: arrival AXUDP port 2 or RF 3/4, station on port 2) — same code
  path (`flex_relay_send` to `ext_port`), but not exercised live.
- PC/Flexnet as the originating peer (only (X)Net and LinBPQ here).
- Long sessions / I-frame traffic beyond the banner; the circuit table
  idles out after 30 min.
- Windows build (`win/`) not rebuilt.
