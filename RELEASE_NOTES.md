# Release notes

Newest first. Each entry says what changed, what an operator needs to do
when upgrading, and whether anything changed on the wire. Directive and
command details are in [README.md](README.md); protocol details in
[PROTOCOL_SPEC.md](PROTOCOL_SPEC.md).

| Version | Date | LinBPQ base | Highlights |
|---|---|---|---|
| [2.3.1](#v231--2026-10-02) | 2026-10-02 | 6.0.25.41 | AX.25 connects to aliased applications run the whole alias |
| [2.3.0](#v230--2026-09-28) | 2026-09-28 | 6.0.25.41 | Local application callsigns as FlexNet destinations |
| [2.2.4](#v224--2026-09-28) | 2026-09-28 | 6.0.25.41 | L2 forwarding hardened |
| [2.2.3](#v223--2026-09-28) | 2026-09-28 | 6.0.25.41 | Upstream rebase |
| [2.2.2](#v222--2026-09-22) | 2026-09-22 | 6.0.25.40 | PC/Flexnet link drops after full-table exchange fixed |
| [2.2.1](#v221--2026-09-21) | 2026-09-21 | 6.0.25.40 | Full-table answers, count-to-infinity guard, cost clamp |
| [2.2.0](#v220--2026-09-19) | 2026-09-19 | 6.0.25.40 | Opt-in router role; packed route frames |
| [2.1.x](#v21x--2026-05-16--2026-09-14) | 2026-05 → 09 | up to 6.0.25.40 | PC/Flexnet compatibility; upstream rebases |
| [2.0.0](#v200--2026-05-15) | 2026-05-15 | 6.0.25.x | First general release |
| [1.x](#v1x--2026-04-12--2026-05-15) | 2026-04 → 05 | 6.0.25.x | Development series |

---

## v2.3.1 — 2026-10-02

**AX.25 connects to an application with a command alias now run the
whole alias.**

An application whose command is an alias making an outward Telnet
connection, for example a DX cluster on the same host:

```
APPLICATION 3,DX,ATTACH 2 127.0.0.1 63000 S,DXCL,DXCLUS,255
```

worked for local users and over NET/ROM, but an AX.25 connect to `DXCL`
failed. This includes a connect arriving through FlexNet as a local
callsign (v2.3.0). The link came up, then the user saw
`Error - Telnet Outward Connect needs SYSOP Status` (with
`SECURETELNET=1`, the default) or `Error - Invalid Command` (with
`SECURETELNET=0`).

The cause is in LinBPQ's own L2 accept path, which this overlay carries
in `L2Code.c`. It ran the first 12 characters of the alias text as the
connecting user (`ATTACH 2 127` in the example). The NET/ROM path runs
the application name instead, and LinBPQ then expands the whole alias
with temporary sysop status for that one command. The L2 path now does
the same.

Verified on a live node, connecting from an (X)Net neighbour over the
local-callsign path: both errors before the change, the application
reached afterwards with `SECURETELNET=1`. Local connects, the local
application command and connects to applications without an alias are
unchanged.

**Upgrading:** review your application aliases first. An AX.25 user
connecting to an aliased application now runs that alias with temporary
sysop status, as NET/ROM users and local users of the application
command already did. Nothing else to do. The fix is also available as a
stand-alone patch for stock LinBPQ:
[patches/0001-l2-appl-alias](patches/0001-l2-appl-alias/).

**Wire impact:** none.

## v2.3.0 — 2026-09-28

**Local application callsigns as FlexNet destinations.**

`FLEXNETSSIDRANGE` can only advertise SSIDs of the node's own base
callsign. A node whose applications use other callsigns — a BBS as
`BBSX` and a DX cluster as `DXCL-2` on a node called `NODEA` — could not
make them reachable from the FlexNet network at all.

New directives:

```
FLEXNETLOCAL BBSX DXCL-2       ; explicit list, up to 16 callsigns
FLEXNETLOCALAPPS YES           ; or: every APPLICATION callsign outside NODECALL's base
```

- Each callsign is advertised as its own destination at cost 1, in the
  same route frame as the node's own record, so the number of frames a
  peer receives does not change.
- Every entry is checked against the `APPLICATION` table at start-up.
  Callsigns nothing answers, and callsigns on the node's own base, are
  reported (also on a silent build) and not advertised. A warning is
  printed if `BBS=0`, which would make LinBPQ refuse every application
  connect.
- Path queries for a local callsign are answered with the chain
  `asker, node, callsign`, and the resulting connect
  (`user > callsign via node`) is delivered to the application; replies
  carry the node callsign as a repeated digipeater. A direct connect with
  no digipeater also works.
- Peers echoing a local callsign back are ignored (counted as
  `echo-skips` in `FL`).
- `FL` lists the local callsigns and their status; `D` shows them at the
  end of the listing and identifies them in the detail view.

Tested from (X)Net and PC/Flexnet neighbours, one and two hops away.

**Upgrading:** nothing to do. Without the new directives the node sends
exactly the frames v2.2.4 sent. Before enabling, see the checklist in the
README (`BBS=1`, `PERMITTEDAPPLS`, and choosing a callsign that is not in
use on FlexNet or NET/ROM).

Requested in [issue #1](https://github.com/onionuser79/linbpq-flexnet/issues/1).

## v2.2.4 — 2026-09-28

**L2 forwarding hardened.** Applies to nodes with `FLEXNETL2TRANSIT YES`.

- **Next hop pinned per circuit.** Up to v2.2.3 the next hop was looked up
  for every frame. If the route to the destination changed during a
  session, frames still returning over the old hop were not recognised
  and reached the originator carrying a digipeater it never used, which
  broke the session. The hop is now fixed on the circuit's first frame
  and kept while that neighbour's link is up; it is re-resolved only when
  the link is gone or a new connection follows a completed teardown. A
  replaced hop is still removed from frames in flight.
- **Circuit lifetime follows the AX.25 session.** DISC starts the
  teardown, UA or DM completes it, and the slot is kept 120 s for
  retransmissions. An open circuit keeps its slot through 2 h of silence
  (was 15 min). When the table is full only closed or long-silent
  circuits are reclaimed, never a live one. Table size 64 → 128.
- **Frames that loop back** to the node on a circuit it forwards are
  dropped.
- `FL` shows `circuits`, `repinned`, `looped` and `evicted`.

Wire format unchanged. Nodes without `FLEXNETL2TRANSIT YES` are
unaffected.

## v2.2.3 — 2026-09-28

Rebased onto **LinBPQ 6.0.25.41** (adds upstream's `NPING` command). No
FlexNet change.

Two upstream defects are still present in 6.0.25.41 and remain handled
in the overlay, as since v2.1.41: the crash-test null dereference in the
`REBOOT` command is removed, and the AXIP receive-error messages no
longer use an uninitialised buffer as a format string.

## v2.2.2 — 2026-09-22

**Links to PC/Flexnet no longer drop after a full-table exchange.**

PC/Flexnet asks its neighbours for their whole table (`3+`) every
75–90 minutes. After the answer is closed with `3-`, it accepts at most
two further route frames on that link and then disconnects. Since v2.2.0
changes were pushed as they happened, so a link to PC/Flexnet ended
within a minute of almost every exchange.

- New directive **`FLEXNETPCFQUIESCE`**, default `YES`: after answering a
  PC/Flexnet neighbour's `3+`, send it no further route records until its
  next `3+`. PC/Flexnet neighbours only; (X)Net neighbours are unaffected.
- When PC/Flexnet starts a new AX.25 session on a link (it recycles AXIP
  links on a fixed lifetime of about 5445 s), the node re-sends its routes
  at once, so the neighbour is never left without them.

Measured: full-table exchanges followed by a link drop went from 30 of 30
to 0 of 3 over the verification window; median session lifetime from
under 15 minutes to PC/Flexnet's full ~90-minute cycle.

**Upgrading:** nothing to do; the default is the fix. `NO` restores the
v2.2.1 behaviour.

## v2.2.1 — 2026-09-21

Four corrections to route advertisement.

- **A full-table request is answered with the full table.** A `3+` was
  being answered only with entries whose cost had changed by more than
  10 % — typically a handful out of two hundred — so every neighbour's
  view of the node was incomplete. The answer now contains every
  advertisable destination.
- **`3-` is sent after the answer, not in the middle of it.** The
  end-of-batch token went out as soon as the send queue was momentarily
  empty between rate-limit refills, and further records followed it. It
  now waits until the queue has stayed empty for two refill intervals.
- **Count-to-infinity guard.** A destination whose cost rises three times
  in a row to four times the lowest cost seen is treated as a routing
  loop: it is withdrawn once and held down. Before this, about a third of
  re-advertised destinations could be climbing at any one time.
- **Finite costs are clamped to 4095** on the wire. PC/Flexnet stores link
  cost in 12 bits; a larger value is corrupt to it. The unreachable value
  60000 is unchanged.

## v2.2.0 — 2026-09-19

**Opt-in router role**, all directives defaulting to `NO`:

- `FLEXNETTRANSIT` — re-advertise routes learned from neighbours.
  Change-driven (a record goes out when the advertised cost moves by
  ≥ 10 %), paced per neighbour (PC/Flexnet 1 frame / 5 s, (X)Net 1 / 2 s),
  with split horizon, withdrawal of routes lost with a neighbour, a hold-
  down on withdrawn routes, ageing of learned routes, and a 120 s refresh
  of direct neighbours. Requires `DIGIFLAG=1` on the FlexNet port.
- `FLEXNETL2TRANSIT` — carry sessions to multi-hop destinations by
  FlexNet L2 chain rewriting: append the next hop on the way out, remove
  it on the way back. Enabling it widens re-advertisement from direct
  neighbours to every learned destination.
- `FLEXNETPATHFORWARD` — relay other nodes' path queries by inserting the
  next hop, instead of answering from cache. Resolves paths longer than a
  single answer can express.
- `FLEXNETLT3BYTE` — accept three-byte link-time frames (default `NO`).

The compiled default of `FLEXNETTRANSIT` is now `NO`. Earlier 2.1 builds
defaulted to `YES`, so a node without the directive re-advertised routes.

**Link stability**, independent of the router role:

- **Route records are packed.** Several records now share one frame, as
  the protocol intends; previously each record used its own I-frame
  (about 15 bytes of a 236-byte frame). Re-sending a 200-destination
  table after a reset went from about 18 minutes to under 100 seconds,
  and the queue towards PC/Flexnet from busy 80 % of the time to 6 %.
- **No re-init on a healthy link.** When LinBPQ recycled a neighbour's
  internal link slot, the node reset its FlexNet session and sent a new
  init, which makes PC/Flexnet restart its measurement of the link.
  PC/Flexnet's reported cost for the node dropped to the level of its
  (X)Net neighbours.
- Path answers never contain more than 8 digipeaters. A longer chain
  cannot be expressed in AX.25; answering with one made the destination
  unreachable for the asking peer.

Also: the repository's first unit tests (`tools/unit/`).

**Upgrading:** a node that had no `FLEXNETTRANSIT` line and relied on the
old `YES` default must now set it explicitly. Recommended alongside:
`RETRIES=25` on the FlexNet port and `OnlyVer2point0=1` (see README).

---

## v2.1.x — 2026-05-16 → 2026-09-14

The 2.1 series made the node a stable neighbour for PC/Flexnet and kept
it current with upstream LinBPQ.

### Upstream rebases and maintenance

| Version | Date | Change |
|---|---|---|
| 2.1.42 | 2026-09-14 | Version marker for the upstream makefile fix below. No functional change. |
| 2.1.41 | 2026-09-10 | Rebased onto LinBPQ 6.0.25.40. Three upstream defects handled: crash-test null dereference in `REBOOT` removed; AXIP receive-error messages no longer use an uninitialised buffer as a format string (reachable from a malformed AXUDP datagram); missing `-lbacktrace` added to the makefile (upstream later fixed it the same way). |
| 2.1.40 | 2026-08-11 | Rebased onto LinBPQ 6.0.25.36. |
| 2.1.39 | 2026-07-09 | `FL` no longer shows a working link as `PENDING` after LinBPQ recreates the session internally; a link is considered established from ongoing valid FlexNet traffic, not only from the peer's one-time init. |
| 2.1.38 | 2026-06-03 | `FLEXNET_PROD` build switch: silent console for production. |
| 2.1.37 | 2026-06-03 | Version string and documentation. |
| 2.1.35 | 2026-06-02 | Rebased onto LinBPQ 6.0.25.30 (new INP3 fields in the shared header). |

### PC/Flexnet compatibility

| Version | Change |
|---|---|
| 2.1.36 | PC/Flexnet sometimes sends FlexNet frames with PID `0xF0`. They are now recognised by content and processed, instead of dropped. |
| 2.1.27 | Non-FlexNet frames on a FlexNet link are no longer answered with a LinBPQ banner, which PC/Flexnet treats as a protocol error. |
| 2.1.24 – 2.1.28 | Keepalive every ~29 s towards PC/Flexnet (it recycles quiet AXIP links); link-time value tuned so PC/Flexnet's link-cost samples settle at single digits. |
| 2.1.25 – 2.1.26 | When PC/Flexnet re-establishes the AX.25 session, the existing FlexNet session is adopted instead of restarted. |
| 2.1.14 – 2.1.16 | Sessions survive LinBPQ's internal link-slot maintenance: a transient link state no longer ends the FlexNet session, an established session is never re-handshaked, and a session is moved to a recycled link slot instead of being recreated. |
| 2.1.17 – 2.1.23 | An init rate-limit was tried and **withdrawn** in 2.1.23: PC/Flexnet expects a full handshake on every new AX.25 session. |
| 2.1.13 | **Link-time frames rate-limited** — at most one per 320 s towards PC/Flexnet, per 20 s towards (X)Net. Frames sent sooner were recorded by PC/Flexnet as saturated samples, pinning its cost for the node at 4095. |
| 2.1.12 | A different keepalive reply was tried and withdrawn in 2.1.13. |
| 2.1.11 | Routes are sent on receipt of the peer's init rather than its first keepalive; peer type inferred from the init. |
| 2.1.10 | Keepalive shape accepted from PC/Flexnet (201 bytes, `CR`-terminated). |
| 2.1.8 | A connection to a direct FlexNet neighbour carries the node callsign as a repeated digipeater; PC/Flexnet refused a bare user SABM. |
| 2.1.7 | Users' own connections to a FlexNet neighbour are no longer mistaken for the node-to-node FlexNet link. |
| 2.1.6 | Route frames no longer start with `3+`, which is a request, not a marker; PC/Flexnet was dropping the link. |
| 2.1.0 | Inbound SABM from a PC/Flexnet neighbour accepted as a FlexNet link, without the LinBPQ connect banner. |

Versions 2.1.33 and 2.1.34 were experiments that were not released.

### Operator features

| Version | Change |
|---|---|
| 2.1.9 | `D` sorting (`/COST`, `/CALL`, `/AGE`), neighbour filter (`D < call`), cached-path filters (`D !`, `D ?`). |

**Upgrading within 2.1:** nothing to do; no configuration changes.

---

## v2.0.0 — 2026-05-15

First general release. A LinBPQ node takes part in a FlexNet network with
its own destinations:

- FlexNet link protocol over AXUDP `MAP` entries flagged `F`.
- Node callsign, optionally with an SSID range (`FLEXNETSSIDRANGE`),
  advertised to neighbours.
- Destination table from several FlexNet neighbours, cheapest route per
  destination.
- `C <call>` into the FlexNet network with the node's identity preserved
  in the digipeater chain; inbound connections from FlexNet users.
- Hop-by-hop path discovery with a persistent path cache (`D <call>`
  shows the route).
- L3RTT link probes, including the "no routes" signal to peers.
- `D`, `FL` and `V` commands.

Tested against (X)Net 1.39.

## v1.x — 2026-04-12 → 2026-05-15

| Version | Date | Change |
|---|---|---|
| 1.10.0 | 2026-05-15 | `FLEXNETSSIDRANGE`: advertise an SSID range as one destination and declare its upper edge in the init handshake. |
| 1.9.9 | 2026-05-15 | Fixed corruption of NET/ROM traffic on FlexNet links (PID byte overwritten after L3RTT handling). |
| 1.9.8 | 2026-05-14 | Status frames `"1n" CR` classified instead of logged as unknown. |
| 1.9.7 | 2026-05-14 | Reverted 1.9.4 (see below). |
| 1.9.5 | 2026-05-13 | `C <flexnet-neighbour>` from the console fixed; non-L3RTT `0xCF` frames passed to NET/ROM. |
| 1.9.4 | 2026-05-13 | Re-advertisement of learned routes. **Withdrawn in 1.9.7**: it extended the digipeater chain without contracting it on the return path, which AX.25 v2 rejects. Done correctly in 2.2.0. |
| 1.9.3 | 2026-05-13 | AXIP SSID normalisation; session table cleanup. |
| 1.9.2 | 2026-05-12 | Several FlexNet neighbours with cost-based route selection. |
| 1.9.1 | 2026-05-12 | Path cache persisted to disk. |
| 1.9 | 2026-05-12 | Path discovery (CE types 6/7). |
| 1.3.x | 2026-04 | L3RTT counters and link-down signal, link-time smoothing, keepalive cadence. |
| 1.2.0 | 2026-04-22 | Node identity preserved in the outbound digipeater chain. |
| 1.1.0 | 2026-04-12 | Reconnect handling. |
| 1.0 | 2026-04-12 | First version: FlexNet link protocol, outgoing and incoming connections, `D` and `FL`. |
