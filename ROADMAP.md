# Roadmap

**Current release: v2.3.0** (LinBPQ 6.0.25.41). What has shipped is in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

The node is a correct FlexNet participant and, when configured to, a
router for everything it can carry. What is left is routing *policy* and
a few refinements.

## Next patch release

Fold [patches/0001-l2-appl-alias](patches/0001-l2-appl-alias/) into the
`L2Code.c` overlay (verified on a live node): an AX.25
connect to an aliased APPLICATION (such as a Telnet `ATTACH` to a DX
cluster) should run the whole alias, as NET/ROM connects already do.

## Next: v2.4 — per-link routing options

Today transit is all or nothing for the whole node. v2.4 adds per-link
policy, using the options the (X)Net manual defines for its links
(§4.3.24.3.1), as a suffix on the `F` flag of an AXUDP `MAP` entry:

| Option | Effect |
|---|---|
| `F` | Unchanged: the neighbour and everything behind it are advertised |
| `F-` | Do not advertise the neighbour itself; advertise what is behind it |
| `F>` | Advertise neither — for private or internal links |
| `F!` | Advertise the neighbour only, not what is behind it |
| `F=` | As `!`, and send this neighbour no destinations except our own |
| `F+` | Add 2000 (≈ 200 s) to the cost of everything learned over this link — for Internet tunnels |
| `F)` | Hide the link from non-sysop `FL` output (display only) |

Example:

```
MAP NODEB-2  192.0.2.10    UDP 10093  F      ; full transit
MAP NODEC    198.51.100.7  UDP 10093  F+     ; Internet tunnel, penalised
MAP NODED-1  10.0.0.5      UDP 10093  F>     ; private link, not advertised
```

Rules: options only narrow what `FLEXNETTRANSIT` allows, never widen it;
an unknown option is reported and the link comes up with default policy;
adding `>` to a live link withdraws what that link had advertised rather
than leaving it to age out.

Before building, three behaviours will be checked on a live (X)Net node:
whether changing an option makes (X)Net withdraw routes or just stop
advertising them; whether `+` applies to routes as received or as
re-advertised; and whether (X)Net accepts more than one option
character.

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
