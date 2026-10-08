# Roadmap

**Current release: v2.5.0** (LinBPQ 6.0.25.41). What has shipped is in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

The node is a correct FlexNet participant and, when configured to, a
router for everything it can carry, with per-link routing policy. What
is left is a few refinements.

## In progress — v2.6: routing between ports, external stations

Built as v2.6.0-rc1 and covered by unit tests (`test_crossport`,
`test_external`); see [RELEASE_NOTES.md](RELEASE_NOTES.md). Routing
between an AXUDP and a KISS port verified live, both directions, on a
whole session. Next: a node with an (X)Net neighbour on RF and FlexNet
neighbours on AXUDP, including an external station, then release.

## In progress — Windows build

`win/` cross-compiles `LinBPQ.exe` with FlexNet (unreleased; see
[win/README.md](win/README.md)). It builds and starts with FlexNet
initialised. Still to do before it is called supported: AXUDP and KISS
(serial COM port) FlexNet links to live peers on Windows, and a soak
comparable to the Linux releases.

## Done: v2.5 — FlexNet over KISS (RF) ports

Shipped in v2.5.0; see [RELEASE_NOTES.md](RELEASE_NOTES.md) and the
README's *FlexNet on a KISS (RF) port*. Tested between two LinBPQ nodes
on a KISS link. Still open: a real RF channel, and an (X)Net or
PC/Flexnet neighbour on KISS — in particular whether they open the link
themselves, as they do on AXUDP, and how their link time behaves at
1200 Bd.

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
| FlexNet over KISS against (X)Net / PC/Flexnet on RF | v2.5 is tested LinBPQ to LinBPQ only | A tester with an RF FlexNet neighbour |
| RMNC/Flexnet interoperability | Untested | A tester with an RMNC neighbour |

## Open protocol questions

Listed in [PROTOCOL_SPEC.md §14](PROTOCOL_SPEC.md#14-open-questions).
None of them blocks interoperation.

## Out of scope

- Replacing the dedicated FlexNet routers — (X)Net, PC/Flexnet,
  RMNC/Flexnet.
- A shared protocol library with the sibling `flexnetd` project.
