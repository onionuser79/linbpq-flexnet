# Patch 0001 (L2 connect to an aliased APPLICATION) — live A/B on IR2UFV

2026-10-02, IR2UFV (test instance), LinBPQ 6.0.25.41 + linbpq-flexnet
v2.3.0, flexdebug flavour for both binaries. Report:
[issue #1](https://github.com/onionuser79/linbpq-flexnet/issues/1)
(SR4DON, `C SR4DXC` from SR1DSZ / SR6DWH-11). Patch:
[`patches/0001-l2-appl-alias/`](../../patches/0001-l2-appl-alias/).

## Setup

Reproduces Tom's configuration shape on IR2UFV:

```
; Telnet port (PORTNUM=1) CONFIG
SECURETELNET=1                  ; or 0 for run A2 (1 is also the default)

APPLICATION 2,UFVT,ATTACH 1 127.0.0.1 63999 S,IR2UFX,UFVTST,0
FLEXNETLOCAL IR2UFX
```

`127.0.0.1:63999` is a dummy application (`harness/listener.py`) that
prints a banner, echoes, and logs what LinBPQ sends it. The remote user
is the -14 telnet user `IW7EAS-1`, connecting with `C IR2UFX` from (X)Net
IW2OHX-14 through the v2.3 local-call path
(`IW7EAS-1>IR2UFX via IW2OHX-14* IR2UFV`), exactly like SR1DSZ → SR4DON.
`D IR2UFX` on -14 answered `route: IW2OHX-14 IR2UFV IR2UFX` before every
run.

| Binary | md5 (first 12) |
|---|---|
| v2.3.0 as deployed | `e544e53e2060` |
| v2.3.0 + patch 0001 | `bcbf44777df8` |

## Result

| Run | Binary | SECURETELNET | `C IR2UFX` from -14 | Listener |
|---|---|---|---|---|
| A | v2.3.0 | 1 | `*** connected to IR2UFX` / `Error - Telnet Outward Connect needs SYSOP Status` / `*** reconnected to IW2OHX-14` | no connection |
| A2 | v2.3.0 | 0 | `*** connected to IR2UFX` / `Error - Invalid Command` / `*** reconnected to IW2OHX-14` | no connection |
| **B** | **patched** | **1** | `*** connected to IR2UFX` / `*** Connected to UFVT` / `PATCHTEST-APP: connected to the dummy application`, user data echoed | `ACCEPT`, `RX b'IW7EAS-1\r\n'`, `RX b'hello from the patch test\r'` |

Runs A and A2 match Tom's two results character for character, which
confirms the diagnosis: the L2 path ran `ATTACH 1 127` (the alias cut at
12 characters) as the non-secure remote user. In run B the session got
the whole alias with temporary sysop status, and LinBPQ sent the user's
callsign to the application on connect, the way DXSpider's BPQ listener
expects.

Regression checks on the patched binary, SECURETELNET=1:

| Path | Result |
|---|---|
| Local `C IR2UFX` from the IR2UFV console (sysop) | Reaches the application, signon `IW7EAS` |
| Local `UFVT` command | Reaches the application, signon `IW7EAS` |
| L2 `C IR2UFV-8` from -14 (APPLICATION 1, BBS, **no alias**, `cATTACHTOBBS` path) | BBS prompt `[BPQ-6.0.25.41-B2FWIHJM$]`, unchanged |

Not exercised: NET/ROM connect to the application (qual 0 keeps it out
of NODES; that path is untouched by the patch), and the XID-before-SABM
variant of `L2SABM()` (it reaches the same line with the same
`LINK->ApplName`).

## Afterwards

IR2UFV was restored to the original cfg (`cmp` identical) and the
v2.3.0 binary (`e544e53e2060`), listener stopped. `IR2UFX` was
advertised again only for the duration of the test; PC/Flexnet ages a
withdrawn local call out within minutes (see
[`../local_calls_2026-09-28/FIELD_TEST.md`](../local_calls_2026-09-28/FIELD_TEST.md)).

Trap hit while cleaning up: `pkill -f "...listener.py"` inside
`ssh host '...'` matched the remote `bash -c` carrying that same string
and killed the ssh session (exit 255). The listener did die with it.
Kill by PID, or match on something that is not in your own command
line.

## Harness

`harness/` — `listener.py` (dummy app), `drive.py` (telnet login + one
command; credentials from `NODE_USER`/`NODE_PASS`), `ufvctl.sh`
(backup / test cfg with SECURETELNET=n / install binary + restart /
restore). Run on iw2ohx-gw.
