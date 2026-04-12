# Feasibility Study: FlexNet CE/CF Protocol Integration into LinBPQ

Author: IW2OHX | Date: April 2026

---

## 1. Objective

Integrate the FlexNet CE/CF routing protocol into LinBPQ (pilinbpq),
enabling BPQ nodes to participate in FlexNet routing alongside their
existing NET/ROM capability.

**User-facing changes:**

1. **MAP flag**: Add `F` parameter to enable FlexNet on AXUDP links:
   ```
   MAP IW2OHX-14 44.134.24.4 UDP 10093 B F
   ```

2. **D command**: Display FlexNet destination table from the BPQ node prompt:
   ```
   IW2OHX-1:BOLNET> D
   FlexNet Destinations:
   DB0AAT    0-9        7   via IW2OHX-14
   DK0WUE    0-13       2   via IW2OHX-14
   IW2OHX    3-3        1   via IW2OHX-14
   ...
   ```

**Scope**: AXUDP links only (no KISS/RF in this phase).

---

## 2. LinBPQ Source Analysis

**Source**: LinBPQ 6.0.25.23 by G8BPQ (github.com/g8bpq/LinBPQ)

### 2.1 AXUDP / MAP Architecture

MAP entries are parsed in `bpqaxip.c:2216-2345`. Each MAP line creates
an `arp_table_entry` struct (defined in `asmstrucs.h:1379-1421`) with
callsign, IP address, UDP port, and flags.

The `B` (broadcast) flag is parsed at line 2324:
```c
if (_stricmp(p_UDP, "B") == 0)
    bcflag = TRUE;
```

Adding an `F` (FlexNet) flag follows the identical pattern. The flag is
stored in the `arp_table_entry` struct and passed through `add_arp_entry()`.

The AXUDP driver (`bpqaxip.c:ExtProc`) receives raw AX.25 frames via
UDP, verifies CRC, and passes them to the L2 layer with PID intact.
No PID filtering occurs in the driver — CE (0xCE) and CF (0xCF) frames
will flow through transparently.

### 2.2 L2 PID Dispatch

Connected-mode I-frames are processed in `L2Code.c:PROC_I_FRAME()`
(line 2723). The PID is extracted at line 2765 and dispatched via a
switch statement (line 2767):

```c
PID = EOA[2];
switch(PID) {
    case 0xf2: // compressed intermediate
    case 0xf1: // compressed last
    case 0xcc: // IP
    case 0xcd: // ARP
    case 0x08: // NOS fragmented IP
    default:   // ALL OTHER PIDs → LINK->RX_Q
}
```

PID=0xCE is currently unhandled and falls to the `default` case, which
queues the frame to `LINK->RX_Q` with PID preserved. Adding
`case 0xce:` before `default` intercepts FlexNet frames cleanly.

**PID=0xCF conflict**: ~~Originally assessed as no conflict~~ — **CORRECTION
(per G8BPQ)**: NET/ROM uses PID 0xCF for ALL connected-mode L3 frames,
not just UI broadcasts. Both NET/ROM and FlexNet use 0xCF on connected
links. **A given link must be FlexNet (F) or NET/ROM (B), not both.**
Do not combine B and F flags on the same MAP entry. INP3 also uses
L3RTT frames but with a different format than FlexNet L3RTT.

### 2.3 Link Management

The `LINKTABLE` struct (`asmstrucs.h:921-1020`) tracks active L2
connections with sequence numbers, data queues, and statistics. Adding
a `BOOL FlexNetLink` flag identifies which connections are FlexNet
sessions vs. normal user/NET/ROM links.

Connection establishment (SABM/UA) is handled by `L2SABM()` at line
1292. Session setup initializes window size, timers, and sequence
numbers. No protocol-specific logic — FlexNet links use the same L2
connection management as NET/ROM.

### 2.4 Command System

Commands are registered in a static array in `Cmd.c:4659`:
```c
struct CMDX COMMANDS[] = {
    "NODES       ", 1, CMDN00, 0,
    "LINKS       ", 1, CMDL00, 0,
    "PORTS       ", 1, CMDP00, 0,
    "ROUTES      ", 1, CMDR00, 0,
    ...
};
```

Each entry maps a command string to a handler function. Adding a "D"
command is one table entry plus a handler function. Output uses
`Cmdprintf()` (line 246) which handles packet-length splitting
automatically, and `SendCommandReply()` (line 4874) queues the
response to the user session.

### 2.5 Existing Routing Structures

LinBPQ stores NET/ROM destinations in `DEST_LIST` structs with both
`NRROUTE[3]` (NET/ROM) and `INP3ROUTE[3]` (INP3) route entries per
destination. The FlexNet destination table will be a separate structure
to avoid coupling with NET/ROM state.

---

## 3. Proposed Architecture

### 3.1 New File: `FlexNetCode.c`

A single new source file (~800 lines) containing all FlexNet logic:

| Component | Description |
|-----------|-------------|
| Destination table | Array of `{callsign, ssid_lo, ssid_hi, rtt, via}` entries |
| CE frame parser | Classify and parse CE frames (types 0-6) |
| CE frame builders | Build init, keepalive, link time, route records |
| CF L3RTT handler | Parse/respond to L3RTT probes |
| Session handler | Event-driven CE session state machine |
| Timer callback | Periodic keepalive and link time exchange |
| D command handler | Format and display destination table |

Protocol code is adapted from flexnetd v0.3.0 (`ce_proto.c`,
`cf_proto.c`, `poll_cycle.c`). The main difference: flexnetd uses a
blocking event loop per session (forked child); LinBPQ uses a
single-threaded polling model with timer callbacks. The protocol logic
is identical; only the I/O pattern changes.

### 3.2 Modified Files

| File | Change | Lines |
|------|--------|-------|
| `asmstrucs.h` | Add `FlexNetFlag` to `arp_table_entry`, `FlexNetLink` to `LINKTABLE`, FlexNet dest struct | ~30 |
| `bpqaxip.c` | Parse `F` flag in MAP (same pattern as `B`) | ~10 |
| `L2Code.c` | Add `case 0xce:` in PROC_I_FRAME switch | ~15 |
| `Cmd.c` | Add `"D"` entry to COMMANDS[], add handler | ~5 |
| `Makefile` | Add `FlexNetCode.c` to OBJS | ~1 |

### 3.3 Data Flow

```
AXUDP frame (UDP)
  │
  ▼
bpqaxip.c:ExtProc()          ← receive, strip CRC
  │
  ▼
L2Code.c:PROC_I_FRAME()      ← extract PID from I-frame
  │
  ├─ PID=0xCE ──► FlexNetCode.c:FlexNet_ProcessCE(LINK, Buffer)
  │                  ├─ '0' init      → echo init, send keepalive
  │                  ├─ '1' linktime  → store, reply with our value
  │                  ├─ '2' keepalive → echo, send link time
  │                  ├─ '3' routing   → parse records, merge to table
  │                  └─ '4' token     → echo token
  │
  ├─ PID=0xCF (on FlexNet link) ──► FlexNet_ProcessCF(LINK, Buffer)
  │                                    └─ L3RTT → echo with timestamps
  │
  └─ other PIDs ──► existing handlers (NET/ROM, IP, text)
```

### 3.4 FlexNet Session Lifecycle

```
1. LinBPQ connects to MAP neighbor (SABM/UA)
   ↓
2. If MAP has F flag → FlexNet_InitSession(LINK)
   Send: CE init (SSID range) + CE keepalive (241 bytes)
   ↓
3. Neighbor responds with init + link time
   → FlexNet_ProcessCE() handles handshake
   ↓
4. Timer fires every ~21s → FlexNet_TimerTick(LINK)
   Send: keepalive echo + link time (value=2)
   ↓
5. Route exchange after first keepalive
   Send: 3+\r + compact route record + 3-\r
   Receive: neighbor's compact records → merge to dest table
   ↓
6. Repeat 4-5 for lifetime of link
   ↓
7. On disconnect → FlexNet_CloseSession(LINK)
   Remove routes learned from this neighbor
```

### 3.5 PID Handling Matrix

| PID | UI frame | Connected (no F) | Connected (F flag) |
|-----|----------|-------------------|--------------------|
| 0xCE | ignored | default → RX_Q | **FlexNet_ProcessCE()** |
| 0xCF | NET/ROM NODES | default → RX_Q | **FlexNet_ProcessCF()** |
| 0xF0 | ignored | default → RX_Q | default → RX_Q |

**CORRECTION**: NET/ROM uses 0xCF in connected mode too (all L3 frames).
A MAP link must be F (FlexNet) or B (NET/ROM), not both.

---

## 4. What We Reuse from flexnetd

| flexnetd component | LinBPQ adaptation |
|---|---|
| `ce_proto.c` — frame parsers and builders | Adapt to LinBPQ buffer format (`DATAMESSAGE`) |
| `cf_proto.c` — L3RTT parse/build | Reuse directly |
| `poll_cycle.c` — session state machine | Restructure from blocking loop to event-driven callbacks |
| `dtable.c` — destination table merge | Adapt for LinBPQ global memory (no malloc) |
| Protocol constants (`RTT_INFINITY`, `CE_KEEPALIVE_LEN`, SSID encoding) | Reuse directly |
| Kernel T2 tuning concept | Not needed — LinBPQ handles its own L2 timing |

---

## 5. Risk Assessment

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| PID 0xCF collision | Medium | Low | Check FlexNetLink flag before routing to FlexNet |
| LinBPQ API changes | Low | Low | Pin to version 6.0.25.x |
| Memory for dest table | Low | Low | Fixed array (2000 entries max) |
| Timer integration | Medium | Medium | Use PORTTIMERCODE callback |
| Thread safety | Low | Low | LinBPQ is single-threaded |
| Build compatibility | Low | Low | Single .c file, standard C |
| Protocol correctness | Medium | Low | Proven in flexnetd production |

---

## 6. Effort Estimate

| Phase | Work | Sessions |
|-------|------|----------|
| Phase 1: MAP F-flag + data structures | Parse flag, add structs | 1 session |
| Phase 2: CE protocol core | Port ce_proto.c parsers/builders | 1 session |
| Phase 3: Session handler | Event-driven init/keepalive/route exchange | 1-2 sessions |
| Phase 4: D command | Table display + command registration | 0.5 session |
| Phase 5: Testing | Live testing with xnet neighbor | 2-3 sessions |

**Total: ~860 lines new/modified code, 5-7 sessions.**

---

## 7. Conclusion

**Feasibility: CONFIRMED.** The integration is straightforward because:

1. LinBPQ's L2 PID dispatch is extensible (add `case 0xce:`)
2. MAP flag parsing has a clean, replicable pattern
3. AXUDP passes frames with PID intact (no driver changes)
4. The command system is a simple lookup table
5. flexnetd's protocol code is pure C and portable
6. No PID conflicts between NET/ROM (UI) and FlexNet (connected)
7. LinBPQ's single-threaded model simplifies state management

The bulk of the work is adapting flexnetd's blocking session loop into
LinBPQ's event-driven timer model. The protocol logic itself is proven
and can be reused with minimal changes.
