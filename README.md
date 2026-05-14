# LinBPQ FlexNet Integration (v1.9.7)

Native FlexNet CE/CF routing protocol support added to LinBPQ so a
BPQ node can participate in a FlexNet packet-radio network alongside
its existing NET/ROM stack.

> **⚠ Scope — read first.** linbpq-flexnet runs as a **leaf node**
> in a FlexNet mesh. It is **not** a full FlexNet AX.25 router and is
> **not** a replacement for `xnet` / `PC/FlexNet` / `flexnetd`. It
> does not re-advertise other neighbours' destinations into the
> cloud, and it does not act as an L2 digipeat transit for FlexNet
> traffic. Two FlexNet peers sitting on either side of your linbpq
> node remain mutually invisible at the FlexNet routing layer — that
> is by design and confirmed by the v1.9.7 release notes.
>
> If you need a true FlexNet transit router, run `xnet`, `PC/FlexNet`,
> or `flexnetd` (sibling project from the same author). linbpq-flexnet
> lets your existing LinBPQ node *participate* in the network as a
> well-behaved leaf.

Author: IW2OHX | Based on LinBPQ 6.0.25.23 by G8BPQ.

---

## What it does (high level)

- **FlexNet leaf participation.** Your BPQ node initiates FlexNet
  protocol exchanges (init, keepalive, link time, compact routing)
  with each neighbour you flag in `bpq32.cfg`, and gets its own
  callsign onto the FlexNet network's distance-vector tables.
- **Bidirectional connectivity.** FlexNet users can connect *to*
  your BPQ node via digipeated SABM. BPQ users can connect *out*
  to any FlexNet destination they see in their `D` table.
- **Node identity preservation.** Outbound connects carry your BPQ
  node's callsign as the originating digipeater
  (`USER → DEST via MYCALL* NEIGHBOUR`), so remote nodes see the
  connection as coming from *your* node, not from your upstream
  FlexNet neighbour.
- **L3RTT counters with link-down guard.** Tick-based `c1`–`c4`
  exchanges so neighbours can measure round-trip time and processing
  delay. When local routing has zero reachable destinations, replies
  carry `c3=0 c4=0` so peers route around us.
- **`D` command** — FlexNet destination table with wildcard search
  (`D`, `D <call>`, `D IW*`, `D *MLB`, `D < <neighbour>`). Detail
  view renders the cached path hop chain; list view marks resolved
  destinations with `!`. Rendered in three-column layout, xnet-style.
- **CE type-6 / type-7 path discovery.** `FlexNet_Timer` fires one
  PATH_REQ every 60 s round-robin through the destination table;
  replies populate an on-disk path cache (`flexnet_path_cache.dat`)
  with 4 h TTL.
- **Multi-FlexNet-neighbour** with cost-based routing (v1.9.2). A
  destination is associated with the neighbour that reports the
  best cost; the outgoing connect goes through that neighbour.
- **`C <flexnet-neighbour>`** works correctly (v1.9.5). For direct
  FlexNet neighbours we send a plain L2 SABM without the
  self-referential digi chain; the L2 layer's `pid=0xCF` dispatch
  falls through to NetROM L4 when the payload is not L3RTT so
  CACK/INFO from peers reach the originating user session.

---

## Testing conditions

This integration has been verified **only against `xnet` peers** on
HAMNET (specifically PCFlexNet variants running V3.3h on European
HAMNET nodes). It has not been integration-tested against:

- `PC/FlexNet` on DOS
- `flexnetd` (Linux daemon, sibling project) — protocol is the same
  but no live cross-node test has been run
- Older xnet versions before V3.3h
- Native AX.25 RF links (only AXUDP over the HAMNET tunnel has been
  exercised)

Live test results from 2026-05-14 (under v1.9.5; same numbers apply
to v1.9.7 since the revert only removed transit re-advertisement):

| Test | Pass rate |
|------|-----------|
| BPQ-13 → 18 low-cost FlexNet cloud destinations | 17/19 = **89 %** |
| BPQ-14 (xnet) → IW2OHX-13 (us, single target, 21 attempts) | 21/21 = **100 %** sub-second |

Tests are driven by `tools/human_connect.py` against the live FlexNet
cloud with human-paced (25–70 s) connect attempts.

---

## Repository layout

| File | Description |
|------|-------------|
| `FlexNetCode.c`   | New file: FlexNet protocol module. CE/CF dispatch, `D`/`FL` commands, L3RTT probes, multi-neighbour routing, connection acceptance, on-disk path cache. |
| `asmstrucs.h`     | Modified: `FlexNetFlag`, `FlexNetLink`, path-cache fields, public function declarations. |
| `bpqaxip.c`       | Modified: `F` flag parsing on MAP entries, FlexNet relay acceptance, AXIP byte-6 SSID normalisation for ARP lookups. |
| `L2Code.c`        | Modified: `pid=0xCE`/`pid=0xCF` dispatch with auto-init, FlexNet inbound-SABM acceptance, v1.9.5 fall-through to NetROM L4 for non-L3RTT CF frames. |
| `Cmd.c`           | Modified: `D` and `FL` command registration; FlexNet route lookup in `C` connect handler; v1.9.5 no-digi when target == neighbour. |
| `flexnet_l3.{c,h}`| Reserved for a future shared protocol module with `flexnetd`. Currently compiled but not linked from `FlexNetCode.c`. |
| `makefile`        | Modified: builds `FlexNetCode.o` and `flexnet_l3.o`. |
| `ROADMAP.md`      | Two GA items (`CE-UNKNOWN` investigation + SSID-range mapping). Out-of-scope items deliberately removed. |
| `QUICK_WINS.md`   | Opportunistic improvements — cherry-pick freely. |
| `AGENTS.md`       | Methodology for coding agents (human or AI) picking up work. |
| `sync-and-build.sh` | Dev convenience: rsync this repo to a remote BPQ build host and run `make`. |

---

## Build guide (Raspberry Pi / Linux)

Tested on Raspberry Pi OS (aarch64) with LinBPQ 6.0.25.23.

### Step 1: Install build dependencies

```bash
sudo apt update
sudo apt install -y git gcc make libconfig-dev zlib1g-dev \
    libpcap-dev libminiupnpc-dev libjansson-dev libpaho-mqtt-dev
```

### Step 2: Clone the LinBPQ source from G8BPQ

```bash
cd ~
git clone https://github.com/g8bpq/LinBPQ.git linbpq-build
cd linbpq-build
```

If you already have the source tree, pull the latest:

```bash
cd ~/linbpq-build && git pull
```

### Step 3: Clone this repository

```bash
cd /tmp
rm -rf linbpq-flexnet
git clone https://github.com/onionuser79/linbpq-flexnet.git
```

### Step 4: Overlay the modified files into the LinBPQ source tree

```bash
cd ~/linbpq-build

# Backup originals (first time only)
for f in asmstrucs.h bpqaxip.c L2Code.c Cmd.c makefile; do
    [ -f "$f.orig" ] || cp "$f" "$f.orig"
done

# Copy modified + new files
for f in FlexNetCode.c asmstrucs.h bpqaxip.c L2Code.c Cmd.c \
         flexnet_l3.c flexnet_l3.h makefile; do
    cp /tmp/linbpq-flexnet/$f .
done
```

### Step 5: Build

```bash
make
```

The binary is `./linbpq`.

### Step 6: Install (back up first)

```bash
sudo cp /usr/local/bin/linbpq /usr/local/bin/linbpq.bak.$(date +%Y%m%d)
sudo cp linbpq /usr/local/bin/linbpq
sudo systemctl restart linbpq
```

### Rollback

```bash
sudo cp /usr/local/bin/linbpq.bak.YYYYMMDD /usr/local/bin/linbpq
sudo systemctl restart linbpq
```

### Verifying the build

After restart, telnet into the BPQ console and run `V`:

```
BPQBOL:IW2OHX-13} Version 6.0.25.23 (64 bit) and FlexNet v1.9.7
```

The `and FlexNet v1.9.7` suffix confirms the FlexNet module is loaded.

---

## Configuration

### Enable FlexNet on an AXUDP link

In `bpq32.cfg`, add `F` to the AXUDP MAP entry for the FlexNet neighbour:

```
MAP IW2OHX-14 44.134.24.4 UDP 10093 F
```

> **Do not combine `B` and `F` flags on the same MAP entry.** NET/ROM
> (B flag) and FlexNet (F flag) both use `PID=0xCF` for L3 connected
> traffic. A given link must be one protocol or the other. If you
> need both NET/ROM and FlexNet connectivity to the same node, use
> separate MAP entries on different BPQ ports.
> (Thanks to John G8BPQ for this clarification.)

The `F` flag enables the FlexNet CE/CF protocol on that link. The node
will:

1. Exchange init handshakes and keepalives.
2. Measure link quality via link-time round-trip exchange.
3. Advertise its own `MYCALL` and receive the neighbour's
   destination table.
4. Accept incoming FlexNet user connections (digipeated SABMs that
   reach us).
5. Auto-route outgoing `C` commands through FlexNet when the
   destination is in the local table.

### AXUDP RESPTIME tuning

Set a low `RESPTIME` (ack delay) on the AXUDP port to prevent L2 REJ
frames. FlexNet peers retransmit within ~100 ms; BPQ's default
`RESPTIME` is too high for this rhythm.

In `bpq32.cfg` port configuration:

```
RESPTIME=1
```

### Node identity

The FlexNet identity is taken from `NODECALL` in `bpq32.cfg`. For
example, `NODECALL=IW2OHX-13` advertises `IW2OHX (13-13)` to the
FlexNet network. Only the specific SSID is advertised — no range.
(SSID-range application binding is a v2.0 GA item, see `ROADMAP.md`.)

---

## Console commands

### `D` — FlexNet destinations

```
D                    show all FlexNet destinations
D IW2OHX-14          show details for one specific destination
D IW*                prefix match
D *MLB               suffix match
D *HU*               substring match
D < IW2OHX-13        show destinations whose advertised-via is IW2OHX-13
```

Output is xnet-style three-column layout, 24-char cells:

```
HB9ON  2-2      40!     HB9ON  3-3      44!     HB9ON  4-4      40!
HB9ON  6-6      40!     HB9ON  8-8      44!     HB9ON  10-10     5!
…
```

The trailing `!` on a row indicates the destination's full hop chain
is cached (populated by CE type-7 PATH_REP). Without `!`, the
detail-view of that destination uses the local-walk fallback.

### `FL` — FlexNet link status

Shows active FlexNet sessions with neighbour callsign, link-time
quality, link uptime, advertised route count, and per-neighbour stats.

### `V` — version

Shows BPQ version and the FlexNet module version (e.g.
`FlexNet v1.9.7`) so you can confirm what's running.

---

## Known limitations

- **Leaf only.** No transit-role re-advertisement of other
  neighbours' destinations. Two FlexNet peers sitting behind your
  node will not see each other through us. The v1.9.4 attempt at
  transit re-advertisement broke AX.25 V2 reciprocity on the return
  path and was reverted in v1.9.7.
- **Integration-tested against `xnet` only.** Other FlexNet
  implementations may behave differently — particularly around
  inbound CF handling and SABM digipeat conventions.
- **Direct connects to a FlexNet neighbour** depend on the peer
  having a NetROM L4 stack that handles CACK on a FlexNet-flagged
  L2 link. With unpatched `xnet` peers, `C <flexnet-neighbour>` from
  the BPQ console may produce no output even though the underlying
  L3 handshake completes — the peer does not surface the CACK to
  its L4. The mirror fix on the peer side is needed for full
  end-to-end success.
- **Path cache fixed-size.** Currently 64 destinations.

---

## See also

- `ROADMAP.md` — v2.0 GA outstanding items (just two).
- `QUICK_WINS.md` — opportunistic improvements not blocking GA.
- `AGENTS.md` — methodology and conventions for coding agents
  picking up work on this repo.
- [`flexnetd`](https://github.com/onionuser79/flexnetd) — sibling
  project; FlexNet routing daemon for URONode. Same author.
- [`g8bpq/LinBPQ`](https://github.com/g8bpq/LinBPQ) — upstream
  LinBPQ source by John Wiseman, G8BPQ.
