# LinBPQ FlexNet Integration v1.9.3 (pre-GA)

Native FlexNet CE/CF routing protocol support for LinBPQ with
**node identity preservation**, **real L3RTT counter exchange** with
proper NetRom L3 envelope, and **CE type-6/7 path discovery** so the
`D <call>` command shows full FlexNet hop chains.

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
- **D command** — FlexNet destination table with wildcard search.
  List view has a `Path` column with `!` marking destinations whose
  full hop chain is cached (from CE type-7 PATH_REP). Detail view
  (`D <call>`) renders the cached chain (e.g.
  `*** route: IW2OHX-13 IW2OHX-14 IR3UHU-2 IQ5KG-7 IR5S`) or a
  local-walk fallback when no full path is on file yet
- **FL command** — active FlexNet link status with timing, quality, uptime
- **V command** — shows FlexNet module version (`linbpq-1.9`) alongside BPQ version
- **CE type-6/7 path discovery (v1.9)** — `FlexNet_Timer` fires one
  PATH_REQ every 60 s round-robin through `FlexNetDests[]`; replies
  populate an in-memory cache with 4 h TTL so the full round-robin
  cycle (~3 h at 60 s × ~190 dests) fits inside the cache window
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
- **Single SSID** -- only the NODECALL SSID is advertised (no configurable range
  with per-SSID "application" mapping). Planned for v2.x.
- **(removed in v1.9.2)** ~~Single FlexNet neighbour~~ — v1.9.2 supports
  multiple FlexNet neighbours, on the same BPQ port or across different
  ports. Sessions are identified per L2 link, not per port. A proactive
  CE-init scan bootstraps the FlexNet handshake when an L2 link to an
  F-flagged MAP entry is up but the peer hasn't sent CE init yet. Each
  destination is attributed to the neighbour that announced the lowest
  RTT, so routing decisions follow the cheaper path automatically (the
  user sees no Via column — routing is transparent).
- **(removed in v1.9.1)** ~~Path cache is in-memory only~~ — v1.9.1
  added an on-disk path cache (`flexnet_path_cache.dat` in `linbpq`'s
  working directory). Entries are saved every 5 min when at least one
  row has changed, and reloaded on startup if they are less than 5 h
  old. After a restart, cached destinations render in `D <call>`
  immediately; round-robin re-probing refreshes them in the background.
- **INP3 L3RTT** -- INP3 also uses L3RTT frames but with a different format. FlexNet L3RTT and INP3 L3RTT are not interchangeable.
- **RESPTIME tuning required** -- AXUDP ports need `RESPTIME=1` in bpq32.cfg to prevent L2 REJ frames (FlexNet nodes retransmit within ~100ms)
- **L3RTT c1-c4 counters** -- v1.2 still echoes L3RTT frames as text only, no proper tick-based c1-c4 counters yet. Full counter semantics are planned for v1.3 (see `ROADMAP.md`).

---

## Changelog

- **v1.9.3** (2026-05-13) -- **AXIP routing + session-table fixes
  uncovered by the v1.9.2 multi-neighbour soak.** Three closely
  related bugs were silently making outbound FlexNet connects fail
  at a 98% rate with two F-flagged MAPs configured (and inbound
  UA replies traverse the wrong neighbour):
    1. `bpqaxip.c` AXIP TX path normalised the resolved next-hop
       SSID byte with `& 0x7e`, clearing the H-bit and end-of-list
       bit but never restoring the AX.25 reserved bits (`0x60`)
       that `convtoax25` sets when building `arp_table` entries
       from `MAP` lines. The `memcmp` on byte 6 of the callsign
       therefore failed for every digi whose SSID byte came from a
       wire-derived `LINK->LINKCALL`. The fallback ("no ARP match
       → first F-flagged entry") then routed the frame to
       `arp[0]` blindly. With one FlexNet neighbour this masked
       the bug; with two it sent ~half the traffic to the wrong
       UDP endpoint. Fix: `axcall[6] = (axcall[6] & 0x7e) | 0x60;`.
    2. `L2Code.c` auto-bootstrapped a FlexNet session on **any**
       incoming CE-PID frame, including from non-FlexNet peers
       (e.g. a user accidentally relayed by URONode). `FL` then
       listed ghost rows like `IW7ER-15`. Fix: gate the auto-init
       on `FlexNet_IsPeerFlexNetMapped(LINK->LINKCALL, port)`.
    3. `FlexNet_InitSession` matched existing sessions only by
       LINK pointer. When an L2 link dropped and reconnected
       with a fresh LINK slot, a new session was allocated and
       the previous one stayed in the table with a stale pointer
       — visible in `FL` as an empty-callsign row. Fix: also
       match by neighbour callsign on the same port; and add a
       per-tick reaper in `FlexNet_Timer` that drops any session
       whose LINK is gone, dead, or has lost its callsign,
       demoting any destinations attributed to that slot.
- **v1.9.2** (2026-05-12) -- **Multi-FlexNet-neighbour support.**
  Sessions are now keyed by L2 link pointer instead of BPQ port number,
  so multiple FlexNet neighbours can coexist on the same port (or on
  different ports). A proactive CE-init scan runs every 30 s from
  `FlexNet_Timer`: any connected L2 link whose remote peer is in a
  FlexNet-flagged MAP entry but has no FlexNet session yet gets a CE
  init handshake. The destination table now records, per dest, which
  neighbour announced the lowest RTT; outgoing connects and CE type-6
  path probes route through that neighbour's session. Routing is
  transparent to the user — there is no Via column in `D`, only the
  RTT and the `!` cached-path marker. `FL` shows one row per session
  with the count of destinations actually routed through it. Closes
  v2.x item #3.
- **v1.9.1** (2026-05-12) -- **On-disk path cache.** Persists
  `path_hops[] + path_updated` to `flexnet_path_cache.dat` in `linbpq`'s
  CWD. Periodic save (every 5 min if any cache row changed since the
  last save) bounds disk writes. Reload on startup keeps entries that
  are less than 5 h old; older rows are skipped. Eliminates the ~3 h
  post-restart re-probe warm-up. Closes v2.x item #1.
- **v1.9** (2026-05-12) -- **Pre-GA release.** CE type-6/7 path discovery
  end-to-end. PATH_REQ wire format includes the next-hop neighbour
  (`<origin> <next_hop> <target>`) so peers forward and reply with the
  full accumulated chain. `D <call>` detail view now renders the cached
  hop chain; D list view marks resolved entries with `!` in the `Path`
  column. Path cache TTL is 4 h to cover a full round-robin cycle.
  Local-walk fallback retained for destinations not yet probed.
  Background path probing runs every 60 s. Roadmap items #7+#8+#10
  fully working; #9 partial (TTL fix shipped, capacity resize pending);
  all P1 closed.
- **v1.3.7** (2026-05-11) -- Proactive keepalive threshold relaxed from
  180 s → 300 s. Closes all P1 items.
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
