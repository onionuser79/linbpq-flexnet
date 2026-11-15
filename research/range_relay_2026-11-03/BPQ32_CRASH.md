# Relaying into stock BPQ32 — IW2OHX-15 crashed three times (2026-11-03)

Context: the IW2OHX-12 → **IW2OHX-4** cutover (range `IW2OHX 4-12`, v2.10). Covered
externals -5 CHAT, -7 AI, -8 BBS, -10 RMS are application calls of BPQ32 IW2OHX-15
(Windows, 6.0.25.38) on the same host. A dedicated port 18 on -15 carried the relay.
Station view and current state: iw2ohx-bpq-ops `flexnet-ssid-map.md` §6.

## Same fault every time

`bpq32.exe` → `bpq32.dll` 6.0.25.38, `c0000005` at offset **`0x150ca3`** (a second fault at
`0x0a80d4` follows ~6 s later, during the crash teardown). WER reports in
`C:\ProgramData\Microsoft\Windows\WER\ReportArchive\AppCrash_bpq32.exe_*`.

| -15 start | Crash | After | Port 18 | -4 build | Relay traffic before the crash |
|---|---|---|---|---|---|
| 15:45 | — (ran 75 min) | — | AXUDP, `MAP IW2OHX-4 127.0.0.1 UDP 10099` | rc1 | relayed SABMs **dropped** by BPQAXIP (source call not mapped) |
| 17:00:33 | 17:03:08 | 155 s | AXUDP + `DONTCHECKSOURCECALL` | rc1 | AI, BBS SABMs `USER > call via IW2OHX-14*,IW2OHX-4*`; no session in the app logs |
| 17:23:36 | 17:26:09 | 153 s | KISS over UDP (rx 10101 / tx 10102, `NOPARAMS`, `QUALITY=0`) | rc2 | AI, BBS, CHAT sessions clean (`Incoming Connect from IW7EAS on Port 18`); RMS/CMS answered in the crash second |
| 17:31:23 | 17:33:50 | 147 s | same KISS over UDP | rc2, externals **off** | **none** (-4 `Range relay: out=0`) |
| 17:44:26 | — | — | **no port 18** (pre-cutover config) | rc2 | none |

**Reading:** a timed event ~150 s after start, set off by the port-18 configuration —
not by relayed sessions (crash 3 had none; crash 2's CMS overlap was coincidence).
Whether it is -15's own periodic work on that port or a periodic frame from -4 (port 6
KISS / port 2 AXUDP towards 127.0.0.1) was not established. Not reproducible on LinBPQ
(IR2UFV ran the relay to a plain AXUDP station all afternoon).

## What did work over the KISS link

AI greeted `IW7EAS-1`, the BBS logged `Incoming Connect from IW7EAS on Port 18`, the chat
server joined `IW7EAS`, RMS reached CMS as `IW7EAS-1`: the relay itself (single-digi leg)
is fine against BPQ32's application layer.

## Why AXUDP needed DONTCHECKSOURCECALL at all

Stock BPQAXIP drops a frame whose ORIGIN is not in its MAP table ("can't reply") unless
`DONTCHECKSOURCECALL` or `AUTOADDMAP`. A relayed session carries the user's call. Our
overlay `bpqaxip.c` accepts such frames when the UDP source IP matches an F-flagged MAP
(FlexNet relay); BPQ32 has no equivalent. KISS has no call→address table.

## State after

-15 back on `bpq32.cfg.bak-20261103-154402-node4` (hash-identical, no port 18), stable past
150 s. -4: `FLEXNETEXTERNAL IW2OHX-5/-7/-8/-10` commented out; port 6 idle.
Candidate next steps: report to G8BPQ with the three WER dumps; or serve -7/-10 from the
LinBPQ node itself.
