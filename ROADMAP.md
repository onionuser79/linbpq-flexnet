# Roadmap

**Current release: v2.11.2** (LinBPQ 6.0.26.1). What has shipped is in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

The node is a correct FlexNet participant and, when configured to, a
router for everything it can carry, with per-link routing policy. The
next releases close the gap to the network-management side of the
dedicated routers: seeing what the routing is doing, managing links
without a restart, and settling the remaining interoperability
questions. Features are taken from the (X)Net 1.38 manual where (X)Net
documents them; section numbers below refer to that manual.

## Next: v2.12 — routing reset and connect fallback

| Item | What | Notes |
|---|---|---|
| **FlexNet routing reset** | Send it on demand with a sysop command (`FLRESET <call>`): an init on the live session, then accept the neighbour's full table | Wire format captured (PROTOCOL_SPEC §5.3.8). Receiving it already works: it is answered as a peer re-init (§5.3.7) |
| **`C` over the better of NET/ROM and FlexNet** | A callsign in both tables is connected over NET/ROM only; a dead NET/ROM path makes it unreachable although FlexNet works (README *Known limitations*, seen live in the v2.6.0 QA pass). (X)Net connects over the better of the two (§4.1.2). First step: retry over FlexNet when the NET/ROM connect fails; then compare the two costs | NET/ROM quality and FlexNet link time are different units, so the comparison needs a defined conversion |
| **Recover a link left half-open by a SABM collision** | After a neighbour restart, LinBPQ can answer the neighbour's SABM with UA while its own link waits in the connecting state, discarding traffic until the neighbour times out (~80 s seen) | Stock LinBPQ L2 behaviour; v2.6.2 fixed only the FlexNet init that followed, not the L2 state |

## v2.13 — links and table sizes without rebuilding or restarting

| Item | What | Notes |
|---|---|---|
| **Add and remove FlexNet links at runtime** | Sysop commands to add, remove and list FlexNet neighbours and their options, on AXUDP and KISS ports, as (X)Net's `ro flexnet add/del/list` (§4.3.24.3). Removes the README limitation that a link change needs a restart | Changes LinBPQ's AXIP MAP table at runtime; must work on the Windows build too |
| **Configurable table sizes** | Directives for the maximum number of neighbours, destinations, transit circuits and local calls, today fixed at 8, 2000, 128 and 16 (README *Known limitations*), as (X)Net's `PA BOOT destin` (§4.3.14.1) | The 8-neighbour limit is the first a hub reaches. Arrays allocated once at start-up |

## Later — candidates, not scheduled

| Item | Why | Trigger |
|---|---|---|
| **Per-hop acknowledgement** of forwarded sessions, as (X)Net does | Users through the node would no longer pay the whole path's round trip in every retry timer | Needs a capture of frame loss through an (X)Net path first |
| Verify NET/ROM L4 (CREQ) transit towards LinBPQ FlexNet peers | Implemented since v2.2 under `FLEXNETTRANSIT` (L4 envelopes addressed to a destination behind a FlexNet neighbour are forwarded at L2), but never exercised live: (X)Net and PC/Flexnet do not use it | A pair of LinBPQ nodes that route NET/ROM through a FlexNet hop |
| Built-in rotation of `/tmp/flexnet_axudp.log` | Debug builds grow the log without limit; logrotate `copytruncate` covers it on Linux (README *Build*), but Windows has no logrotate. Release builds never open the log, and the v2.11 route-event log reduces the need for debug builds | An operator who needs to run a debug build on Windows for long periods |

## Considered and not planned

(X)Net features reviewed for this roadmap and left out:

- `Find` (§4.1.4), `NRR` (§4.1.13), beacons, `crond`, `callchkd`: plain
  AX.25 or NET/ROM functions, already in LinBPQ or outside FlexNet.
- `statd` / `rstatd` link statistics over time (§5.1.15–16): better
  collected outside the node by sampling `FL`.
- `ro local` with NET/ROM and proxy entries (§4.3.24.4): LinBPQ has its
  own NODES and application handling; `FLEXNETEXTERNAL` covers the
  FlexNet side.
- Changing the SSID range at runtime (`ro flexnet param ssid`,
  §4.3.24.3.4): rarely changed; a restart is acceptable.

## Open protocol questions

Listed in [PROTOCOL_SPEC.md §14](PROTOCOL_SPEC.md#14-open-questions).
None of them blocks interoperation; the v2.11 capture session targets
§14.6.

## Done

### v2.11.2 — replies on a user's FlexNet connect

Shipped in v2.11.2; see [RELEASE_NOTES.md](RELEASE_NOTES.md): responses
the node builds from a received frame (UA to a DISC, DM, FRMR) keep the
node marked as repeated, so the neighbour relays them and the far end no
longer repeats its DISC until its retry limit. Found by a QA smoke run.

### v2.11 — routing visibility and the cost-0 withdrawal

Shipped in v2.11.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md): cost-`0`
records act as withdrawals (also closes the WAMPES item below), `D
<call> *`, `FLSTAT`, `FLOG`. A full QA campaign on this release is
still to run; it should include a check that no destination is withdrawn
on a cost-`0` record and then never offered again.

**Capture session on a live (X)Net node — done.** Results, now in
PROTOCOL_SPEC:

- the routing-reset frame is an ordinary init sent on the live session
  (§5.3.8); this node already answers it correctly;
- removing a link announces the lost destination with cost `0`, not
  `60000`, and no infinity follows; a remote loop can then keep the
  destination alive (§7.4) — hence the phantom fix above;
- (X)Net accepts exactly one option character per link (§13.1); `+`
  applies to routes as received;
- (X)Net opening a KISS link itself: not tested, no RF (X)Net neighbour
  available.

### v2.10.1 — WAMPES neighbours

Shipped in v2.10.1; see [RELEASE_NOTES.md](RELEASE_NOTES.md): a
neighbour that polls with PC/Flexnet's keepalive every few minutes
(WAMPES) gets each poll answered at once. WAMPES's cost-`0` route
withdrawals are handled since v2.11.

### v2.10 — external stations inside the node's own SSID range

Shipped in v2.10.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md):
`FLEXNETEXTERNAL` entries inside `FLEXNETSSIDRANGE` are covered by the
range record, and connects that peers address straight to an SSID of the
range are relayed to the station.

### v2.9 — (X)Net-style link overview

Shipped in v2.9.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md): `FL` opens
with a table laid out as (X)Net's `L`, listing NET/ROM, INP3 and FlexNet
links with their AX.25 statistics and link quality.

### v2.8 — external stations on the node's own base call

Shipped in v2.8.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md):
`FLEXNETEXTERNAL` accepts a station that shares the node's base call
when its SSID is outside the node's own SSID range. v2.8.2 adds the
`Q/T` column to `FL`: the node's own link time to each neighbour, in
(X)Net's unit.

### v2.7 — FLEXPROBE and parser health

Shipped in v2.7.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md):
`FLEXPROBE <call>` (a path query on demand, with a listing of the
outcomes) and a `Parser:` line in `FL` counting unknown and malformed
frames from neighbours.

### Windows build in production use

`win/` cross-compiles `LinBPQ.exe` with FlexNet; see
[win/README.md](win/README.md). Introduced in v2.6.0. Since 2026-10-08 a
Windows node has run on it continuously as a production node, through
every release up to v2.10.0, with FlexNet links over AXUDP and over KISS
on serial COM ports to (X)Net and LinBPQ neighbours. That is a longer
run than any Linux soak so far. It is started by hand on purpose: its
KISS COM ports go through a serial port splitter, which must be checked
running before LinBPQ starts, so an autostart is not wanted.

### v2.6.1 — routing-table fixes

A QA pass over a live mesh of three LinBPQ and three (X)Net nodes; see
[RELEASE_NOTES.md](RELEASE_NOTES.md). No new directives.

### v2.6 — routing between ports, external stations

Shipped in v2.6.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md). Verified
live on a node with an (X)Net neighbour on two RF KISS ports and FlexNet
neighbours on AXUDP, including an external station: connects and path
queries from every neighbour, through the node, across ports.

### v2.5 — FlexNet over KISS (RF) ports

Shipped in v2.5.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md) and the
README's *FlexNet on a KISS (RF) port*. Tested between two LinBPQ nodes
on a KISS link, and since v2.6.0 live with an (X)Net neighbour on two
RF channels (1200 and 9600 Bd; link times of a few seconds at 1200 Bd).
Whether (X)Net opens a KISS link itself is in the v2.11 capture session.

### v2.4 — per-link routing options

Shipped in v2.4.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md) and the
README's *Per-link routing options*. The two remaining interoperability
questions (withdraw or fall silent on an option change; more than one
option character) are in the v2.11 capture session.

Settled from a live (X)Net node's configuration and tables: (X)Net
applies `+` to routes **as received**, as this node does. A node with a
direct `+` link to a neighbour (link time 100 ms) routed to that
neighbour through another path at a cost of 2.8 s, and advertised that
path onward; the penalised link carried no destinations.

## Out of scope

- Replacing the dedicated FlexNet routers — (X)Net, PC/Flexnet,
  RMNC/Flexnet.
- A shared protocol library with the sibling `flexnetd` project.
