# Roadmap

**Current release: v2.4.0** (LinBPQ 6.0.25.41). What has shipped is in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

The node is a correct FlexNet participant and, when configured to, a
router for everything it can carry, with per-link routing policy. What
is left is a few refinements.

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
| Transit between FlexNet neighbours on **different ports** | Circuits are keyed on one port today | A deployment with FlexNet neighbours on two ports |
| **Per-hop acknowledgement** of forwarded sessions, as (X)Net does | Users through the node would no longer pay the whole path's round trip in every retry timer | Needs a capture of frame loss through an (X)Net path first |
| NET/ROM L4 (CREQ) transit towards LinBPQ FlexNet peers | Not used by (X)Net or PC/Flexnet; may matter between LinBPQ nodes | Demand |
| `FLEXPROBE <call>` sysop command | Force a path query on demand instead of waiting for the background cycle | — |
| Show an unknown-frame counter in `FL` | At-a-glance check that the parser keeps up with peers | — |
| Rotate `/tmp/flexnet_axudp.log` | Debug builds grow the log without limit | — |
| FlexNet over RF (KISS) ports | Only AXUDP has been tested | A tester with an RF FlexNet neighbour |
| RMNC/Flexnet interoperability | Untested | A tester with an RMNC neighbour |

## Open protocol questions

Listed in [PROTOCOL_SPEC.md §14](PROTOCOL_SPEC.md#14-open-questions).
None of them blocks interoperation.

## Out of scope

- Replacing the dedicated FlexNet routers — (X)Net, PC/Flexnet,
  RMNC/Flexnet.
- A shared protocol library with the sibling `flexnetd` project.
