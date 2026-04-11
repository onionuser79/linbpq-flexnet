# LinBPQ FlexNet Integration

FlexNet CE/CF routing protocol support for LinBPQ (pilinbpq).

Adds native FlexNet routing to LinBPQ via AXUDP MAP entries, enabling BPQ
nodes to participate in FlexNet routing alongside their existing NET/ROM
capability.

Author: IW2OHX | Based on LinBPQ 6.0.25.23 by G8BPQ | April 2026

## Features

- **MAP F flag**: Enable FlexNet on any AXUDP link
  ```
  MAP IW2OHX-14 44.134.24.4 UDP 10093 B F
  ```
- **D command**: Display FlexNet destination table from the BPQ node prompt
- **CE protocol**: Init handshake, keepalive, link time, compact routing records, token exchange
- **CF protocol**: L3RTT probe/reply for round-trip measurement
- **Automatic**: Route advertisement, link quality convergence, periodic keepalive

## Repository Contents

| File | Description |
|------|-------------|
| `FlexNetCode.c` | New source file — complete FlexNet protocol module |
| `asmstrucs.h.patch` | Patch: FlexNet data structures, session state, dest table |
| `bpqaxip.c.patch` | Patch: F flag parsing in MAP entries |
| `L2Code.c.patch` | Patch: PID=0xCE/0xCF dispatch to FlexNet handlers |
| `Cmd.c.patch` | Patch: D command registration |
| `makefile.patch` | Patch: Add FlexNetCode.o to build |
| `FEASIBILITY.md` | Feasibility study with full architecture analysis |

## How to Apply

```bash
# Clone LinBPQ source
git clone https://github.com/g8bpq/LinBPQ.git
cd LinBPQ

# Copy new file
cp /path/to/linbpq-flexnet/FlexNetCode.c .

# Apply patches
patch -p1 < /path/to/linbpq-flexnet/asmstrucs.h.patch
patch -p1 < /path/to/linbpq-flexnet/bpqaxip.c.patch
patch -p1 < /path/to/linbpq-flexnet/L2Code.c.patch
patch -p1 < /path/to/linbpq-flexnet/Cmd.c.patch
patch -p1 < /path/to/linbpq-flexnet/makefile.patch

# Build
make
```

## Configuration

In your BPQ port config, add `F` to any AXUDP MAP entry to enable FlexNet:

```
MAP IW2OHX-14 44.134.24.4 UDP 10093 B F
```

The `F` flag tells LinBPQ to run the FlexNet CE/CF protocol on that link
alongside normal AX.25/NET/ROM traffic.

## Protocol Implementation

Based on the proven flexnetd v0.3.0 protocol stack. See
[FEASIBILITY.md](FEASIBILITY.md) for the full architecture analysis.

## Status

**Work in progress** — initial implementation, needs testing with live
FlexNet nodes.
