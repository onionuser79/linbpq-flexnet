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
- **D command**: FlexNet destination table with wildcard search and L3RTT path tracing
- **FL command**: Active FlexNet link status with timing, quality, uptime
- **CE protocol**: Init handshake, keepalive, link time, compact routing records, token exchange
- **CF protocol**: L3RTT probe/reply for round-trip measurement and path discovery
- **Automatic**: Route advertisement, link quality convergence, periodic keepalive

## Repository Contents

This repository contains the **full modified source files** based on
LinBPQ 6.0.25.23. No sed patches needed — just copy and build.

| File | Description |
|------|-------------|
| `FlexNetCode.c` | FlexNet protocol module (CE/CF, D/FL commands, L3RTT probes) |
| `asmstrucs.h` | Modified header: FlexNetFlag, FlexNetLink, path cache fields |
| `bpqaxip.c` | Modified: F flag parsing in MAP entries |
| `L2Code.c` | Modified: PID=0xCE/0xCF dispatch to FlexNet handlers |
| `Cmd.c` | Modified: D and FL command registration |
| `makefile` | Modified: FlexNetCode.o added to build |
| `FEASIBILITY.md` | Feasibility study with full architecture analysis |

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

### Step 5: Verify files are in place

```bash
# FlexNet module should be ~1250 lines
wc -l FlexNetCode.c

# Check FL command registered
grep FlexNet_CmdLinks Cmd.c

# Check L3RTT path cache fields
grep path_hops asmstrucs.h

# Check FlexNetCode.o in build
grep FlexNetCode makefile

# Check F flag parsing
grep flexflag bpqaxip.c

# Check CE/CF PID dispatch
grep flexnet_default L2Code.c
```

All six checks should produce output.

### Step 6: Build

```bash
make clean
make
```

The binary is `./linbpq`.

### Step 7: Install (backup first!)

```bash
# Back up your current running binary
sudo cp /usr/local/bin/linbpq /usr/local/bin/linbpq.bak.$(date +%Y%m%d)

# Install
sudo cp linbpq /usr/local/bin/linbpq

# Restart
sudo systemctl restart linbpq
```

### Step 8: Test the new commands

From the BPQ node prompt:

```
FL                  show active FlexNet link(s)
D                   full destination table
D IW*               prefix wildcard search
D *MLB              suffix wildcard search
D *HU*              substring wildcard search
D IW2OHX            specific destination detail
D W4MLB-1           specific with SSID + L3RTT path probe
D W4MLB-1           re-issue to see cached path
```

### Rollback (if needed)

```bash
sudo cp /usr/local/bin/linbpq.bak.$(date +%Y%m%d) /usr/local/bin/linbpq
sudo systemctl restart linbpq
```

---

## Quick Build (all steps in one script)

```bash
#!/bin/bash
# build-linbpq-flexnet.sh — build LinBPQ with FlexNet integration
set -e

SRCDIR=~/linbpq-build

# Clone or update LinBPQ source
if [ ! -d "$SRCDIR" ]; then
    git clone https://github.com/g8bpq/LinBPQ.git "$SRCDIR"
fi

# Get FlexNet integration files
cd /tmp
rm -rf linbpq-flexnet
git clone https://github.com/onionuser79/linbpq-flexnet.git

# Copy all modified files
cd "$SRCDIR"
for f in FlexNetCode.c asmstrucs.h bpqaxip.c L2Code.c Cmd.c makefile; do
    [ -f "$f.orig" ] || cp "$f" "$f.orig" 2>/dev/null
    cp /tmp/linbpq-flexnet/$f .
done

echo "Building..."
make clean
make

echo "Done! Binary: $SRCDIR/linbpq"
echo "Install with: sudo cp linbpq /usr/local/bin/linbpq"
```

---

## Updating an Existing Installation

If you already have a working FlexNet-patched LinBPQ and want to
update to the latest version:

```bash
# Pull latest FlexNet integration
cd /tmp
rm -rf linbpq-flexnet
git clone https://github.com/onionuser79/linbpq-flexnet.git

# Copy updated files
cd ~/linbpq-build
cp /tmp/linbpq-flexnet/FlexNetCode.c .
cp /tmp/linbpq-flexnet/asmstrucs.h .
cp /tmp/linbpq-flexnet/Cmd.c .

# Rebuild
make clean
make

# Backup and install
sudo cp /usr/local/bin/linbpq /usr/local/bin/linbpq.bak.$(date +%Y%m%d)
sudo cp linbpq /usr/local/bin/linbpq
sudo systemctl restart linbpq
```

Only `FlexNetCode.c`, `asmstrucs.h`, and `Cmd.c` changed in this update.
The other files (`bpqaxip.c`, `L2Code.c`, `makefile`) are unchanged from
the previous version.

---

## Configuration

In your BPQ port config (`bpq32.cfg`), add `F` to any AXUDP MAP entry:

```
MAP IW2OHX-14 44.134.24.4 UDP 10093 B F
```

The `F` flag enables FlexNet CE/CF protocol on that link. The node will:
1. Exchange init handshakes and keepalives
2. Measure link quality via link time exchange
3. Advertise its routes and receive the neighbor's routing table
4. Respond to L3RTT probes
5. Send L3RTT probes for path discovery on demand

### Node Commands

**D** — FlexNet destinations:
```
D              show all FlexNet destinations
D IW*          prefix wildcard (all calls starting with IW)
D *MLB         suffix wildcard
D *HU*         substring wildcard
D W4MLB-1      specific destination detail + L3RTT path probe
```

Specific destination query output:
```
*** W4MLB  (1-1) T=26
*** route: IW2OHX-14 IR3UHU-2 IW8PGT-15 HB9ON-15 VE3MCH-8 W4MLB-1
```
On first query, an L3RTT probe is sent; re-issue D to see the cached path.
Path cache expires after 120 seconds.

**FL** — FlexNet link status:
```
FL             show active FlexNet links with timing/quality
```
Output:
```
FlexNet Links:
Link         Port  Status     LT(ms) KA     Uptime      Routes
------------ ----  ---------  ------ -----  ----------  ------
IW2OHX-14    1     CONNECTED  200    1234   2h 15m      198
```

## Protocol Implementation

Based on the proven flexnetd v0.3.0 protocol stack by IW2OHX.
See [FEASIBILITY.md](FEASIBILITY.md) for the full architecture analysis.

## Status

**Work in progress** — compiles successfully on Raspberry Pi,
needs testing with live FlexNet nodes. L3RTT probe format may
need adjustment for XNET compatibility during live testing.
