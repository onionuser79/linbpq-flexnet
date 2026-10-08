# Roadmap

**Current release: v2.6.0** (LinBPQ 6.0.25.41). What has shipped is in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

The node is a correct FlexNet participant and, when configured to, a
router for everything it can carry, with per-link routing policy. What
is left is a few refinements.

## In progress — Windows build

`win/` cross-compiles `LinBPQ.exe` with FlexNet; see
[win/README.md](win/README.md). Since v2.6.0 it runs a live node with
FlexNet links over AXUDP and over KISS on serial COM ports, to (X)Net
and LinBPQ neighbours. Still to do before it is called supported: a
soak comparable to the Linux releases, and an autostart recipe.

## Done: v2.6 — routing between ports, external stations

Shipped in v2.6.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md). Verified
live on a node with an (X)Net neighbour on two RF KISS ports and FlexNet
neighbours on AXUDP, including an external station: connects and path
queries from every neighbour, through the node, across ports.

## Done: v2.5 — FlexNet over KISS (RF) ports

Shipped in v2.5.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md) and the
README's *FlexNet on a KISS (RF) port*. Tested between two LinBPQ nodes
on a KISS link, and since v2.6.0 live with an (X)Net neighbour on two
RF channels (1200 and 9600 Bd; link times of a few seconds at 1200 Bd).
Still open: a PC/Flexnet neighbour on KISS, and whether (X)Net opens a
KISS link itself when the node does not (here the node opened it).

## Done: v2.4 — per-link routing options

Shipped in v2.4.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md) and the
README's *Per-link routing options*. Still open, and worth a look on a
live (X)Net node before relying on interoperating details:

- whether (X)Net withdraws routes or just stops advertising them when a
  link's option changes (this node withdraws);
- whether (X)Net accepts more than one option character on a link (this
  node does).

Settled from a live (X)Net node's configuration and tables: (X)Net
applies `+` to routes **as received**, as this node does. A node with a
direct `+` link to a neighbour (link time 100 ms) routed to that
neighbour through another path at a cost of 2.8 s, and advertised that
path onward; the penalised link carried no destinations.

## Later — candidates, not scheduled

| Item | Why | Trigger |
|---|---|---|
| **Per-hop acknowledgement** of forwarded sessions, as (X)Net does | Users through the node would no longer pay the whole path's round trip in every retry timer | Needs a capture of frame loss through an (X)Net path first |
| NET/ROM L4 (CREQ) transit towards LinBPQ FlexNet peers | Not used by (X)Net or PC/Flexnet; may matter between LinBPQ nodes | Demand |
| `FLEXPROBE <call>` sysop command | Force a path query on demand instead of waiting for the background cycle | — |
| Show an unknown-frame counter in `FL` | At-a-glance check that the parser keeps up with peers | — |
| Rotate `/tmp/flexnet_axudp.log` | Debug builds grow the log without limit | — |
| FlexNet over KISS against PC/Flexnet on RF | Tested against LinBPQ and (X)Net only | A tester with an RF PC/Flexnet neighbour |
| RMNC/Flexnet interoperability | Untested | A tester with an RMNC neighbour |

## Open protocol questions

Listed in [PROTOCOL_SPEC.md §14](PROTOCOL_SPEC.md#14-open-questions).
None of them blocks interoperation.

## Out of scope

- Replacing the dedicated FlexNet routers — (X)Net, PC/Flexnet,
  RMNC/Flexnet.
- A shared protocol library with the sibling `flexnetd` project.
