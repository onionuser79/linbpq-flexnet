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
| `FlexNetCode.c` | New source file — self-contained FlexNet protocol module |
| `bpqaxip.c.patch` | Patch: F flag parsing in MAP entries |
| `L2Code.c.patch` | Patch: PID=0xCE/0xCF dispatch to FlexNet handlers |
| `Cmd.c.patch` | Patch: D command registration |
| `makefile.patch` | Patch: Add FlexNetCode.o to build |
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

### Step 2: Clone LinBPQ source

```bash
cd ~
git clone https://github.com/g8bpq/LinBPQ.git linbpq-build
cd linbpq-build
```

### Step 3: Download FlexNet integration files

```bash
git clone https://github.com/onionuser79/linbpq-flexnet.git /tmp/flexnet
```

### Step 4: Copy the new FlexNet module

```bash
cp /tmp/flexnet/FlexNetCode.c .
```

`FlexNetCode.c` is fully self-contained — all FlexNet constants, structs,
and protocol logic are defined inside the file. No header modifications
needed for compilation.

### Step 5: Patch asmstrucs.h (two one-liners)

Add `FlexNetFlag` to the MAP entry struct and `FlexNetLink` to the link
table struct. These are the only structural changes to existing LinBPQ
code:

```bash
sed -i '/time_t LastHeard;/a\\tBOOL FlexNetFlag;' asmstrucs.h
sed -i '/int apiSeq;/a\\tBOOL FlexNetLink;' asmstrucs.h
```

Verify:
```bash
grep FlexNetFlag asmstrucs.h   # should show: BOOL FlexNetFlag;
grep FlexNetLink asmstrucs.h   # should show: BOOL FlexNetLink;
```

### Step 6: Patch bpqaxip.c (F flag in MAP parsing)

Add the `F` flag parsing. After the block that handles the `B` flag
(search for `_stricmp(p_UDP,"B")`), add:

```bash
sed -i '/_stricmp(p_UDP,"B")/,/continue;/{
/continue;/a\
\t\t\tif (_stricmp(p_UDP,"F") == 0)\
\t\t\t{\
\t\t\t\tflexflag =TRUE;\
\t\t\t\tp_UDP = strtok(NULL, " \\t\\n\\r");\
\t\t\t\tcontinue;\
\t\t\t}
}' bpqaxip.c
```

Also add the `flexflag` variable declaration and initialization:

```bash
sed -i '/int bcflag;/a\\tint flexflag;' bpqaxip.c
sed -i '/bcflag=0;/a\\t\tflexflag=0;' bpqaxip.c
```

And after the `add_arp_entry()` call in the MAP section, set the flag:

```bash
sed -i '/add_arp_entry(PORT, axcall/a\
\t\t\tif (flexflag)\
\t\t\t{\
\t\t\t\tif (PORT->arp_table_len > 0)\
\t\t\t\t\tPORT->arp_table[PORT->arp_table_len - 1].FlexNetFlag = TRUE;\
\t\t\t}' bpqaxip.c
```

### Step 7: Patch L2Code.c (PID dispatch)

Add FlexNet PID handling before the `default:` case in the
`PROC_I_FRAME()` function's switch statement:

```bash
sed -i '/^	default:$/i\
\tcase 0xce:\
\n\t\tif (LINK->FlexNetLink)\
\t\t{\
\t\t\tmemmove(\&Msg->PID, Info, Length);\
\t\t\tBuffer->LENGTH = Length + MSGHDDRLEN;\
\t\t\tFlexNet_ProcessCE(LINK, Buffer);\
\t\t\tLINK->L2ACKREQ = PORT->PORTT2;\
\t\t\tLINK->KILLTIMER = 0;\
\t\t\treturn;\
\t\t}\
\t\tgoto flexnet_default;\
\n\tcase 0xcf:\
\n\t\tif (LINK->FlexNetLink)\
\t\t{\
\t\t\tmemmove(\&Msg->PID, Info, Length);\
\t\t\tBuffer->LENGTH = Length + MSGHDDRLEN;\
\t\t\tFlexNet_ProcessCF(LINK, Buffer);\
\t\t\tLINK->L2ACKREQ = PORT->PORTT2;\
\t\t\tLINK->KILLTIMER = 0;\
\t\t\treturn;\
\t\t}\
\t\tgoto flexnet_default;\
' L2Code.c
```

Also add the `flexnet_default:` label after `default:`:

```bash
sed -i 's/^	default:$/	default:\n\tflexnet_default:/' L2Code.c
```

### Step 8: Patch Cmd.c (D command)

Add the FlexNet destinations command to the command table:

```bash
sed -i '/"ROUTES      "/a\\t"DEST        ",1,FlexNet_CmdDest,0,' Cmd.c
```

### Step 9: Patch makefile

Add FlexNetCode.o to the build:

```bash
sed -i 's/NETROMTCP.o$/NETROMTCP.o \\\n FlexNetCode.o/' makefile
```

### Step 10: Build

```bash
make
```

The binary is `./linbpq`.

### Step 11: Install (backup first!)

```bash
# Back up your current binary
sudo cp /usr/local/bin/linbpq /usr/local/bin/linbpq.bak

# Install
sudo cp linbpq /usr/local/bin/linbpq

# Restart
sudo systemctl restart linbpq
```

---

## Quick Build (all steps in one script)

For convenience, here's all the patching in one script:

```bash
#!/bin/bash
# build-linbpq-flexnet.sh — apply FlexNet patches and build LinBPQ
set -e

cd ~/linbpq-build

# Get FlexNetCode.c
curl -sL https://raw.githubusercontent.com/onionuser79/linbpq-flexnet/main/FlexNetCode.c -o FlexNetCode.c

# Patch asmstrucs.h
grep -q FlexNetFlag asmstrucs.h || sed -i '/time_t LastHeard;/a\\tBOOL FlexNetFlag;' asmstrucs.h
grep -q FlexNetLink asmstrucs.h || sed -i '/int apiSeq;/a\\tBOOL FlexNetLink;' asmstrucs.h

# Patch bpqaxip.c (F flag)
grep -q flexflag bpqaxip.c || {
    sed -i '/int bcflag;/a\\tint flexflag;' bpqaxip.c
    sed -i '/bcflag=0;/a\\t\tflexflag=0;' bpqaxip.c
    sed -i '/_stricmp(p_UDP,"B")/,/continue;/{
/continue;/a\
\t\t\tif (_stricmp(p_UDP,"F") == 0)\n\t\t\t{\n\t\t\t\tflexflag =TRUE;\n\t\t\t\tp_UDP = strtok(NULL, " \\t\\n\\r");\n\t\t\t\tcontinue;\n\t\t\t}
}' bpqaxip.c
    sed -i '/add_arp_entry(PORT, axcall/a\
\t\t\tif (flexflag) { if (PORT->arp_table_len > 0) PORT->arp_table[PORT->arp_table_len - 1].FlexNetFlag = TRUE; }' bpqaxip.c
}

# Patch L2Code.c (PID dispatch) — only if not already patched
grep -q flexnet_default L2Code.c || {
    sed -i 's/^	default:$/	default:\n\tflexnet_default:/' L2Code.c
}

# Patch Cmd.c (D command)
grep -q FlexNet_CmdDest Cmd.c || sed -i '/"ROUTES      "/a\\t"DEST        ",1,FlexNet_CmdDest,0,' Cmd.c

# Patch makefile
grep -q FlexNetCode makefile || sed -i 's/NETROMTCP.o$/NETROMTCP.o \\\n FlexNetCode.o/' makefile

echo "Patches applied. Building..."
make

echo "Done! Binary: ./linbpq"
```

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
