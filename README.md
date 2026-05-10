# LinBPQ FlexNet Integration v1.3.3

Native FlexNet CE/CF routing protocol support for LinBPQ with
**node identity preservation** in outbound connections and **real
L3RTT counter exchange** with proper NetRom L3 envelope.

Enables BPQ nodes to participate in FlexNet routing alongside their
existing NET/ROM capability. Bidirectional connectivity: FlexNet users
can connect to the BPQ node, BPQ users can connect to any FlexNet
destination, and the BPQ node's callsign is preserved as the originating
digipeater across the FlexNet mesh.

Author: IW2OHX | Based on LinBPQ 6.0.25.23 by G8BPQ | April 2026

## Features

- **MAP F flag** — enable FlexNet on any AXUDP link
- **Node identity preservation (v1.2)** — outbound connections carry the BPQ
  node's callsign as the first digipeater (`USER → DEST via MYCALL* NEIGHBOR`).
  Remote nodes see the connection as originating from your BPQ node, not from
  the upstream FlexNet neighbor
- **L3RTT c1–c4 counters (v1.3)** — proper tick-based counters at 10 ms
  granularity from raw `CLOCK_MONOTONIC`. Replies carry `c3` (recv tick) and
  `c4` (send tick) so peers can compute RTT and our processing delay
- **Link-down guard (v1.3)** — when our local routing table has zero reachable
  destinations, replies go out with `c3=0 c4=0`; peers interpret this as
  "we're up but our link is down" and route around us
- **NetRom L3 INFO envelope on L3RTT replies (v1.3.3)** — replies are wrapped
  in a NetRom L3 INFO frame mirroring the probe's `IN`/`ID` (so xnet's
  pending-probe table binds the reply) and the probe's low TTL (xnet's L3RTT
  class marker). Without the wrap, bare L3RTT replies are parsed but never
  bound, and `xnet's L* shows the link stuck`. See `V1.3_DESIGN.md` for the
  three-iteration resolution including the dead-end `dest=L3RTT pseudo`
  forwarding-loop discovery
- **D command** — FlexNet destination table with wildcard search and L3RTT path tracing
- **FL command** — active FlexNet link status with timing, quality, uptime
- **V command** — shows FlexNet module version (`linbpq-1.3`) alongside BPQ version
- **Automatic routing** — `c <callsign>` auto-routes through FlexNet when destination is in table
- **Incoming connections** — FlexNet users can connect to the BPQ node via digipeated SABM
- **CE protocol** — init handshake, keepalive, link time, compact routing, token exchange
- **CF protocol** — L3RTT probe/reply for round-trip measurement and path discovery

## Repository Contents

This repository contains the **full modified source files** based on
LinBPQ 6.0.25.23. No patches needed -- just copy and build.

| File | Description |
|------|-------------|
| `FlexNetCode.c` | FlexNet protocol module: CE/CF, D/FL commands, L3RTT probes, routing, connection acceptance |
| `asmstrucs.h` | Modified: FlexNetFlag, FlexNetLink, path cache fields, function declarations |
| `bpqaxip.c` | Modified: F flag MAP parsing, FlexNet relay acceptance, digipeater bit fix |
| `L2Code.c` | Modified: PID 0xCE/0xCF dispatch with auto-init, FlexNet connection acceptance |
| `Cmd.c` | Modified: D and FL command registration, FlexNet route lookup in connect handler |
| `makefile` | Modified: FlexNetCode.o added to build |
| `FEASIBILITY.md` | Architecture and feasibility study |

---

## Build Guide (Raspberry Pi / Linux)

Tested on Raspberry Pi OS (aarch64) with LinBPQ 6.0.25.23.

### Step 1: Install build dependencies

```bash
sudo apt update
sudo apt install -y git gcc make libconfig-dev zlib1g-dev \
     libpcap-dev libminiupnpc-dev libjansson-dev \
     libpaho-mqtt-dev
```

### Step 2: Clone the LinBPQ source from G8BPQ

```bash
cd ~
git clone https://github.com/g8bpq/LinBPQ.git linbpq-build
cd linbpq-build
```

If you already have the source tree, pull the latest:

```bash
cd ~/linbpq-build
git pull
```

### Step 3: Download FlexNet integration files

```bash
cd /tmp
rm -rf linbpq-flexnet
git clone https://github.com/onionuser79/linbpq-flexnet.git
```

### Step 4: Copy all modified files into the LinBPQ source tree

```bash
cd ~/linbpq-build

# Backup originals (first time only)
for f in FlexNetCode.c asmstrucs.h bpqaxip.c L2Code.c Cmd.c makefile; do
    [ -f "$f.orig" ] || cp "$f" "$f.orig" 2>/dev/null
done

# Copy modified files
cp /tmp/linbpq-flexnet/FlexNetCode.c .
cp /tmp/linbpq-flexnet/asmstrucs.h .
cp /tmp/linbpq-flexnet/bpqaxip.c .
cp /tmp/linbpq-flexnet/L2Code.c .
cp /tmp/linbpq-flexnet/Cmd.c .
cp /tmp/linbpq-flexnet/makefile .
```

### Step 5: Build

```bash
make
```

The binary is `./linbpq`.

### Step 6: Install (backup first!)

```bash
sudo cp /usr/local/bin/linbpq /usr/local/bin/linbpq.bak.$(date +%Y%m%d)
sudo cp linbpq /usr/local/bin/linbpq
sudo systemctl restart linbpq
```

### Rollback

```bash
sudo cp /usr/local/bin/linbpq.bak.$(date +%Y%m%d) /usr/local/bin/linbpq
sudo systemctl restart linbpq
```

---

## Configuration

### Enable FlexNet on an AXUDP link

In `bpq32.cfg`, add `F` to the AXUDP MAP entry for the FlexNet neighbor:

```
MAP IW2OHX-14 44.134.24.4 UDP 10093 F
```

**Important: do not combine B and F flags on the same MAP entry.**
NET/ROM (B flag) and FlexNet (F flag) both use PID 0xCF for connected-mode
L3 frames. A given link must be used for one protocol or the other, not
both. If you need both NET/ROM and FlexNet connectivity to the same node,
use separate MAP entries on different ports.

(Thanks to John G8BPQ for this clarification.)

The `F` flag enables FlexNet CE/CF protocol on that link. The node will:
1. Exchange init handshakes and keepalives
2. Measure link quality via link time exchange
3. Advertise its routes and receive the neighbor's routing table
4. Accept incoming FlexNet user connections
5. Auto-route outgoing connections to FlexNet destinations

### AXUDP RESPTIME tuning (important)

Set a low RESPTIME (ack delay) on the AXUDP port to prevent L2 REJ
frames. FlexNet nodes retransmit within ~100ms; BPQ's default RESPTIME
is too high for this.

In `bpq32.cfg` port configuration:

```
RESPTIME=1
```

### Node identity

The FlexNet identity is taken from `NODECALL` in `bpq32.cfg`. For example,
`NODECALL=IW2OHX-13` advertises `IW2OHX (13-13) RTT=1` to the FlexNet
network. Only the specific SSID is advertised (no range).

---

## Node Commands

### D -- FlexNet Destinations

```
D              show all FlexNet destinations
D IW*          prefix wildcard
D *MLB         suffix wildcard
D *HU*         substring wildcard
D W4MLB-1      specific destination detail + L3RTT path probe
```

**List mode** (wildcard or no filter):
```
FlexNet Destinations:
Dest     SSID    RTT Via
-------- ----- ----- -----------
IR3UHU   0-15      2 IW2OHX-14
HB9ON    0-15      5 IW2OHX-14
...
193 destinations
```

**Detail mode** (specific callsign):
```
*** W4MLB  (1-1) T=26
*** route: IW2OHX-14 IR3UHU-2 IW8PGT-15 HB9ON-15 VE3MCH-8 W4MLB-1
```

On first query, an L3RTT probe is sent to discover the full path.
Re-issue the D command to see the cached result. Cache expires after 120s.

### FL -- FlexNet Links

```
FL             show active FlexNet links
```

Output:
```
FlexNet Links:
Link         Port  Status     LT     KA     Uptime      Routes
------------ ----  ---------  ------ -----  ----------  ------
IW2OHX-14    3     CONNECTED  60s    42     01:15:30    193
```

| Field | Description |
|-------|-------------|
| Link | Neighbor callsign |
| Port | BPQ port number of the AXUDP link |
| Status | CONNECTED (fully operational), INIT (handshake in progress), PENDING (waiting for peer) |
| LT | Link Time -- peer's reported delay in seconds. Initial value is 60s (FlexNet default before convergence). Lower is better. Typical AXUDP: <1s |
| KA | Keepalive count -- number of keepalive frames received from peer. Increments every ~21 seconds. Useful for verifying link liveness |
| Uptime | Session duration since link establishment |
| Routes | Number of reachable FlexNet destinations learned from this neighbor |

### C -- Connect via FlexNet

```
c ir5s         auto-routes through FlexNet if destination is in table
c ir1uaw-10    connects to specific SSID
```

When you type `c <callsign>` and the destination is not a NET/ROM node,
the FlexNet destination table is checked automatically. If found, the
connection is routed through the FlexNet port as an L2 connect.

---

## Enabling Debug Mode

For protocol troubleshooting, rebuild with verbose debug output:

```bash
make CFLAGS+="-DFLEXNET_DEBUG=1"
```

Or edit `FlexNetCode.c` and uncomment:
```c
#define FLEXNET_DEBUG 1
```

When enabled:
- **Console output** -- every CE/CF frame, keepalive, route entry, link time exchange, L3RTT probe, and connection decision is logged to stdout via Consoleprintf
- **Traffic log** -- bidirectional AXUDP frame trace with decoded AX.25 headers written to `/tmp/flexnet_axudp.log`

To disable, rebuild without the flag (default is `FLEXNET_DEBUG 0`).

---

## Protocol Reference

FlexNet uses two PIDs over connected-mode AX.25 I-frames:

| PID | Protocol | Purpose |
|-----|----------|---------|
| 0xCE | FlexNet native | Init, keepalive, link time, routing, token exchange |
| 0xCF | L3RTT / data | Round-trip measurement, path discovery |

### Link establishment sequence

```
BPQ node                          FlexNet neighbor (XNET)
    |                                  |
    |  SABM ───────────────────────>   |
    |  <─────────────────────── UA     |
    |  CE init (our SSID) ─────────>   |
    |  CE keepalive ───────────────>   |
    |  <──────────── CE init (peer)    |
    |  <──────────── CE link time      |
    |  CE link time (reply) ───────>   |
    |  3+/routes/3- ──────────────>    |
    |  <────────── compact records     |
    |  <────────── 3-                  |
    |         [CONNECTED]              |
    |                                  |
    |    keepalive cycle (~21s)        |
    |  <──────────── CE keepalive      |
    |  CE keepalive (echo) ────────>   |
    |  CE link time ──────────────>    |
```

### Compact routing record format

```
3CALLSIGN(6) SSID_LO(1) SSID_HI(1) RTT(digits) ' ' \r
```

SSID encoding: char = 0x30 + ssid (0-15).
RTT in 100ms ticks. RTT >= 60000 = unreachable.

---

## Status

**v1.2.0 -- Identity-preserving release (2026-04-22)**

Verified live with IW2OHX-14 (XNET) as FlexNet neighbor. Bidirectional
connectivity confirmed with multiple stations across the FlexNet network,
and the BPQ node's callsign now appears correctly as the originating
digipeater at the far end.

**Tested live connections:**
- **Outgoing:** IW2OHX-13 connects to IR5S (Altopascio), IR1UAW-10 (Tigullio), IGATE
- **Incoming:** IW7BIA connects from IW2OHX-12 via FlexNet to IW2OHX-13
- **Identity verified:** at IR5S, `u` command shows `IR5S>IW7EAS v IQ5KG-7 IW2OHX-13`
  -- IW2OHX-13 (our node), not IW2OHX-14 (the upstream FlexNet neighbor)
- **Route table:** ~193 FlexNet destinations reachable

### How identity preservation works (v1.2)

flexnetd on Linux relies on the kernel's `AX25_IAMDIGI` socket option to
let a daemon act as both a digipeater and an endpoint for the same frame.
LinBPQ has its own internal L2 stack with no equivalent flag, so v1.2
implements the mechanism directly in two places:

1. **Outbound (`Cmd.c`)** -- when dispatching an outgoing FlexNet connect,
   the SABM is built with a two-digi chain `MYCALL* NEIGHBOR` (H-bit set on
   MYCALL, clear on NEIGHBOR). MYCALL is our NODECALL from `bpq32.cfg`,
   NEIGHBOR is the FlexNet peer (e.g. IW2OHX-14). This mirrors the pattern
   used by flexnetd and preserves our identity across the mesh.

2. **Inbound (`L2Code.c`)** -- when the remote replies with a mirrored digi
   list (`NEIGHBOR* MYCALL`), the standard digipeat logic would retransmit
   the frame back to the air because MYCALL is the next unmarked digi. The
   v1.2 fix intercepts this case: if an active LINK already exists for
   `(ORIGIN, DEST, Port)` we mark our H-bit and consume the frame locally,
   letting the L2 state machine complete the UA / I / RR exchange.

### Known Limitations

- **PID 0xCF conflict** -- NET/ROM and FlexNet both use PID 0xCF for connected-mode L3 frames. A MAP entry must use either `B` (NET/ROM) or `F` (FlexNet), not both. Using both on the same link will cause protocol confusion. (Per G8BPQ guidance.)
- **Single SSID** -- only the NODECALL SSID is advertised (no configurable range)
- **Single neighbor** -- currently supports one FlexNet neighbor. Multi-neighbor support is planned.
- **INP3 L3RTT** -- INP3 also uses L3RTT frames but with a different format. FlexNet L3RTT and INP3 L3RTT are not interchangeable.
- **RESPTIME tuning required** -- AXUDP ports need `RESPTIME=1` in bpq32.cfg to prevent L2 REJ frames (FlexNet nodes retransmit within ~100ms)
- **L3RTT c1-c4 counters** -- v1.2 still echoes L3RTT frames as text only, no proper tick-based c1-c4 counters yet. Full counter semantics are planned for v1.3 (see `ROADMAP.md`).

---

## Changelog

- **v1.2.0** (2026-04-22) -- **Node identity preservation.** Outbound SABM
  now carries `MYCALL* NEIGHBOR` digi chain, and `L2Code.c` intercepts the
  returning frames so they reach the local LINK state machine instead of
  being retransmitted as a digipeat. Verified end-to-end: remote nodes see
  the connection as originating from our BPQ node, not from the FlexNet
  neighbor. Closes P4 #18 in ROADMAP.md.
- **v1.1.0** (2026-04-12) -- Session reconnect handling (dedup by port),
  FL command converging link time, D command wildcards, PID 0xCF documentation
  correction per G8BPQ feedback (B and F flags cannot coexist on same MAP entry)
- **v1.0.1** (2026-04-12) -- Outgoing connect fixes: Port/PORT variables, digipeater insertion, digi list termination
- **v1.0** (2026-04-12) -- Production release: FLEXNET_DEBUG flag, FL display fix, full documentation
- **v0.9** -- Debug builds: Consoleprintf trace, AXUDP traffic logger to /tmp/flexnet_axudp.log
- **v0.8** -- Incoming connections: bpqaxip FlexNet relay acceptance, L2 FlexNet_CheckIncoming, digi bit fix in send path
- **v0.7** -- Outgoing connections: FlexNet_FindRoute auto-routing in connect handler
- **v0.6** -- FL command, D wildcard/detail/path, L3RTT probe mechanism
- **v0.5** -- Console debug (Consoleprintf), MYCALL as FlexNet identity, init SSID fix
- **v0.4** -- Auto-init FlexNet session on first CE frame (bootstrap fix)
- **v0.3** -- Initial implementation: CE/CF protocol, D command, route exchange
