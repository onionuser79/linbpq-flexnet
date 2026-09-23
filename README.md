# LinBPQ FlexNet Integration (v2.2.2)

Native FlexNet CE/CF routing protocol support added to LinBPQ so a
BPQ node can participate in a FlexNet packet-radio network alongside
its existing NET/ROM stack.

> **⚠ Scope — read first.** Out of the box linbpq-flexnet advertises
> **only its own destinations**: two FlexNet peers either side of your
> node stay mutually invisible at the routing layer. That is the
> default, and for most installations it is the right one.
>
> Since **v2.2.0** the node *can* carry other neighbours' destinations
> and forward FlexNet traffic, via three independent opt-ins —
> `FLEXNETTRANSIT` (re-advertise what neighbours teach us),
> `FLEXNETL2TRANSIT` (L2 digipeat-chain forwarding) and
> `FLEXNETPATHFORWARD` (relay CE type-6 path traversals). **All three
> default to `NO`** and must be set explicitly.
>
> **That capability is scoped to direct neighbours.** Multi-hop
> destinations cannot be carried: (X)Net never sends CREQ — it
> digipeats and expects L2 routing — so advertising a multi-hop route
> creates a black hole rather than a path. An earlier unscoped attempt
> (v1.9.4) broke AX.25 V2 reciprocity on the return path and was
> reverted in v1.9.7.
>
> So this is **not** a drop-in replacement for the three real FlexNet
> routers — **(X)Net**, **PC/Flexnet**, **RMNC/Flexnet**. If you need a
> full transit router, run one of those. What linbpq-flexnet gives you
> is an existing LinBPQ node that participates properly in the mesh,
> and can relay for its immediate neighbours when you ask it to.

Author: IW2OHX | Based on LinBPQ 6.0.25.40 by G8BPQ.

---

## What's new in v2.2.0

Two wire-level defects found by a 22 h capture of all three FlexNet
links, both fixed. Full write-up:
[`research/link_stability_2026-09-19/`](research/link_stability_2026-09-19/).

- **Packed route advertisements.** The compact CE format is one `'3'`
  per *frame* followed by N records; we were emitting one record per
  AX.25 I-frame — 15 bytes of a 236-byte `PACLEN`, measured at exactly
  1.00 records/frame across 22 h while peers filled to 205–248 B. A
  token now buys a *frame*. The I-frame rate to the peer is unchanged.
  A ~210-destination re-seed after a session reset went from **17.6
  minutes to under 100 seconds**, and the advertisement queue to a
  PC/Flexnet peer went from **non-empty 80 % of the time (median depth
  72) to 6 % (median 0)**.
- **No re-INIT on a healthy link.** `FlexNet_InitSession`'s
  same-callsign/new-LINK path reset the session and sent a fresh CE-INIT
  whenever BPQ recycled the peer's `LINKTABLE` slot — which it does
  during internal L2 maintenance, with nothing on the wire. Every such
  INIT reseeds the peer's link-cost ring with a `600` outlier. The
  established-guard that v2.1.15 added to the same-LINK path now covers
  this one too. **This closes the "PC/Flexnet AXIP cost-ring cycles
  every ~3 hours" limitation** listed in earlier releases.

Measured effect on PC/Flexnet's cost for us, from its `L *`:
`883/5` before → `336/5` and falling, with every ring sample we have
contributed since the upgrade reading `1` — the same value its (X)Net
peers show.

Also in this release: `tools/unit/`, the first unit tests in the repo.
They extract the functions under test verbatim from `FlexNetCode.c`, so
they cannot drift from shipped code.

**Not fixed in v2.2.0, and not what it looked like:** PC/Flexnet kept
cycling the link every 70-87 min. It is not a ~60 s evaluation tick of
its own — the teardowns come in *pairs* 60 s apart, and the second of
each pair is PC/Flexnet re-seeding a fresh session, not a new decision.
Each one follows a `3+` full-table exchange, 1:1 in both directions.
**v2.2.1 fixes three real defects in how that exchange is answered and
the teardowns continue**, so the mechanism that ends the session is
still open. See
[`research/link_stability_2026-09-20/DESTINATION_EXCHANGE_CLIMB.md`](research/link_stability_2026-09-20/DESTINATION_EXCHANGE_CLIMB.md).

## What's new in v2.2.2

**The `IW2OHX-12` teardown is fixed.** Open since 2026-09-18 and untouched
by v2.2.0 and v2.2.1, which fixed real defects in the `3+` exchange while
the sessions kept dying at the same rate.

Root cause, from a 20.9 h capture taken with every node-poller switched off
for the first time: **PC/Flexnet tolerates unsolicited compact records until
it has completed a `3+` exchange, and treats them as a protocol error
afterwards.** Past the closing `3-` it accepts at most two more record
frames and then drops the L2 session — 30/30, reacting synchronously, 30 of
32 teardowns within 0.06 s of one of our frames on a session healthy to the
last ack.

Two things it is *not*, both of which survive casual inspection: not the
record content (`IW2OHX-14 = 2` went out 614 times harmlessly and 14 times
fatally), and not the `3-` placement (a record within 5 s after a `3-`
happens 549 times outside a transaction with zero teardowns).

The fix is [`FLEXNETPCFQUIESCE`](#talking-to-a-pcflexnet-peer-v222--flexnetpcfquiesce),
default on. Measured on IR2UFV:

| | baseline, 20.9 h | v2.2.2 |
|---|---|---|
| `3+` followed by a teardown | **30 / 30** | **0 / 3** |
| record frames after the closing `3-` | 1 or 2, never 0 | **0** |
| session lifetime | median 889 s | 5445 s, three times |

**And a measurement of PC/Flexnet worth having on its own:** what remains
after the fix is PCF's intrinsic AXIP link cycle, and it is a **fixed
5445 s link lifetime, not an idle timeout** — three consecutive cycles
measured to the second across windows with very different traffic. The
2026-05 `flxnod32.dll` reverse-engineering could not distinguish lifetime
from idle; this settles it. It is not reachable from our side, so v2.2.2
handles it rather than prevents it: the gate is released and the peer
re-seeded when it restarts its session.

Full write-up:
[`research/link_stability_2026-09-22/ROOT_CAUSE_PCF_QUIESCE.md`](research/link_stability_2026-09-22/ROOT_CAUSE_PCF_QUIESCE.md).

## What's new in v2.2.1

Four fixes to the advertisement plane, all found by a continuous 24 h
capture of the `IW2OHX-12` (PC/Flexnet) and `IW2OHX-14` ((X)Net) links.
Full write-up:
[`research/link_stability_2026-09-20/DESTINATION_EXCHANGE_CLIMB.md`](research/link_stability_2026-09-20/DESTINATION_EXCHANGE_CLIMB.md).

**A full-table request is answered with the full table.** A peer sending
`3+` is asking for the whole destination table. That walk was passing
`force=FALSE`, so an explicit request ran through the 10 % change-
detection threshold — a filter meant for *unsolicited* advertisements —
and we answered with whatever happened to have moved: measured at **3,
24, 45, 3, 38, 72, 39 and 36 records out of 204**, followed by the `3-`
end-of-batch marker. Every FlexNet peer's table for this node has
therefore been *wrong*, not merely stale, for as long as the walk has
existed. `IW2OHX-14` asked once and got 5 records (2 unique) for the
same 204 destinations; (X)Net tolerates the short answer, PC/Flexnet
hangs up on it, which is the only reason it surfaced on `-12`. Answers
now carry **157-171 unique records** of a ~204-entry table.

The `force=FALSE` was inherited from `flex_advertise_seed_peer()`, where
it is correct and documented: a fresh session's `advertised[]` is empty,
so every entry fires on the never-advertised sentinel anyway. That
precondition does not hold mid-session — hence the tell, that the seed
dump after a restart is complete while a `3+` minutes later returns
three records.

**End-of-batch means the batch ended.** The `3-` marker owed after a
`3+` walk was emitted as soon as the pending queue read empty. That
queue is shared with ordinary change-driven advertisements and drains in
bucket-sized bites, so it reads empty *between refills* while the
response is still in flight: measured, 146 of 213 records went out, then
`3-`, then 3 s later the next 16 records arrived behind a marker already
sent. The `3-` now requires the queue to stay empty for two of the
peer's bucket refill intervals, and any record queued in the meantime
restarts the clock. Late is fine; wrong is not.

**Count-to-infinity containment.** `flex_climb_is_loop()` tracks, per
peer and destination, the cheapest cost seen and the consecutive rises
away from it. Three rises reaching 4x that floor is a lap counter
rather than a path: the destination is withdrawn once and held down.
A fall re-floors and resets, so a route that settles costs nothing, and
both conditions are required — the ratio alone would catch an honest
re-route onto a much worse path, the step count alone would catch any
slowly degrading link. Before the change, 43 of 204 destinations were
climbing geometrically (`K1YMI` 109 -> 4910 across 235 distinct values),
35.7 % of everything advertised, which a purely *relative* 10 %
threshold cannot suppress. The floor must **persist** across a
withdrawal: an earlier revision reset it, the destination re-floored at
its inflated cost and the ladder resumed.

**Wire clamp.** A *finite* cost above 4095 is clamped in
`flex_build_route_rec()`. PC/Flexnet carries its link cost in a 12-bit
field, so a larger number is not a worse route to it but a corrupt one.
The `60000` withdrawal sentinel is exempt — it is a signal, not a
measurement. Measured: **33 over-limit finite records in 10.9 h before,
0 in the 13.4 h after.**

**What this release does *not* fix.** `IW2OHX-12` still tears the link
down after a full-table exchange: 23 `3+` requests in 24 h, 23 followed
by a teardown within 120 s. The short answer was a real defect and is
fixed, but it was not what ends the session, and the mechanism that does
is still open. See *Known limitations*.

---

## What it does (high level)

- **FlexNet participation.** Your BPQ node initiates FlexNet
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
- **`D` command** — FlexNet destination table with wildcard search,
  sort, route filter, and cached-path filter. All modifiers
  combine. Rendered in three-column layout, xnet-style; the `!`
  marker in the list view flags destinations whose PATH_REP cache is
  populated and fresh.
  - Callsign filter: `D <call>` (exact), `D IW*` (prefix), `D *MLB`
    (suffix), `D *HU*` (substring), `D *` (all).
  - Via-neighbour filter (v2.1.9): `D < <neighbour>` shows only routes
    whose chosen neighbour matches. Accepts SSID (`D < IW2OHX-14`) or
    base call (`D < IW2OHX` matches any SSID).
  - Cached-path filter (v2.1.9): `D !` only fresh cached path,
    `D ?` only uncached.
  - Sort (v2.1.9): `D /COST` ascending RTT, `D /CALL` alphabetical,
    `D /AGE` freshest cached path first.
  - Detail view (`D <call>` exact) renders the cached path hop chain
    when present.
- **CE type-6 / type-7 path discovery.** `FlexNet_Timer` fires one
  PATH_REQ every 60 s round-robin through the destination table;
  replies populate an on-disk path cache (`flexnet_path_cache.dat`)
  with 4 h TTL.
- **Multi-FlexNet-neighbour** with cost-based routing (v1.9.2). A
  destination is associated with the neighbour that reports the
  best cost; the outgoing connect goes through that neighbour.
- **`C <flexnet-neighbour>`** works correctly. For non-direct
  FlexNet destinations we emit the two-digi chain
  `MYCALL* NEIGHBOUR`. For direct neighbours we emit a **single-digi
  chain `MYCALL*`** (v2.1.8) so the peer's monitor and L2 connect-
  accept logic see who is relaying the user — a bare un-digi'd
  user SABM is what older code (zero-digi from v1.9.5) produced and
  some peers (PC/Flexnet specifically) DM'd it as an unknown
  station. The `pid=0xCF` L2 dispatch falls through to NetROM L4
  when the payload is not L3RTT so CACK/INFO from peers reach the
  originating user session.

---

## Testing conditions

Verified against:

- `(X)Net V1.39` over AXUDP/HAMNET (primary integration target —
  v2.0.0 GA test rig)
- `PC/Flexnet` over AXUDP — verified fully working against
  IW2OHX-12 (PC/Flexnet V4.0). FlexNet INIT handshake, KA→LT
  cycle, and compact-route advertisements from the peer all
  function. The session reaches `CONNECTED` and the peer pushes
  hundreds of routes (19-entry compact batches every ~5 sec).
  Requires the MAP entry to use the `F` flag only (no `B`). The
  link-cost saturation symptom observed against PC/Flexnet V4 in
  v2.1.0–v2.1.12 (`(4095/2)` pinned in PC/Flexnet's `L *` table)
  is closed in **v2.1.13** by rate-limiting outbound CE link-time
  replies to land in the peer's expected-reply window.

  **v2.2.0** removed the two things that were inflating that cost.
  PC/Flexnet's `L *` row for us now reads `336/5` and falling, with
  every ring sample contributed since the upgrade reading `1` — the
  value its (X)Net peers show. Before v2.2.0 the ring was repeatedly
  reseeded with `600` outliers by our own spurious re-INITs, and the
  link was never idle because the advertisement queue never drained.

  v2.1.37 (carrying the v2.1.36 fix) closed the earlier case where
  PC/Flexnet sends FlexNet-shaped INFO with `PID=0xF0` instead of
  `0xCE`; the v2.1.27 drop was silently swallowing these. See
  `research/ir2ufv-pcf-v2.1.35-capture-analysis-2026-06-02.md`.

Not yet integration-tested:

- `RMNC/Flexnet`
- Older (X)Net versions
- Native AX.25 RF links (only AXUDP over the HAMNET tunnel has been
  exercised)

Live test results from the v2.0.0 GA rig (2026-05-15) driven by
`tools/human_connect.py` against the live FlexNet cloud:

| Test | Pass rate |
|------|-----------|
| BPQ-13 → 20 destinations (cloud + direct FlexNet neighbours) | 37/39 = **95 %** |
| xnet IW2OHX-4 → IW2OHX-13 + via-13 destinations | 31/42 = **74 %** |
| `C IR2UFV-8` from cloud (FLEXNETSSIDRANGE 0-8, BBS at -8) | reaches BBS from xnet -4, xnet -14, IW2OHX-13, and local 2525 |

Earlier 2026-05-14 baselines under v1.9.5 were 89 % / 100 % on
smaller target sets; the v2.0.0 numbers cover wider target lists
including direct FlexNet neighbours that v1.9.5 didn't exercise
through NetROM L4. See `RELEASE_HISTORY.md` for the full version timeline.

**v2.2.0 advertisement measurements** — IR2UFV against all three of its
FlexNet neighbours, 22 h baseline capture vs. the v2.2.0 build:

| | before | v2.2.0 |
|---|---|---|
| records per CE frame | 1.00 on every link, 22 h | 3.4–5.2 steady, 14.3 at cold start |
| bytes per CE frame (`PACLEN` 236) | 12.2 | up to 198 |
| queue to PC/Flexnet, non-empty | 80 % of run | 6 % |
| queue to PC/Flexnet, median / max | 72 / 199 | 0 / 56 |
| re-seed after a session reset | 17.6 min | < 100 s |
| spurious re-INITs on live links | 3–4 per link per 0.8 h | 0 |

Full method and the caveats in
[`research/link_stability_2026-09-19/`](research/link_stability_2026-09-19/).

---

## Repository layout

| File | Description |
|------|-------------|
| `FlexNetCode.c`   | New file: FlexNet protocol module. CE/CF dispatch, `D`/`FL` commands, L3RTT probes, multi-neighbour routing, connection acceptance, on-disk path cache. |
| `asmstrucs.h`     | Modified: `FlexNetFlag`, `FlexNetLink`, path-cache fields, public function declarations. |
| `bpqaxip.c`       | Modified: `F` flag parsing on MAP entries, FlexNet relay acceptance, AXIP byte-6 SSID normalisation for ARP lookups. |
| `L2Code.c`        | Modified: `pid=0xCE`/`pid=0xCF` dispatch with auto-init, FlexNet inbound-SABM acceptance, v1.9.5 fall-through to NetROM L4 for non-L3RTT CF frames. |
| `Cmd.c`           | Modified: `D` and `FL` command registration; FlexNet route lookup in `C` connect handler; v1.9.5 no-digi when target == neighbour. |
| `flexnet_l3.{c,h}`| Standalone FlexNet L3 protocol module (CREQ / CACK / INFO builders, connection table). Currently compiled but not linked from `FlexNetCode.c` — kept for possible reuse. |
| `makefile`        | Modified: builds `FlexNetCode.o` and `flexnet_l3.o`. |
| `ROADMAP.md`      | Open work — the FlexNet L2-routing milestone, v2.3 local `APPLICATION` calls, v2.4 per-link routing options — plus the shipped ledger. |
| `RELEASE_HISTORY.md` | Archive: the full per-release narrative, root causes and reverted experiments. History, not plan. |
| `QUICK_WINS.md`   | Opportunistic improvements — cherry-pick freely. |
| `AGENTS.md`       | Methodology for coding agents (human or AI) picking up work. |
| `sync-and-build.sh` | Dev convenience: rsync this repo to a remote BPQ build host and run `make`. |

---

## Build guide (Raspberry Pi / Linux)

Tested on Raspberry Pi OS (aarch64) with LinBPQ 6.0.25.40.

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

### Build flavours

Two compile-time switches control FlexNet console output. Both default to off (informational logging on, per-frame trace off — the historical dev behaviour).

| Switch | Default | Effect |
|--------|---------|--------|
| `FLEXNET_PROD` | `0` | When set to `1`, **suppresses all FlexNet informational messages** to the console (session lifecycle, route advertisement, neighbour add, etc.). Use for production deployments where the node console should stay quiet. Verify it took with `strings <binary> | grep -c 'FlexNet: '` — measured at v2.2.1, **1** on the silent build against **81** on `flexdebug`. The one that survives is the deliberate `advertised[] full` operator warning, a bare `Consoleprintf` rather than an informational message. |
| `FLEXNET_DEBUG` | `0` | When set to `1`, enables per-frame protocol trace (CE frame type/length per peer, L2-CE-VIA-F0 bypass events, etc.) to the console **and** to `/tmp/flexnet_axudp.log`. Use during investigation. |

**Build commands:**

```bash
# Default dev build (chatty info, no per-frame trace) — historical behavior
make

# Production build (silent on console)
make CFLAGS+="-DFLEXNET_PROD=1"

# Debug build (verbose per-frame trace + info messages)
make CFLAGS+="-DFLEXNET_DEBUG=1"
```

`FLEXNET_PROD=1` and `FLEXNET_DEBUG=1` compose orthogonally: `FLEXNET_PROD=1` silences the info layer regardless of `FLEXNET_DEBUG`. Combining them gives the unusual "per-frame trace on, info off" — not normally useful.

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
BPQBOL:IW2OHX-13} Version 6.0.25.40 (64 bit) and FlexNet v2.2.1
```

The `and FlexNet vX.Y.Z` suffix confirms the FlexNet module is loaded.

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

The FlexNet identity is taken from `NODECALL` in `bpq32.cfg`. By
default, only the node's own SSID is advertised — e.g.
`NODECALL=IW2OHX-13` advertises `IW2OHX (13-13)`.

### SSID-range advertisement (v1.10.0+)

To make multiple SSIDs on the node call reachable from the FlexNet
cloud, add the `FLEXNETSSIDRANGE` directive to `bpq32.cfg`:

```
FLEXNETSSIDRANGE 0-8
```

This makes the node advertise its callsign with SSID range 0..8,
visible to FlexNet peers as e.g. `IR2UFV  0-8  1`. The CE INIT
handshake also declares `max_ssid = 8` so peers don't clamp the
range to the node's own SSID.

The range is purely a FlexNet-layer advertisement. **Inbound
connects use BPQ's existing `APPLICATION` line** to dispatch to
the right app:

```
APPLICATION 1,BBS,,IR2UFV-8,UFVBBS,255    ; -8 → BBS
; future:
; APPLICATION 2,CHAT,,IR2UFV-7,UFVCHT,255 ; -7 → chat
```

- `C IR2UFV-8` from a FlexNet peer → BBS.
- `C IR2UFV` (SSID 0) → node command parser.
- `C IR2UFV-3` (no APPLICATION line) → refused.

NetROM and the existing application bindings are unaffected. The
SSID range is FlexNet-only.


### Transit role (v2.2, opt-in — `FLEXNETTRANSIT`)

By default this node advertises **only its own destinations** and does
not re-advertise what it learns from neighbours. Transit behaviour is
opt-in via `bpq32.cfg`:

```
FLEXNETTRANSIT YES      ; YES|ON|1 enable · NO|OFF|0 disable
```

| | |
|---|---|
| **Compiled default** | `NO` — a node with no `FLEXNETTRANSIT` line never re-advertises |
| **When disabled** | no re-advertisement, no CREQ forwarding, no transit bookkeeping |

The default is deliberately off. Transit is a role a node opts into,
never one it inherits by omission — and not re-advertising is also the
safe behaviour toward PC/Flexnet peers. Leave it unset unless the node is
meant to carry other nodes' routes, and set it explicitly rather than
relying on the default in either direction.

#### Talking to a PC/Flexnet peer (v2.2.2 — `FLEXNETPCFQUIESCE`)

```
FLEXNETPCFQUIESCE YES   ; YES|ON|1 (default) · NO|OFF|0 to restore v2.2.1
```

**After answering a PC/Flexnet peer's `3+`, this node sends it no further
compact records until its next `3+`.** That is what the protocol
specifies — `PROTOCOL_SPEC.md` §2.6 exchanges routes *inside* a
`3+`…`3-` transaction at cycle boundaries — and it is what PC/Flexnet
itself does: over one 20.9 h capture it sent **162** record frames to
this node's **5583**.

It is also the fix for a teardown that was open from 2026-09-18 through
v2.2.0 and v2.2.1. PC/Flexnet tolerates unsolicited records until it has
completed a `3+` exchange and treats them as a protocol error afterwards:
past the closing `3-` it accepts **at most two** more record frames and
then drops the L2 session. Measured 30/30 in the baseline, and it reacts
*synchronously* — 30 of 32 teardowns landed within 0.06 s of one of our
record frames, on a session that was healthy to the last ack.

| | baseline (20.9 h) | v2.2.2 |
|---|---|---|
| `3+` followed by a teardown | **30 / 30** | **0 / 3** |
| record frames after the closing `3-` | 1 or 2, never 0 | **0** |

Scoped to the PCF family via `flex_peer_is_pcf()`. (X)Net sent no `3+`
across the whole baseline and is unaffected — though it is not incapable
of sending one, which is why the scoping is explicit rather than implied
by the transaction.

The gate is released when the peer asks again, and also when it restarts
its L2 session: PC/Flexnet cycles AXIP peers on a **fixed 5445 s link
lifetime** (measured to the second across consecutive cycles, on windows
with very different traffic — so it is a lifetime, not an idle timeout)
and rebuilds its FlexNet state afterwards, while this node's session
survives on the same `LINK` pointer. Without that release the peer would
hold no routes at all until its next `3+`. See
[`research/link_stability_2026-09-22/`](research/link_stability_2026-09-22/).

**What it costs:** the peer's view of this node now refreshes only per
`3+`, so a dead destination can persist in its table until then. The
alternative was a session that ended 30-40 s after every `3+`.

#### How a transit node advertises (v2.2.0)

Re-advertisement is **event-driven**: a record goes out when something
actually changed, not on a table sweep. Captures show this is what
(X)Net does, and it is what keeps a PC/Flexnet peer healthy — earlier
clock-driven attempts saturated PCF's RTT at 4095 and cost it its
session.

| Mechanism | Behaviour |
|---|---|
| **Decision rule** | For each peer and destination, compare what we would advertise (`learned RTT + our link RTT to the source peer`) against what we last told that peer. Emit only if it moved by ≥ 10 %, with a 1-tick (100 ms) floor. |
| **Per-peer rate limit** | Token bucket sized per peer family: PC/Flexnet 1 record / 5 s (burst 2), (X)Net-like 1 record / 2 s (burst 4). Family comes from the peer's own keepalive shape. |
| **Direct-neighbour refresh** | Every 120 s the direct-neighbour set is re-offered to each other peer so it cannot age out between changes. The rest of the table is change-driven only. |
| **`3+` request** | Walks the full learned view through the decision rule and meters it out through the bucket, then sends one trailing `3-` once the queue has drained. |
| **Poison-reverse** | On losing a peer, any destination with no surviving path is withdrawn (RTT=60000) to the other peers; destinations still reachable another way stay quiet. |
| **Split-horizon** | A route is never advertised back toward the peer it was learned from. |

`FL` gains a transit section showing, per peer, the family, learned and
advertised counts, queue depth and current token credit.

#### `FLEXNETL2TRANSIT` — carrying multi-hop traffic (v2.3)

```
FLEXNETL2TRANSIT YES    ; default NO; requires FLEXNETTRANSIT YES too
```

Enables **FlexNet L2 forwarding**: symmetric digi-chain rewriting, which
is how the real routers carry a session to a destination that is not
adjacent to the transit node. Forward, we set our own H-bit and **append
the next hop** as a new unrepeated digi; on the way back we **remove the
entry we added**. The originator therefore only ever sees the chain it
sent, which is what keeps AX.25 V2's digi-reversal invariant intact.

Observed on the wire, IW2OHX-14 → IR2UFV → IW2OHX-4 → IQ2LB-6:

```
in   IW7EAS-1->IQ2LB-6   IW2OHX-14* IR2UFV                 (2 digis)
out  IW7EAS-1->IQ2LB-6   IW2OHX-14* IR2UFV* IW2OHX-4       (3 digis)
in   IQ2LB-6->IW7EAS-1   IW2OHX-4* IR2UFV IW2OHX-14        (3 digis)
out  IQ2LB-6->IW7EAS-1   IR2UFV* IW2OHX-14                 (2 digis)
```

It is a **separate directive from `FLEXNETTRANSIT`, and separately
defaulted off**, because it rewrites *other stations'* frames — not
something a node should begin doing because it inherited a setting.
`FL` reports `extended` / `contracted` / `declined` counts; a large
`declined` is normal, since it counts every frame left to the stock
digipeat, which is the right answer for an adjacent destination.

Safety limits: we never append a callsign already present in the chain
(loop guard), never exceed the port's `PORTMAXDIGIS` or AX.25's 8-digi
ceiling — that ceiling is FlexNet's only hop limit, since AX.25 has no
TTL — and we only ever **remove a hop our own table says we appended**
for that exact `(user, dest, port)`, because an originator-supplied digi
is indistinguishable from ours on the reverse path.

#### `FLEXNETPATHFORWARD` — relaying path queries (v2.3)

```
FLEXNETPATHFORWARD YES  ; default NO; requires FLEXNETTRANSIT YES too
```

A CE type-6 path query is **a chain under construction, not a question
put to one node**. Captures of a real router show it plainly:

```
in   '6' 0x21 "    0" "IW2OHX-4 IW2OHX-12 IR3UGM"
out  '6' 0x22 "    0" "IW2OHX-4 IW2OHX-12 IW2OHX-14 IR3UGM"
```

A node that cannot finish the chain **inserts its own next hop before
the target**, bumps the byte after the type, and passes the type-6 on.
The node adjacent to the target answers with a type-7, which travels
back down the chain.

Answering a query from our own path cache — the v2.2 behaviour — works
until it doesn't, in two ways:

* a cached chain longer than 8 digipeaters cannot be answered at all,
  because AX.25 cannot express it (observed: destinations 9 and 13 digis
  out);
* our own background probe times out for roughly 1 query in 9, and a
  query we cannot answer renders no route for the peer that asked.

With forwarding on, neither matters, because **no single node has to
know or express the whole path**. Observed effect on a peer, for a
destination that could never be answered before:

```
D DB0ACA-15
*** route: IW2OHX-4 IR2UFV IW2OHX-14 IR3UHU-2 IZ3LSV-14 IR3UHF OE7XGR
           OE2XZR OE9XFR-10 DB0WV DB0ACA-15
```

The type-7 answer is relayed back with **no per-traversal state**: the
chain is `[origin, …hops…, target]`, so the station that asked is
whoever sits immediately before us in it.

Separate directive, separately defaulted off, because forwarding puts
our callsign into other stations' queries and costs one frame per hop.
`FL` reports `forwarded` / `declined` / `replies-relayed`.

Safety limits: never hand a frame back to the asker; decline if the next
hop is the origin or already in the chain (the chain is the only loop
information the wire carries); bound by the maximum chain length; and a
stale next-hop session is re-resolved from the neighbour callsign before
giving up. Every decline logs its reason.

On the header byte: we copy the inbound bytes and add one, which is
exactly the delta the captures show. What that byte *means* is not
settled — our own reply builder treats it as a hop count and the replies
we receive do not all fit that reading — so reproducing an observed
delta assumes no theory. The 5-char field is the **QSO id** and passes
through a relay untouched; that is what lets the originator match the
answer.

#### Scope: we advertise exactly what we can carry

The advertisement scope is **derived from `FLEXNETL2TRANSIT`**, not set
separately, because they answer the same question:

| `FLEXNETL2TRANSIT` | Advertised | Why |
|---|---|---|
| `NO` | direct neighbours only | all we can deliver is a destination adjacent to us, via the stock digipeat |
| `YES` | every learned destination | L2 forwarding carries multi-hop |

That coupling makes the failure mode unreachable by construction.
Getting it wrong once was instructive: advertising multi-hop *without*
being able to carry it put **67 unreachable destinations** into a
neighbour's table, and it then preferred us over its working path.
**Re-advertisement makes peers prefer you, so advertising a route you
cannot carry is worse than advertising nothing.**

In a debug build, suppressions appear as `NOT-DIRECT` lines; turning L2
forwarding off on a node that had advertised more emits one `RETRACT`
per destination it withdraws, rather than stranding them.

#### Required alongside it: `DIGIFLAG=1` on the FlexNet port

**A transit node needs both settings.** `FLEXNETTRANSIT YES` makes us
*advertise* routes; `DIGIFLAG=1` on the AXIP/FlexNet port is what lets
us actually *carry* them for the commonest case:

```
PORT
        ...
        DIGIFLAG=1      ; 0=OFF, 1=ALL, 255=UI only
        CONFIG
        ...
```

For a destination **one hop beyond us**, (X)Net does not send a NetROM
CREQ. It sends an AX.25 SABM carrying a two-digi chain
`<peer>* <us>` and expects us to repeat it. With digipeating off the
SABM is silently ignored and the originator reports `link failure`,
while `D <dest>` shows a cost but no path — so the node advertises
routes it cannot carry, which is worse than not advertising them.

Keep `DIGIFLAG=0` unless the node is carrying transit. Turn it on only
together with `FLEXNETTRANSIT YES`.

**Verified on the live mesh 2026-09-21**, pinned through a transit node
(`IW2OHX-13`, `FLEXNETTRANSIT`/`FLEXNETL2TRANSIT`/`FLEXNETPATHFORWARD` all
`YES`, `DIGIFLAG=1`): a connect 1 hop beyond it succeeds on the digipeat
alone, and connects 3 and 4 hops beyond it succeed through L2 forwarding,
with the `contracted` counter proving the reverse-path rewrite. One caveat
worth more than the three successes: an attempt made ~3 min after a restart
failed with `extended=4 contracted=0` because the originator's table still
held a chain the next hop could not complete. **Don't judge transit inside
the first few minutes after a restart, and read the `route:` line before
calling a failed connect a forwarding defect.**

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
On a node with `FLEXNETTRANSIT YES` a second section reports the
transit state per peer — family, learned/direct/advertised counts,
queue depth and token credit — plus the count of RTT=0 refresh markers
skipped.

`Status` is `CONNECTED` (peer established and our routes advertised),
`INIT` (established, routes not yet sent), or `PENDING` (not yet
established). A session is "established" once we've either received the
peer's one-shot type-0 INIT **or** (v2.1.39+) seen sustained CE traffic
from it over a healthy L2 — the latter covers the case where BPQ
recycled our link slot mid-session and the peer's single INIT was
already long past, which pre-v2.1.39 left a fully-working link stuck at
`PENDING`.

### `V` — version

Shows BPQ version and the FlexNet module version (e.g.
`FlexNet v2.1.28`) so you can confirm what's running.

---

## Known limitations

- **Transit is limited to direct neighbours.** With
  `FLEXNETTRANSIT YES` the node re-advertises destinations its
  *immediate* neighbours own; it cannot carry multi-hop routes,
  because (X)Net never sends CREQ — it digipeats and expects L2
  routing — so a multi-hop advertisement becomes a black hole. An
  earlier unscoped attempt (v1.9.4) broke AX.25 V2 reciprocity on the
  return path and was reverted in v1.9.7. With the default
  `FLEXNETTRANSIT NO`, two FlexNet peers behind your node do not see
  each other through it at all.
- **Integration-tested against `xnet` only.** Other FlexNet
  implementations may behave differently — particularly around
  inbound CF handling and SABM digipeat conventions.
- **Path cache fixed-size.** Currently 64 destinations.
- **OPEN: PC/Flexnet still cycles the link after a full-table
  exchange.** `IW2OHX-12` tears the L2 session down 7-120 s after every
  `3+` full-table request it sends — **23 of 23 requests over 24 h**,
  and only 1 of 24 independent teardowns without one. PC/Flexnet
  initiates 100 % of them; this node sends no `DISC` and no `SABM`.
  The teardowns arrive in *pairs* 60 s apart and the second of each is
  PC/Flexnet's fresh-session seed, so 46 wire events are 23 decisions —
  count them as one or the statistics invert. v2.2.1 fixed three real
  defects in that exchange (short answers, a premature end-of-batch
  marker, geometric cost climbs) **and the teardowns continue at the
  same rate**, so the short answer was never the mechanism. The link
  recovers itself each time and (X)Net peers are unaffected — `-14`
  tolerates the same exchange without dropping. Two correlations look
  compelling and are artefacts: the outbound link-time frame and the
  advertisement-volume spike are both *part of* the `3+` answer, so
  they carry its timestamps. See
  [`research/link_stability_2026-09-20/`](research/link_stability_2026-09-20/).
  When measuring this, note that a `3+` arrives only every 75-90 min:
  **a quiet hour is not evidence of a fix.**
- **`FLEXNETTRANSIT` GA scope is direct neighbours only.** (X)Net never
  sends CREQ — it digipeats and expects L2 routing — so multi-hop
  destinations cannot be carried. Advertising them creates black holes.

---

## See also

- `ROADMAP.md` — open work, shipped ledger, lessons carried forward.
- `RELEASE_HISTORY.md` — the full per-release narrative (archive).
- `QUICK_WINS.md` — opportunistic improvements not blocking GA.
- `AGENTS.md` — methodology and conventions for coding agents
  picking up work on this repo.
- [`research/`](research/) — the wire-level investigations behind the
  implementation, indexed by what each one settled.
- [`flexnetd`](https://github.com/onionuser79/flexnetd) — sibling
  project (Linux daemon attempting FlexNet protocol integration with
  URONode; same author). Not a substitute for a real FlexNet router.
- [`g8bpq/LinBPQ`](https://github.com/g8bpq/LinBPQ) — upstream
  LinBPQ source by John Wiseman, G8BPQ.
- The three real FlexNet routers — (X)Net, PC/Flexnet,
  RMNC/Flexnet — are the canonical implementations you'd actually
  deploy as a transit node in a FlexNet mesh.
