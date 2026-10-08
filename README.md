# linbpq-flexnet v2.6.0-rc1

FlexNet routing for **LinBPQ**. A LinBPQ node gains a native FlexNet
(CE/CF) protocol stack alongside its NET/ROM stack, so it can peer with
(X)Net, PC/Flexnet and other FlexNet nodes, appear in their destination
tables, accept connections from the FlexNet network and route its own
users' connections into it.

Based on LinBPQ **6.0.25.41** by John Wiseman, G8BPQ. Licence: GPL v2, as
LinBPQ.

- **What's new:** [RELEASE_NOTES.md](RELEASE_NOTES.md)
- **What's next:** [ROADMAP.md](ROADMAP.md)
- **The protocol, for implementers:** [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md)

---

## Contents

1. [Features](#features)
2. [Compatibility](#compatibility)
3. [Installation](#installation)
4. [Configuration](#configuration)
5. [Parameter reference](#parameter-reference)
6. [Console commands](#console-commands)
7. [Operating notes](#operating-notes)
8. [Known limitations](#known-limitations)
9. [Repository layout](#repository-layout)

---

## Features

**Always on, once a link is flagged for FlexNet** — an AXUDP `MAP`
entry with the `F` flag, or a `FLEXNETLINK=` neighbour on a KISS port:

- FlexNet link protocol with each configured neighbour: init handshake,
  keepalive, link-time measurement, compact route exchange.
- The node's own callsign — optionally a range of its SSIDs, and other
  application callsigns it answers — advertised to the FlexNet network.
- Destination table learned from all FlexNet neighbours; the cheapest
  neighbour is used for each destination.
- Outbound: `C <call>` from the BPQ console reaches any FlexNet
  destination. The node's own callsign is kept in the digipeater chain,
  so remote stations see the connection coming from this node.
- Inbound: FlexNet users can connect to the node, to its applications,
  and to its advertised SSIDs.
- Hop-by-hop path discovery (CE types 6/7), with a persistent path
  cache.
- Link-quality probes on the NET/ROM-compatible layer (L3RTT).
- PC/Flexnet compatibility: link-timing and route-exchange rules that
  PC/Flexnet requires to keep the link up.

**Opt-in router behaviour** (all off by default):

- Re-advertising routes learned from neighbours (`FLEXNETTRANSIT`).
- Carrying other stations' sessions to multi-hop destinations by
  FlexNet L2 chain rewriting (`FLEXNETL2TRANSIT`).
- Relaying other nodes' path queries (`FLEXNETPATHFORWARD`).
- Routing between FlexNet neighbours on different ports, e.g. RF and
  AXUDP (`FLEXNETCROSSPORT`).
- Advertising plain AX.25 stations reached on a given port, such as a DX
  cluster, as destinations of the node (`FLEXNETEXTERNAL`).

This is a FlexNet participant that can also act as a router for the
routes it can carry. It is not a replacement for the dedicated FlexNet
routers — (X)Net, PC/Flexnet, RMNC/Flexnet.

## Compatibility

| Peer | Status |
|---|---|
| (X)Net 1.39 | Tested, AXUDP |
| PC/Flexnet (V4 family) | Tested, AXUDP |
| LinBPQ with linbpq-flexnet | Tested, AXUDP and KISS |
| RMNC/Flexnet | Not tested |
| Earlier (X)Net versions | Not tested |
| FlexNet over KISS (RF) ports | Tested between two LinBPQ nodes on a KISS link; not yet against an (X)Net or PC/Flexnet RF neighbour |

Host: Linux. Tested on Raspberry Pi OS / Debian (aarch64). The build is
LinBPQ's own; other LinBPQ platforms have not been tried, except
**Windows** (`LinBPQ.exe`), which builds with mingw-w64 from
[`win/`](win/README.md) — experimental: it starts with FlexNet
initialised but has not yet carried links to live peers.

---

## Installation

linbpq-flexnet is an **overlay**: it contains only the files that differ
from upstream LinBPQ. You copy them over a LinBPQ source tree of the
matching version and build as usual.

For Windows (`LinBPQ.exe`), see [`win/README.md`](win/README.md) instead.

### 1. Build dependencies

```bash
sudo apt update
sudo apt install -y git gcc make libconfig-dev zlib1g-dev \
    libpcap-dev libminiupnpc-dev libjansson-dev libpaho-mqtt-dev
```

`-lbacktrace` is linked as well; it ships with gcc.

### 2. Get LinBPQ and this overlay

```bash
git clone https://github.com/g8bpq/LinBPQ.git ~/linbpq-build
git clone https://github.com/onionuser79/linbpq-flexnet.git ~/linbpq-flexnet
```

Use the LinBPQ version named at the top of this file. The overlay
replaces whole upstream files, so a different upstream version may not
build or may lose upstream changes.

### 3. Overlay the files

```bash
cd ~/linbpq-build
for f in asmstrucs.h bpqaxip.c L2Code.c Cmd.c makefile; do
    [ -f "$f.orig" ] || cp "$f" "$f.orig"          # keep the originals once
done
for f in FlexNetCode.c flexnet_l3.c flexnet_l3.h \
         asmstrucs.h bpqaxip.c L2Code.c Cmd.c makefile; do
    cp ~/linbpq-flexnet/$f .
done
```

### 4. Build

```bash
make clean
make                                   # standard build
```

The binary is `./linbpq`. Three build flavours are available:

| Command | Console output |
|---|---|
| `make` | FlexNet informational messages on the console (session start, routes, neighbours) |
| `make EXTRA_CFLAGS=-DFLEXNET_PROD=1` | **Silent** — recommended for production. Only operator warnings are printed. |
| `make flexdebug` | Informational messages plus a per-frame protocol trace, also written to `/tmp/flexnet_axudp.log`. For troubleshooting. |

Two build pitfalls:

- Pass the silent switch with **`EXTRA_CFLAGS`**. `make CFLAGS+=...` is
  overridden by the makefile targets and silently has no effect.
- **Run `make clean` when switching flavour.** Object files keep the
  flags they were built with, and `make` does not notice a flavour change.

To check which flavour a binary is:
`strings linbpq | grep -c 'FlexNet: '` — a silent build prints a
single-digit count, a standard or debug build around 90.

### 5. Install

Stop LinBPQ, back up the old binary, copy, start:

```bash
sudo systemctl stop linbpq
sudo cp /usr/local/bin/linbpq /usr/local/bin/linbpq.bak.$(date +%Y%m%d)
sudo cp ~/linbpq-build/linbpq /usr/local/bin/linbpq
sudo systemctl start linbpq
```

Adapt the paths and service name to your installation. The running
binary must be stopped first — copying over it fails with
`Text file busy`.

Rollback: copy the `.bak` binary back and restart.

### 6. Verify

On the node console:

```
V
Version 6.0.25.41 (64 bit) and FlexNet v2.6.0-rc1
```

The `and FlexNet vX.Y.Z` suffix confirms the module is present. Then
`FL` should list each FlexNet neighbour as `CONNECTED` within a minute or
two of the link coming up.

LinBPQ itself logs `not recognised - Ignored` for each `FLEXNET...`
directive at start-up. That is expected: the FlexNet module reads those
lines itself.

---

## Configuration

All configuration is in `bpq32.cfg`. The FlexNet directives are read once
at start-up; restart the node after changing them.

### Minimum: flag a neighbour as FlexNet

On an AXIP/AXUDP port, add `F` to the `MAP` entry of each FlexNet
neighbour:

```
MAP NODEB-2 192.0.2.10 UDP 10093 F
```

That is enough for the node to join the FlexNet network with its own
callsign.

Use `F` alone for a FlexNet neighbour. NET/ROM and FlexNet both use
PID `0xCF`; G8BPQ advises against combining `B` (NET/ROM broadcasts) and
`F` on the same `MAP` entry.

### FlexNet on a KISS (RF) port

A KISS port has no `MAP` table, so its FlexNet neighbours are declared in
the port's own block. `FLEXNET=YES` enables FlexNet on the port;
`FLEXNETLINK=` names one neighbour and can be repeated:

```
PORT
  PORTNUM=5
  ID=2m FlexNet
  TYPE=ASYNC
  PROTOCOL=KISS
  COMPORT=/dev/ttyUSB0
  SPEED=9600
  FRACK=7000
  RETRIES=10
  MAXFRAME=4
  PACLEN=236
  DIGIFLAG=1              ; needed if the node carries transit
  FLEXNET=YES
  FLEXNETLINK=NODEB-2
  FLEXNETLINK=NODEC F+    ; per-link options as on a MAP entry
ENDPORT
```

- The node keeps each declared link up itself: it connects from its node
  callsign when there is no link, starts the FlexNet handshake as soon as
  the link is up, and reconnects after it drops. A neighbour that does
  not answer is retried after 60 s, then 120 s, doubling to 15 minutes,
  so a station that is off the air does not hold the channel. The
  neighbour may equally connect first; both ends can be configured the
  same way.
- Only the stations named by `FLEXNETLINK=` are FlexNet neighbours.
  Every other station on the channel is an ordinary AX.25 user.
- The [per-link routing options](#per-link-routing-options) work as on
  a `MAP` entry, written after the callsign (`FLEXNETLINK=NODEC F+`).
  `F+` suits a slow RF link that should only be a fallback; `F>` keeps
  a link private.
- Accepted on KISS ports (`TYPE=ASYNC`, serial or TCP KISS, and
  `TYPE=I2C`). On an AXUDP port use the `MAP` flag instead. A
  `FLEXNETLINK=` on any other port, or on a port without `FLEXNET=YES`,
  is reported on the console at start-up and ignored.
- Stock LinBPQ does not know these two keywords and prints
  `not recognised - Ignored` for each at start-up. That is expected: the
  FlexNet module reads them itself.
- By default routes are not advertised from one port to another, so a
  node with RF and AXUDP neighbours keeps the two apart. To route
  between them, see [`FLEXNETCROSSPORT`](#flexnetcrossport-yesno).
- The route cost of a slow link counts: the node adds its measured link
  time to each neighbour when choosing where to send a connection, so a
  cheap-looking route behind a slow RF hop is not preferred over a fast
  path.

### Per-link routing options

A suffix on the `F` flag sets the routing policy of that one link. The
options are the ones (X)Net defines for its FlexNet links:

| Option | Effect |
|---|---|
| `F` | Unchanged: the neighbour and everything behind it are advertised |
| `F-` | The neighbour itself is not advertised; what is behind it is |
| `F!` | Only the neighbour is advertised, nothing behind it |
| `F>` | Neither — for private or internal links |
| `F=` | As `!`, and this neighbour is sent no destinations except the node's own |
| `F+` | Everything learned over this link costs 2000 more (about 200 s) — for Internet tunnels |
| `F)` | The link is left out of `FL` for users who are not sysop (display only) |

```
MAP NODEB-2  192.0.2.10    UDP 10093  F      ; full transit
MAP NODEC    198.51.100.7  UDP 10093  F+     ; Internet tunnel, penalised
MAP NODED-1  10.0.0.5      UDP 10093  F>)    ; private link, not advertised, not listed
```

Rules:

- Options can be combined (`F+)`, `F>)`); `F-!` is the same as `F>`.
- Options only **narrow** what `FLEXNETTRANSIT` allows. With
  `FLEXNETTRANSIT NO` nothing learned from a neighbour is advertised
  anyway, so only `+` (which still raises the cost in `D` and steers the
  node's own route choice) and `)` have an effect.
- A destination hidden on one link but also reachable over another is
  still advertised, at the cost of the other path.
- `+` applies to routes as they are received: `D` shows the penalised
  cost, the node prefers an unpenalised path when it has one, and
  neighbours are told the penalised cost. It is never added to a
  withdrawal.
- An unknown option is reported on the console at start-up and the link
  comes up with default policy.
- On a KISS port the options follow the callsign of a `FLEXNETLINK=`
  line instead (see [above](#flexnet-on-a-kiss-rf-port)).
- Options are read with the port configuration, i.e. at start-up on
  LinBPQ. If the port's configuration is re-read while the node runs,
  the new options take effect within 5 seconds, and narrowing a link
  withdraws what it had advertised instead of leaving it to age out.
- Options govern what the node **advertises**. Path queries and
  connects that arrive anyway are still answered and carried.

### Recommended port and node settings

These values are in use on production nodes and were chosen to fix
specific link-stability problems:

```
OnlyVer2point0=1          ; node section: AX.25 2.0 only. FlexNet peers do not
                          ; support 2.2; (X)Net answers an XID with FRMR+DISC.

PORT
  ID=AXUDP
  DRIVER=BPQAXIP
  FRACK=3000
  RETRIES=25              ; ~75 s of patience; a few retries drops slow peers
  RESPTIME=10             ; short ack delay; FlexNet peers retry within ~100 ms
  MAXFRAME=5
  PACLEN=236
  DIGIFLAG=1              ; needed if the node carries transit (see below)
  CONFIG
    UDP 10093
    MAP NODEB-2 192.0.2.10 UDP 10093 F
ENDPORT

PORT
  ID=Telnet
  DRIVER=TELNET
  CONFIG
    DisconnectOnClose=0   ; see below
    ...
ENDPORT
```

**`DisconnectOnClose=0` on the Telnet port is important.** With `1`,
LinBPQ pauses its main loop for one second whenever a telnet session
closes. For that second no AX.25 frame is acknowledged, which is longer
than an (X)Net peer's retry budget (~0.6 s), and the peer drops the
FlexNet link. Every monitoring script or user that telnets in then
causes a link reset.

### Choosing a role

| Role | Directives | The node advertises |
|---|---|---|
| **Participant** (default) | none beyond the `F` flag | itself only |
| **Neighbour relay** | `FLEXNETTRANSIT YES` + `DIGIFLAG=1` | itself and its direct FlexNet neighbours |
| **Router** | `FLEXNETTRANSIT YES`, `FLEXNETL2TRANSIT YES`, `FLEXNETPATHFORWARD YES` + `DIGIFLAG=1` | every destination it has learned |

A node advertises exactly what it can carry: without L2 forwarding it can
deliver only to its direct neighbours, so that is all it re-advertises.
Leave a node as a participant unless you intend it to carry other
stations' traffic.

Example router configuration:

```
FLEXNETSSIDRANGE 0-8
FLEXNETTRANSIT YES
FLEXNETL2TRANSIT YES
FLEXNETPATHFORWARD YES
FLEXNETLT3BYTE YES
```

Set each role directive explicitly, even to its default, so the node's
role can be read from its configuration.

---

## Parameter reference

All directives: one per line, keyword case-insensitive, value separated
by a space, `=` or `:`. Boolean values: `YES`/`NO`, `ON`/`OFF`, `1`/`0`,
`TRUE`/`FALSE`.

| Directive | Default | Summary |
|---|---|---|
| `FLEXNETSSIDRANGE lo-hi` | node SSID only | Advertise a range of SSIDs of the node callsign |
| `FLEXNETLOCAL call ...` | none | Advertise application callsigns on a different base call |
| `FLEXNETLOCALAPPS YES\|NO` | `NO` | Advertise every such application callsign automatically |
| `FLEXNETEXTERNAL call port` | none | Advertise a plain AX.25 station reached on `port` |
| `FLEXNETTRANSIT YES\|NO` | `NO` | Re-advertise routes learned from neighbours |
| `FLEXNETL2TRANSIT YES\|NO` | `NO` | Carry multi-hop sessions by L2 chain rewriting |
| `FLEXNETCROSSPORT YES\|NO` | `NO` | Route between FlexNet neighbours on different ports |
| `FLEXNETPATHFORWARD YES\|NO` | `NO` | Relay other nodes' path queries |
| `FLEXNETPCFQUIESCE YES\|NO` | `YES` | Follow PC/Flexnet's route-exchange rule |
| `FLEXNETLT3BYTE YES\|NO` | `NO` | Accept 3-byte link-time frames |
| `FLEXNET=YES\|NO` | `NO` | In a KISS port block: FlexNet on this port |
| `FLEXNETLINK=call [F<opts>]` | none | In a KISS port block: a FlexNet neighbour on this port |

Port-level: the `F` flag on an AXUDP `MAP` entry, with optional
[per-link routing options](#per-link-routing-options); `FLEXNET=` and
`FLEXNETLINK=` in a KISS port's block; and `DIGIFLAG=1` for any node
with `FLEXNETTRANSIT YES`.

### `FLEXNET=YES|NO` (KISS port block)

Enables FlexNet on the KISS port whose `PORT` … `ENDPORT` block contains
it. Like every port keyword it is written `KEY=value`. Default `NO`. Without it, `FLEXNETLINK=` lines on that port are
ignored.

### `FLEXNETLINK=call [F<options>]` (KISS port block)

Declares one FlexNet neighbour on the port; repeat for more (up to 16 per
node). The node keeps the link to it up — see
[FlexNet on a KISS (RF) port](#flexnet-on-a-kiss-rf-port). The optional
second word takes the same options as the `F` flag of a `MAP` entry
(`F+`, `F>`, `F!)`, …); the leading `F` may be left out.

### `FLEXNETSSIDRANGE lo-hi`

```
NODECALL=NODEA
FLEXNETSSIDRANGE 0-8
APPLICATION 1,BBS,,NODEA-8,NODBBS,255
```

Advertises `NODEA 0-8` as one destination instead of the node SSID only,
and declares SSID 8 as the upper edge in the FlexNet handshake (peers
clamp the range to that value). Accepts `N-M`, `N M` or a single `N`;
0–15, `lo ≤ hi`.

The range only makes the SSIDs reachable. What answers them is LinBPQ's
own `APPLICATION` table: in the example `C NODEA-8` reaches the BBS,
`C NODEA` the node, and an SSID in the range with no application is
refused.

Only advertise SSIDs this node owns. If other nodes use SSIDs of the
same base callsign, a range covering them would claim their callsigns.

### `FLEXNETLOCAL call [call ...]` and `FLEXNETLOCALAPPS YES`

For applications whose callsign has a **different base call** from the
node, which `FLEXNETSSIDRANGE` cannot express:

```
NODECALL=NODEA
BBS=1
APPLICATION 1,FBB,,BBSX,NODBBS,255
APPLICATION 3,DX,ATTACH 5 7300,DXCL-2,DXCLUS,255

FLEXNETLOCAL BBSX DXCL-2       ; explicit; several per line, line repeatable
; or
FLEXNETLOCALAPPS YES           ; every APPLICATION callsign outside NODECALL's base
```

- Each callsign is advertised as its own destination at cost 1, in the
  same frame as the node's own record.
- Up to **16** callsigns; base call at most 6 characters; SSID 0–15. The
  SSID must match the `APPLICATION` line exactly (`DXCL-2` is not
  `DXCL`). Both forms can be combined; duplicates are merged.
- Use the explicit form when the node binds application callsigns you do
  not want visible on the FlexNet network.
- **Only callsigns an `APPLICATION` answers are advertised.** Anything
  else is reported at start-up (also on a silent build) and skipped,
  because peers would install a route to it and every connect would fail.
  A callsign on the node's own base belongs in `FLEXNETSSIDRANGE`.
- Requires **`BBS=1`** — with `BBS=0` LinBPQ ignores application
  callsigns on inbound connects (the node warns). If the FlexNet port has
  **`PERMITTEDAPPLS`**, it must include each advertised application.
- A callsign advertised at cost 1 is claimed network-wide. Check it is not
  in use anywhere — FlexNet (`D <call>` on a peer) **and** NET/ROM
  (`NODES`) — before advertising it.
- Removing a callsign later is slow to take effect elsewhere: PC/Flexnet
  peers keep advertising their copy until their next full route exchange
  (up to about 90 minutes), and a copy can circulate among distant nodes
  at a rising cost until it reaches infinity. Choose callsigns you intend
  to keep.

`FL` lists each configured callsign and whether it is advertised:

```
FlexNet Local calls  echo-skips=0
  BBSX       advertised
  DXCL-2     advertised  [from APPLICATION]
  XYZZY      NOT advertised - no APPLICATION
  NODEA-3    NOT advertised - NODECALL base, use FLEXNETSSIDRANGE
```

### `FLEXNETEXTERNAL call port`

Advertises a station that does **not** run FlexNet — a DX cluster, a
BBS, any AX.25 station — as a destination of this node, reached directly
on `port`. It is the equivalent of a static link on a dedicated FlexNet
router: the station keeps its own callsign and needs no configuration.

```
FLEXNETTRANSIT YES
FLEXNETL2TRANSIT YES

PORT
  PORTNUM=2
  DRIVER=BPQAXIP
  DIGIFLAG=1
  CONFIG
    UDP 10093
    MAP DXCL-6 192.0.2.20 UDP 10093      ; plain AX.25 station, no F flag
ENDPORT

FLEXNETEXTERNAL DXCL-6 2       ; one station per line
```

- Advertised at cost 1 in the node's own record, like a
  [local call](#flexnetlocal-call-call--and-flexnetlocalapps-yes); a path
  query for it is answered `asker, this node, station`.
- Connects arriving through the network are forwarded to the station on
  `port` at L2 — nothing terminates at this node — and the replies are
  carried back, to whichever port the connect came from. This works even
  with `FLEXNETCROSSPORT NO`: the port is named explicitly.
- `C DXCL-6` from the node's own command line connects to it on `port`.
- Requires **`FLEXNETTRANSIT YES`** and **`FLEXNETL2TRANSIT YES`**, and
  `DIGIFLAG=1` on the ports frames arrive on. On an AXUDP port the
  station needs a `MAP` entry (without `F`).
- The port must exist. A station on the node's own base call belongs in
  `FLEXNETSSIDRANGE`. Anything the node cannot carry is reported at
  start-up (also on a silent build) and not advertised.
- Shares the 16-entry table with `FLEXNETLOCAL`; listed in both, a
  callsign is treated as external.
- The node cannot tell whether the station is on the air: while it is
  off, connects to it fail as they would for any unreachable station.

`FL` lists it with the local calls (`DXCL-6  advertised  [external,
port 2]`), and `D` marks it with its port (`DXCL-6(p2)`).

### `FLEXNETTRANSIT YES|NO`

Default `NO`. With `YES` the node re-advertises destinations learned
from its neighbours to its other neighbours, so FlexNet peers on either
side of it can see each other through it.

- Scope depends on `FLEXNETL2TRANSIT`: without it, only **direct
  neighbours** are re-advertised; with it, every learned destination.
- Re-advertisement is change-driven: a record is sent when the cost the
  node would advertise to a peer has moved by at least 10 % (and at
  least 100 ms) since that peer was last told. Direct neighbours are
  re-offered every 120 s.
- Paced per peer: 1 record frame every 5 s (burst 2) to PC/Flexnet, every
  2 s (burst 4) to (X)Net-like peers. Several records share each frame.
- Split horizon, withdrawal of routes lost with a neighbour, a 90 s
  hold-down on withdrawn routes, and detection of routes whose cost keeps
  climbing (count-to-infinity) are built in.
- **Requires `DIGIFLAG=1` on the FlexNet port.** For a destination one
  hop beyond this node, (X)Net sends a digipeated SABM and expects the
  node to repeat it. With digipeating off, the node advertises routes it
  then refuses to carry.

### `FLEXNETL2TRANSIT YES|NO`

Default `NO`. Requires `FLEXNETTRANSIT YES`. Enables FlexNet L2
forwarding: for a session passing through the node to a destination that
is not adjacent, the node appends its next hop to the digipeater chain
on the way out and removes it on the way back, which is how FlexNet
routers carry multi-hop sessions (see [PROTOCOL_SPEC.md §10](PROTOCOL_SPEC.md)).
It also widens re-advertisement to every learned destination, since the
node can now carry them.

This rewrites other stations' frames, which is why it is a separate
switch. Built-in limits: a callsign already in the chain is never added,
the chain never exceeds 8 digipeaters or the port's `PORTMAXDIGIS`, only
a hop the node itself added is ever removed, the next hop is pinned for
the life of each session, and a frame that loops back is dropped.

### `FLEXNETCROSSPORT YES|NO`

Default `NO`. With `YES`, a node with FlexNet neighbours on more than one
port — typically RF (KISS) and AXUDP — routes between them:

- routes learned on one port are offered to neighbours on the others;
- L2 forwarding sends a frame out of the port its next hop is on, and
  carries every reply back to the port the connect came from;
- a station on a FlexNet port that connects through the node by
  digipeating (`C DEST via NODEA`) is routed to `DEST` across the
  network, as on a dedicated FlexNet router.

Requires `FLEXNETTRANSIT YES` and `FLEXNETL2TRANSIT YES` (without them
it has no effect) and `DIGIFLAG=1` on every port involved; a port with
`DIGIFLAG=0` never sends frames across. Per-link options still apply —
a link marked `F>` stays private.

Turning it on changes what the node's neighbours learn, on both sides:
each now sees the other port's destinations through this node. With it
`NO`, a next hop on another port is declined rather than sent out of the
wrong port.

`FL` shows the state and the number of frames sent across ports
(`cross-port ON: frames=…`).

### `FLEXNETPATHFORWARD YES|NO`

Default `NO`. Requires `FLEXNETTRANSIT YES`. When a neighbour asks for
the path to a destination the node cannot complete itself, the node
inserts its next hop and passes the query on, instead of answering from
its own cache or not at all. Peers then see complete routes through the
node, including paths longer than a single answer could describe. Adds
the node's callsign to other stations' path queries and costs one frame
per hop.

### `FLEXNETPCFQUIESCE YES|NO`

Default `YES` — leave it on. After answering a PC/Flexnet neighbour's
full-table request (`3+`), the node sends that neighbour no further route
records until its next request. PC/Flexnet disconnects a link that sends
more than two record frames after that exchange; with this off, a link to
PC/Flexnet ends shortly after every full-table request (roughly every
75–90 minutes). Applies to PC/Flexnet neighbours only (identified by
their keepalive format). The cost: a PC/Flexnet neighbour's view of this
node refreshes once per request instead of continuously.

### `FLEXNETLT3BYTE YES|NO`

Default `NO`. With `YES`, a three-byte link-time frame (`"1n" CR`, a
single-digit value — what (X)Net sends on a fast link) is processed as a
link-time measurement instead of being ignored as a status frame. The
node then measures fast links correctly and answers those frames. The
default is kept for compatibility with existing deployments; `YES` is
recommended and is used on the production router configuration.

### Compile-time switches

| Switch | Default | Effect |
|---|---|---|
| `FLEXNET_PROD` | 0 | 1 = silent console (see [Build](#4-build)) |
| `FLEXNET_DEBUG` | 0 | 1 = per-frame trace to console and `/tmp/flexnet_axudp.log` (the `flexdebug` target) |

---

## Console commands

### `D` — FlexNet destinations

```
D                all destinations
D NODEB-2        one destination, with its path if known
D NODE*          prefix match       D *EB   suffix     D *OD*  substring
D < NODEB-2      only destinations routed via this neighbour (base call matches any SSID)
D !              only destinations with a cached path     D ?   only without
D /COST          sort by cost        D /CALL   by callsign     D /AGE   freshest path first
D BBSX           for a FLEXNETLOCAL callsign: "local call of this node"
```

Modifiers combine. Output is three columns, (X)Net style:

```
NODEB  2-2      3!      NODEC  0-0     17!      DEST   0-0     42
```

`!` marks a destination whose full path is cached (from a path reply);
the detail view then shows the hop chain. The full listing ends with the
node's own local callsigns, if any.

### `FL` — FlexNet links

One row per FlexNet neighbour: port, status, link time, keepalives,
uptime, routes learned. Status is `CONNECTED` (established, routes
sent), `INIT` (established, routes not yet sent) or `PENDING` (not yet
established).

With `FLEXNETTRANSIT YES`, a second section per peer: family
(PC/Flexnet or (X)Net-like), learned / direct / advertised counts, queue
depth, token credit. With `FLEXNETL2TRANSIT YES`: forwarding counters
`extended`, `contracted`, `declined` and circuit counters `circuits`,
`repinned`, `looped`, `evicted`. With `FLEXNETPATHFORWARD YES`:
`forwarded`, `declined`, `replies-relayed`. With local callsigns
configured: the local-calls section shown above. With
[per-link routing options](#per-link-routing-options) on any link: a
`FlexNet Link options` section listing each link's options. A link with
`)` appears in neither section to a user who is not sysop.

With any `FLEXNETLINK=` neighbour configured, a `FlexNet KISS links`
section lists each one with its port, options, the number of times the
node has opened the link, and its state: `up` (FlexNet running), `L2 up`
(link up, handshake starting), `connecting`, `closing`, `down` (no link;
reconnecting shortly), `no answer` (the last attempt failed; waiting to
retry) or `unusable` (rejected at start-up — see the console).

A large `declined` count is normal — it counts every frame left to plain
digipeating, which is correct for adjacent destinations. Counters reset
when the node restarts; compare differences over time, not absolute
values.

### `V` — version

Shows the LinBPQ version and the FlexNet module version.

---

## Operating notes

- **Allow a few minutes after a restart** before judging routing or
  transit. Neighbours' tables take time to converge; a connect attempted
  during convergence can follow a path that no longer exists.
- **A failed connect with no route line** in a peer's `D <call>` output
  usually means the path query died somewhere upstream, not that this
  node answered wrongly.
- **PC/Flexnet recycles AXIP links** on a fixed lifetime of about
  90 minutes (5445 s). The link re-establishes by itself and the node
  re-sends its routes; this is normal.
- **Test transit from the network, not only from the node.** Whether
  peers route through the node depends on its cost relative to their
  existing paths: the same configuration can carry nothing on one node
  and much of the network on another.
- **Monitoring can cause what it measures.** Frequent telnet polling of a
  node without `DisconnectOnClose=0` causes link resets (see
  [Configuration](#recommended-port-and-node-settings)).

## Known limitations

- FlexNet over KISS has been tested between LinBPQ nodes only, not yet
  with an (X)Net or PC/Flexnet neighbour on RF.
- Routing between ports (`FLEXNETCROSSPORT`) is new in v2.6: tested
  live between an AXUDP and a KISS port (LinBPQ on the KISS side), not
  yet with an (X)Net or PC/Flexnet neighbour on RF.
- Sessions are forwarded end to end; the node does not acknowledge
  frames per hop as (X)Net does, so users through it see the whole
  path's round-trip time.
- Fixed table sizes: up to 8 FlexNet neighbours, 2000 destinations,
  128 concurrent transit circuits, 16 local callsigns.
- LinBPQ has no command to re-read an AXIP port's configuration while
  running, and `FLEXNET=` / `FLEXNETLINK=` are read once at start-up, so
  changing a link or its options needs a restart.

---

## Repository layout

| File | Contents |
|---|---|
| `FlexNetCode.c` | The FlexNet implementation (new file) |
| `flexnet_l3.c`, `flexnet_l3.h` | NET/ROM L3 envelope builders/parsers used by the L3RTT layer (new files) |
| `L2Code.c` | Modified: PID `0xCE`/`0xCF` dispatch, FlexNet SABM acceptance, L2 forwarding and local-call hooks |
| `Cmd.c` | Modified: `D`, `FL`, `V`; FlexNet routing for `C` |
| `bpqaxip.c` | Modified: `F` flag and per-link options on `MAP` entries; KISS neighbours share the same lookup |
| `asmstrucs.h` | Modified: FlexNet fields and declarations |
| `makefile` | Modified: FlexNet objects, `flexdebug` target |
| `patches/` | Stand-alone LinBPQ fixes, also for stock LinBPQ; the overlay already includes them (see `patches/README.md`) |
| `win/` | Windows build: cross-compiles `LinBPQ.exe` with mingw-w64 (see `win/README.md`) |
| `tools/unit/` | Unit tests (see `tools/unit/README.md`) |
| `tools/` | Capture and analysis scripts used during development |
| `research/` | Engineering archive: captures and analyses behind the implementation |
| `sync-and-build.sh` | Developer helper: copy the overlay to a remote build tree and build |

| Document | For |
|---|---|
| [README.md](README.md) | Installing and configuring |
| [RELEASE_NOTES.md](RELEASE_NOTES.md) | What changed in each release |
| [ROADMAP.md](ROADMAP.md) | Planned work |
| [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md) | The FlexNet protocol, for other implementers |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Building from source for development, tests, conventions |

Related: [`flexnetd`](https://github.com/onionuser79/flexnetd), a FlexNet
daemon for Linux AX.25 / URONode by the same author;
[`g8bpq/LinBPQ`](https://github.com/g8bpq/LinBPQ), upstream LinBPQ.
