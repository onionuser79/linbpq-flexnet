# linbpq-flexnet — Roadmap

## NEXT MAJOR MILESTONE (post-v2.2.0): FlexNet L2 frame routing

**The single most important piece of work after GA.** Until it exists,
this node can only carry transit traffic for destinations that are its
own direct neighbours, which is what `FLEXNET_ADVERTISE_DIRECT_ONLY`
restricts v2.2.0 to.

Measured 2026-09-17 and it invalidates RFC §4.3's premise: **(X)Net does
not send a NetROM L4 CREQ when it routes through a FlexNet neighbour.**
For a destination many hops beyond us it sends the *same* AX.25
two-digi chain it uses one hop out —

```
IW7EAS-2 -> VE3TOK  IW2OHX-4* IR2UFV  ctl=SABM      (repeated, then failure)
```

— and expects the neighbour to forward the frame onward **at layer 2**.
We repeat it correctly, but the digis are then all consumed while the
destination is still remote, so no downstream node has any role in the
chain and the frame dies. Zero PID=CF frames were seen across every
attempt, including against a destination we had never answered a path
query for. That is the defining behaviour of FlexNet: it is a
**link-layer routing network**, not a NetROM overlay.

### SOLVED 2026-09-18: path QUERIES are a traversal, and we now relay them

The second half of the same secret, and it took a separate capture. A CE
type-6 is **a chain under construction**, not a question put to one
node: the node that cannot finish it inserts its own next hop before the
target, bumps the byte after the type, and passes the type-6 on; the
node adjacent to the target answers type-7, which travels back.

```
in   '6' 0x21 "    0" "IW2OHX-4 IW2OHX-12 IR3UGM"
out  '6' 0x22 "    0" "IW2OHX-4 IW2OHX-12 IW2OHX-14 IR3UGM"
```

Implemented as `FLEXNETPATHFORWARD` (default NO). It retires the two
cases that answering from our own cache could never handle — chains
over 8 digipeaters, and our own probe timing out for about 1 query in 9.
Observed on a peer for a destination we had never been able to answer:

```
D DB0ACA-15
*** route: IW2OHX-4 IR2UFV IW2OHX-14 IR3UHU-2 IZ3LSV-14 IR3UHF OE7XGR
           OE2XZR OE9XFR-10 DB0WV DB0ACA-15
```

The answer relays back with **no per-traversal state**, because the
chain says who asked: whoever sits immediately before us in it. Full
write-up: `research/path_query_2026-09-18/TYPE6_IS_A_TRAVERSAL.md`.

### SOLVED 2026-09-17: the mechanism is symmetric digi-chain rewriting

Captured **on PC/Flexnet IW2OHX-12 itself** while it forwarded a user
session from IW2OHX-4 to IGATE, two hops beyond it (gw is not in that
path, and (X)Net's `MONITOR` is an event monitor, not a frame monitor —
so the transit node was the only vantage point):

```
in   IW7EAS-2->IGATE   IW2OHX-4* IW2OHX-12                SABM
out  IW7EAS-2->IGATE   IW2OHX-4* IW2OHX-12* IW2OHX-14     SABM
in   IGATE->IW7EAS-2   IW2OHX-14* IW2OHX-12 IW2OHX-4      UA
out  IGATE->IW7EAS-2   IW2OHX-12* IW2OHX-4                UA
```

**Forward:** set own H-bit, append the next hop as a new unrepeated
digi. **Reverse:** remove the entry we appended and set own H-bit. The
originator only ever sees the chain it sent, so AX.25 V2's reversal
invariant holds. Confirmed for SABM, UA, I, RR, DISC and DM; `src`/`dst`
never change and nothing is encapsulated.

`flexnetd/PROTOCOL_SPEC.md` §5.1 had ruled this out as illegal, which is
why it was never built — but extension only breaks V2 if you skip the
contraction. **v1.9.4 failed because it did the first half only.** §5.2
of the spec now documents the mechanism.

Full method, tooling and transcripts:
`research/l2_forwarding_2026-09-17/FLEXNET_L2_FORWARDING.md`.

### What the milestone has to deliver

1. **Ingress**: accept an AX.25 frame whose digi chain is fully consumed
   and whose destination is not local, when that destination is in
   `FlexNetDests[]` via another FlexNet neighbour. Today `L2Code.c`
   either digipeats by address or hands the frame to NetROM L3/L4;
   neither applies.
2. **Egress**: forward it toward the chosen neighbour in whatever form
   that neighbour expects — the wire shape must come from a capture of
   (X)Net↔(X)Net multi-hop transit, not from inference.
3. **Reverse path**: map the return traffic back to the originator,
   which means per-circuit state keyed on something stable across the
   rewrite. This is where RFC §6.4/§6.5's sequence and circuit-index
   translation work becomes relevant, just at L2 rather than L4.
4. **Loop and TTL safety**: an L2 routing plane needs its own
   loop prevention. rc4's hold-down and learned-table ageing protect
   the *advertisement* plane only.

### Why it must not be rushed

An L2 routing plane that misbehaves does not fail politely — it can
loop frames between real routers on a shared network. The
capture-first prerequisite is **met** (see above), so the remaining risk
is implementation rather than ignorance: the reverse-path state is the
part to get right, since removing the wrong digi corrupts a stranger's
session rather than ours. Note also that **§6's CREQ hook may
still be right for a BPQ/linbpq peer**, which does use NetROM L4 — that
path has simply never been exercised, and it is a much smaller job than
this one.

---

## PLANNED (v2.3): local `APPLICATION` callsigns as FlexNet destinations — issue #1

Requested by **Tom SQ4BJA** (GitHub issue
[#1](https://github.com/onionuser79/linbpq-flexnet/issues/1)) while running
linbpq-flexnet on SR5DDD and SR4DON with AXUDP FlexNet links to SR6DWH-11
and SR1DSZ. **Accepted — feasibility assessed 2026-09-20, no blockers.**

The gap: `FLEXNETSSIDRANGE` only covers SSIDs of the *node's own base
call*. A node whose applications use unrelated callsigns —

```
NODECALL=SR4DON
APPLICATION 1,FBB,,SR4BBX,OLNBBS,255
APPLICATION 3,DX,ATTACH 2 127.0.0.1 63000 S,SR4DXC,DXCLUS,255
```

— cannot advertise `SR4BBX` / `SR4DXC` into the FlexNet cloud at all.
(X)Net can declare such destinations as local nodes; we cannot. Tom's
suggested shape, a repeatable directive, is the right one.

### Why it is cheap: the receive half already works

No dispatch code is needed. `L2Code.c:537-575` already matches an
inbound SABM against every entry of `APPLCALLTABLE[]` — full callsign,
not just SSIDs of `MYCALL` — gated only on the receiving port's
`PERMITTEDAPPLS` mask. A frame for `SR4BBX` arriving on the AXUDP
FlexNet port with its digi chain consumed is already delivered to the
FBB application today. This is the same "no new dispatch code was
needed" result as the v1.10.0 SSID-range item below, and it means the
work is **advertisement-side only**.

### What has to be built

1. **Config.** `FLEXNETLOCAL <CALL>[-SSID]`, repeatable, parsed in
   `flex_load_config()` beside `flex_parse_ssidrange_line()`; small
   fixed array (16 entries is ample), base call ≤ 6 chars so it fits
   the `%-6.6s` compact-record field. Optionally a
   `FLEXNETLOCALAPPS YES` convenience that auto-walks `APPLCALLTABLE[]`
   (declared in `cheaders.h`) instead of listing calls by hand.
2. **Advertise them.** `flex_send_own_routes()` currently emits exactly
   one compact record (`flex_build_route`, our own call + SSID range,
   rtt=1). It becomes a multi-record frame built the way
   `flex_advertise_drain()` already builds one — **one `'3'` per
   *frame*, not per record**, records appended with
   `flex_build_route_rec()`, single trailing `'\r'`. Local entries go
   out at rtt=1, the same cost as the node itself, because they are
   genuinely zero further hops away.
3. **Answer path queries for them.** This is the part that is easy to
   miss and without which the feature only half-works.
   `flex_target_is_us()` (`FlexNetCode.c:3214`) compares the target's
   base call against `MYCALL` only, so a CE type-6 traversal asking for
   `SR4BBX` would fall through to forward-or-decline and the peer would
   show no route for a destination we just advertised. It must also
   match the `FLEXNETLOCAL` list, so we reply type-7 with a chain
   ending at the local application call.
4. **Scope guards.** Local destinations are *not* transit: they must be
   advertised regardless of `FLEXNETTRANSIT` /
   `FLEXNET_ADVERTISE_DIRECT_ONLY`, since we can always carry them, and
   they must never enter `learned[]` or be subject to hold-down.
5. **Don't create black holes.** Advertising a callsign nothing is
   bound to is exactly the failure mode that produced 67 phantom
   destinations in the v2.2 rc4 experiment. Validate each
   `FLEXNETLOCAL` entry against `APPLCALLTABLE[]` at init and log a
   loud warning (and skip the record) when there is no matching
   `APPLICATION` line. Document the `PERMITTEDAPPLS` requirement: the
   application mask on the FlexNet port must include the app, or the
   connect is refused after we advertised it.
6. **Display.** Show local entries in the `FL` / destination views
   flagged as local, so an operator can see what the node is claiming.
7. **Tests + docs.** Unit tests under `tools/unit/` using the existing
   extract-from-source harness: record packing for N local calls into
   one frame, `flex_target_is_us()` against the local list, unbound
   entry rejected. README config section + this roadmap on release.

### Risks

Low. The wire change is additional compact records of a shape we
already emit for learned destinations, so no peer sees anything new
structurally. The one interaction to watch is a `FLEXNETLOCAL` entry
whose base call equals `NODECALL` — that belongs in
`FLEXNETSSIDRANGE` and should be rejected with a pointer to it rather
than emitted as a duplicate row.

### Validation plan

IR2UFV (second instance on iw2ohx-gw) with a distinct application
callsign bound, checked from (X)Net IW2OHX-4 and IW2OHX-14: the call
must appear in `D <call>` with cost 1, `C <call>` must reach the
application, and the type-7 answer must carry a chain ending at it.
Production IW2OHX-13 stays untouched until that passes.

---

## Current state: **v2.2.1 released** 2026-09-21 — and production IW2OHX-13 is now a FlexNet ROUTER

`FLEXNET_VERSION_STR = "v2.2.1"`, tagged. **Both** nodes now run the full
router configuration; production moved from v2.1.42 *and* from leaf to
router on the same day.

| | IR2UFV | production IW2OHX-13 |
|---|---|---|
| build | `flexdebug` | silent (`-DFLEXNET_PROD=1`) |
| `FLEXNETTRANSIT` | YES | **YES** (was `NO`) |
| `FLEXNETL2TRANSIT` | YES | **YES** (was absent) |
| `FLEXNETPATHFORWARD` | YES | **YES** (was absent) |
| `FLEXNETLT3BYTE` | YES | **YES** (was absent) |
| `DIGIFLAG` on the FlexNet port | 1 | **1** (was absent, i.e. 0) |
| `FLEXNETSSIDRANGE` | `0-8` | **`13-13`, deliberately NOT aligned** |

**`FLEXNETSSIDRANGE` must stay `13-13` on production.** IR2UFV can advertise
`0-8` because IR2UFV is its own callsign. `IW2OHX-13` shares its base call
with other live nodes on the same mesh — `IW2OHX-1`, `-4`, `-12`, `-14`,
`-15` — so advertising `IW2OHX (0-8)` would claim `IW2OHX-4` and `IW2OHX-1`,
which this node does not own.

**RFC §11 is superseded by this** (it kept production non-forwarding), on
Marco's explicit decision of 2026-09-21 after being shown that the same
configuration is *not* equivalent on the two nodes — see below.

### Why the same config is not the same change on production

On IR2UFV transit is effectively inert: it ties or loses on cost for every
destination, (X)Net keeps the incumbent on a tie, and its L2 forwarding
counters had never left `0/0/0`. Production is the opposite. `IW2OHX-4` has
**no direct FlexNet link to `IW2OHX-14`** — its only paths are via
`IW2OHX-12` (rtt 196 at the time) and via `IW2OHX-13` (rtt 3) — so
production wins on cost almost everywhere and became a real transit path
within a minute of the restart.

Measured immediately after: `-4`'s destinations via `IW2OHX-13` went
**1 → 71**, and via `IW2OHX-12` **187 → 134**. Production advertises 168
records to `-4` and 35 to `-14` (split horizon suppresses the rest, since
most of what it knows it learned from `-14`).

### Verified on the live mesh, pinned through us

| Test | Hops beyond `-4` | Result |
|---|---|---|
| `c iw2ohx-14 iw2ohx-13` | 1 | **connected** — the `DIGIFLAG=1` path |
| `c iq2lb iw2ohx-13` | 3 | **connected** — L2 forwarding, `contracted` 0 → 25 |
| `c dk0wue iw2ohx-13` | 4 | **connected** |

`declined=0` throughout. **One transient worth knowing about:** the first
`dk0wue` attempt, ~3 min after the restart, failed with `extended=4
contracted=0` while `-4`'s table still read
`IW2OHX-4 IW2OHX-13 IW2OHX-14 DK0WUE`. `-14` does not reach `DK0WUE`
directly — its path is via `HB9ON-15` — so the chain could not complete.
Once the tables converged the route line read
`… IW2OHX-14 HB9ON-15 DK0WUE` and the connect succeeded. **Do not judge
transit inside the first few minutes after a restart**, and check the
`route:` line before reading a failed connect as a forwarding defect.

### Production is now a loop candidate

`-14 → us → -4 → -12 → -14` is a real cycle, and `flex_climb_is_loop()`
is what contains it. A standing watch is armed on `iw2ohx-gw`:
`/tmp/prod-router-watch/` holds a rotating capture of prod's AXIP port
(`udp port 10093`) and `fl.log`, prod's `FL` transit counters sampled every
5 min. Rollback to leaf is one command — `sudo bash /tmp/rollback-prod-leaf.sh`
— which restores `bpq32.cfg.pre-router-20260921-113224` and restarts; the
v2.2.1 binary stays, only the role reverts.

Production also gains v2.2.0's packed advertisements and re-INIT guard by
leaving v2.1.42.

### What v2.2.1 contains

1. **`3+` answered with the whole table** (`force=TRUE` on the request
   walk). It had been running through the 10 % change-detection threshold,
   answering an explicit full-table request with 3-72 of 204 records. Every
   peer's table for us was wrong, not merely stale. Now 157-171 unique of
   ~204.
2. **End-of-batch requires a sustained empty queue** —
   `FLEXNET_EOB_QUIET_REFILLS = 2`. The shared queue reads empty between
   bucket refills, so `3-` was going out mid-response and records followed
   it.
3. **`flex_climb_is_loop()`** — 3 consecutive rises *and* ≥4× the cheapest
   cost seen ⇒ withdraw once, then hold down. The floor **persists** across
   the withdrawal; resetting it lets the ladder resume at the inflated cost.
4. **Wire clamp** on finite costs > 4095 in `flex_build_route_rec()`, the
   `60000` sentinel exempt. Measured 33 over-limit records in 10.9 h before,
   **0** in 13.4 h after.

### ⚠ Known-unfixed, and it is the next thing to look at

**The `-12` teardowns survive all of the above.** 24 h continuous capture
across the rc1/rc2/rc3 cutovers: 23 inbound `3+`, 23 teardowns within 120 s
of one, 1 of 24 independent teardowns without one. Independent teardowns
0.83/h before → 1.12/h after; PC/Flexnet initiates 100 %. So the short
answer was a real defect *and not the mechanism*. What is left to test is
what PC/Flexnet does with a **correct but large** answer — 157-171 records
over 40-120 s — rather than what it does with a truncated one.

Two measurement notes for whoever picks this up: the climbing-destination
share (26.7 % → 34.5 %) and withdrawal count (412 → 967) both rose after
the fix, but the denominator changed with it — we now re-send the full
table on request, climbing destinations included — so neither is a clean
before/after and neither should be read as the climb guard failing.
`IW2OHX-14` saw 2 independent teardowns in 13.4 h against 0 in the 10.9 h
before; one is a peer-side SABM storm (10 inbound SABMs in 5 s), so the
signal is weak and unattributed.

---

## v2.2.0 — released 2026-09-19

`FLEXNET_VERSION_STR = "v2.2.0"`, tagged, deployed to **IR2UFV only** with
`FLEXNETTRANSIT YES`. **Production IW2OHX-13 is untouched** — still v2.1.42,
still an explicit `FLEXNETTRANSIT NO`, and still running a binary built
before the default flip. RFC §11 keeps prod non-forwarding; there is no plan to
promote transit there.

### What v2.2.0 contains

Transit-role D1-D3 (`FlexNetAdvertised[]`, `flex_advertise_check()`,
per-peer token buckets, poison-reverse with hold-down, `learned[]` ageing),
`FLEXNETL2TRANSIT` L2 digi-chain forwarding, `FLEXNETPATHFORWARD` CE type-6
traversal relay — all opt-in, all defaulting to NO.

Plus the two link-stability fixes that closed the 2026-09-18 blocker:

1. **Packed route advertisements.** One `'3'` per *frame* then N records,
   which is what every peer already sends. We were emitting one record per
   I-frame — 15 of 236 `PACLEN` bytes, exactly 1.00 records/frame across
   22 h. A token now buys a frame; the I-frame rate is unchanged. Re-seed
   after a reset: 17.6 min → under 100 s. Queue to PC/Flexnet: non-empty
   80 % → 6 %.
2. **No re-INIT on a healthy link.** `FlexNet_InitSession`'s
   same-callsign/new-LINK path reset the session and sent INIT whenever BPQ
   recycled the peer's `LINKTABLE` slot, reseeding the peer's cost ring with
   a `600` outlier. v2.1.15's established-guard now covers it. **This closes
   the "PCF AXIP cost-ring cycles every ~3 h" item in § "v2.1 — open
   items".**

Measured: PC/Flexnet's cost for us `883/5` → `336/5` and falling, every
ring sample since the upgrade reading `1`. Detail and caveats in
`research/link_stability_2026-09-19/`.

**Known-unfixed at the time:** PC/Flexnet still cycled the link.
~~on its own ~60 s evaluation tick~~ — **that reading was wrong on both
counts.** The 60 s cadence was our own `FL` telnet poll (see the telnet
freeze in `research/link_stability_2026-09-20/TELNET_SLEEP_FREEZE.md`),
and the 70-87 min cycle follows PC/Flexnet's `3+` full-table request, so
it was not "not ours". Still open after v2.2.1 — see the current-state
section above. IW2OHX-4 flaps independently (it does so against
PC/Flexnet too) and is under separate investigation.

Also new: `tools/unit/`, the repo's first unit tests. They extract the
functions under test verbatim from `FlexNetCode.c` so they cannot drift.

Emission is now event-driven per RFC §5: a per-peer token bucket
(PC/Flexnet 1 record / 5 s burst 2, (X)Net-like 1 / 2 s burst 4, family
taken from the peer's own keepalive shape), a decision rule that emits only
when the expected RTT has moved ≥ 10 % / 1 tick from what that peer was last
told, a 120 s direct-neighbour refresh, a full-view seed when a peer comes
up, `3+` walking, and poison-reverse on peer loss. rc2's cap + rotating
cursor — the thing rc1-rc3 died on three times — is **gone**.

**The result that rc1-rc3 never produced.** `D < IR2UFV` on IW2OHX-4 lists
**~110 destinations it now reaches through IR2UFV** (DB0\*, DK0WUE, PE1\*,
PI\*, SV1\*, HB9\*, OK0NAG, F3KT, VA3\*, VE3\*, K\*/N\*/W\*, the IGATE pair),
plus `IW2OHX 14-14 3` and `IR2UFV 0-8 1`. On IW2OHX-14 only four rows
survive, because -14 is the hub that taught us most of that table and its own
paths win on cost — which is split-horizon and distance-vector working, not a
fault. No `?` indirect prefix in either table. Evidence:
`research/transit_v2/rc4-2026-09-17/PEER_TABLE_EVIDENCE.md`.

Measured on the wire (`tools/parse_advertise.py`): inter-record gap median
**4.98 s** to the PCF peer and **2.02 / 2.03 s** to the two (X)Net peers,
against bucket periods of 5 s and 2 s. Tests B1, B2, B5, B6, B8 pass.

**Six defects the first soak found**, two of which no amount of inspection
would have caught:

1. **Poison-reverse was dead code.** §5.7 names a `FlexNet_HandleSessionDown`
   that does not exist, so the walk was hooked on `FlexNet_CloseSession` —
   which needs an explicit DISC. Real peers die through `FlexNet_Timer`'s
   ghost reaper, which `memset`s the slot: IW2OHX-4 cycled with **zero**
   POISON lines in 25 minutes. Now hooked on both paths.
2. **A late-joining peer got nothing.** Trigger (a) fires on *change*, so a
   peer joining a converged node sees an empty view — PCF-12 sat at
   `advertised[] = 2` against 119 and 126 for the (X)Net peers. PC/Flexnet
   never sends `3+`, so transit toward the one family this redesign exists to
   protect would have stayed dead with nothing in the logs.
3. `flex_advertise_neighbours` had to anchor `last_advert`, or the timer's
   first tick duplicated the session-init refresh.
4. `FlexNet_Timer` ticks **~3×/s**, so a "queue non-empty" log guard spammed
   three lines a second per peer through the whole cold-start drain.
5. `tools/parse_advertise.py` had the compact-batch format wrong — **one `'3'`
   per frame, not per record** — so it parsed our own single-record emissions
   and silently dropped every peer's batch, making the split-horizon check
   pass vacuously on a capture holding 120 and 125 destinations.
   **Postscript (v2.2.0-rc6, 2026-09-19):** the fix was applied to the tool
   and stopped there. "Our own single-record emissions" was written down as
   an observation and never read as the defect it was — the *emitter* had the
   same misunderstanding, one record per I-frame, for another four months. It
   cost 15 bytes of every 236-byte frame and was the dominant advertisement
   volume. See `research/link_stability_2026-09-19/PACKED_ADVERTISEMENTS.md`.
   Lesson: when a wire-format bug is found in a tool, check the emitter and
   the parser for the same assumption before closing it.
6. Its split-horizon test was wrong too: echoing a destination back to a peer
   that also knows it is legitimate when another peer taught it to us. The
   naive intersection reported 38-39 violations on a clean run; the real count
   is 0.

**Open before a v2.2.0 tag:** §10.1.c D2/D3 poison tests (the reaper hook
landed the same day, untested), §10.2 Phase 2 1 h soak, **§10.3 Phase 3 24 h
PCF soak — the hard gate, and it needs an explicit operator OK because a
sustained record burst can leave PC/Flexnet needing a manual reset**, §6
T40-T43 CREQ forwarding, and the node MOTD/CTEXT update. PCF's reported link
time to us went 19 s pre-deploy → 156 s → 117 s, the post-restart INIT reseed
converging back down as v2.1.13 describes; it must reach its old floor before
Phase 3 means anything.

### Previous state: v2.1.42 (LinBPQ 6.0.25.40, upstream baseline `ac38bd6`)

**Production put back into non-forwarding mode 2026-09-14 — `FLEXNETTRANSIT NO`.**
Found while planning v2.2 rc4 D1: `g_flexnet_transit_enabled` is compiled
**default TRUE** (`FlexNetCode.c:295`, per RFC §15 Q2) and production
`/home/bpq/bpq32.cfg` carried **no** `FLEXNETTRANSIT` directive, so IW2OHX-13
had been running the rc2 cap+cursor transit re-advertisement — the exact path
RFC §11 says production must stay off ("production does not re-advertise",
"Do NOT promote"). IR2UFV, the designated test bed, had it explicitly `OFF`.

Confirmed on the wire before changing anything: a 150 s capture of prod's
outbound AXIP (UDP 10093 to 44.134.24.4 and 192.168.1.203) showed **16
transit records** — `3HB9AK`, `3HB9AM`, `3HB9ON` — re-advertised to both
peers alongside 2 self records. Not theoretical.

`FLEXNETTRANSIT NO` inserted at cfg line 14, directly after
`FLEXNETSSIDRANGE 13-13`, verified as a one-line diff against the backup
(`bpq32.cfg.pre-transitno-2026-09-14`), then prod restarted (pid 192721; the
directive is read at init only). IR2UFV untouched across the anchored kill;
telnet 2323 + HTTP 8080 back up.

Post-change capture, same filter, 160 s: **0 transit records**, only the
`3IW2OHX==1` self record per peer. Prod is a silent build so the FlexNet
layer's own "transit disabled" line is suppressed — the runtime evidence is
LinBPQ's own `line no 14 not recognised - Ignored: FLEXNETTRANSIT NO` in the
fresh boot log (proving the running process read the directive; our layer
parses the keys upstream ignores) plus the before/after wire counts.

**Decided 2026-09-14 (operator): the compiled default flips to NO, as part of
rc4 D1.** `g_flexnet_transit_enabled = FALSE`, so a node with no
`FLEXNETTRANSIT` line re-advertises nothing — transit becomes a role a node
opts into, never one it inherits by omission. RFC §15 Q2 (which had locked YES on
2026-05-17) is marked superseded with the rationale, the D1 row in §14 now
carries the flip plus README documentation of the directive (currently
undocumented — which is how this happened), and §11 step 2's "deploy with
transit off" becomes a no-config-edit step. No operational change today: prod
carries an explicit `NO` and IR2UFV an explicit `OFF`, so both keep their
behaviour either way; the test bed will have to set `FLEXNETTRANSIT YES`
explicitly when Phase 1 flips on.

**v2.1.42 (2026-09-14) — release marker for the `ac38bd6` baseline.**
Version-string-only release: `FLEXNET_VERSION_STR` `v2.1.41` → `v2.1.42`, so
the node banners name the upstream baseline the tree is actually reconciled
onto (`ac38bd6`, makefile/`-lbacktrace`). No FlexNet-logic change, no protocol
change — `FLEXNET_VERSION_PROTO` stays `linbpq-1.9`, and the upstream base
stays `6.0.25.40` because `ac38bd6` does not bump upstream's own version.

Two clean full rebuilds on iw2ohx-gw, 0 errors / 0 warnings each: the default
chatty build for the IR2UFV soak, then `make clean && make
EXTRA_CFLAGS=-DFLEXNET_PROD=1` for production. Both artefacts gated before
install — version string must read `v2.1.42`, and the chatty/silent marker
must match the target (`grep -acF "FlexNet: initialized (max"`: 1 for IR2UFV,
0 for production).

**IR2UFV (chatty), deployed first:** pid 186781, production `/home/bpq/linbpq`
untouched throughout (pid 149782 verified present after the anchored kill),
telnet 2525 listening, `V` reports `Version 6.0.25.40 (64 bit) and FlexNet
v2.1.42`, `FL` shows both links CONNECTED — IW2OHX-14 (63 routes) and
IW2OHX-4 (10 routes). Rollback: `/home/bpq-ufv/linbpq.pre-v2.1.42-2026-09-14`
plus the matching `bpq32.cfg`.

**Production IW2OHX-13 (silent):** pid 187369, IR2UFV instance count unchanged
across the anchored kill, all 4 ports initialised, telnet 2323 + HTTP 8080
listening, `V` reports `Version 6.0.25.40 (64 bit) and FlexNet v2.1.42`.
Runtime build proof: **0** `^FlexNet:` console lines since the last restart
banner in `nohup.out`, against **13** in IR2UFV's `/tmp/ir2ufv.console` —
silent and chatty behaving as intended. Rollback:
`/home/bpq/linbpq.pre-v2.1.42-2026-09-14` + matching cfg.

MOTDs updated at both nodes per the standing rule — `IR2UFV (0-8) - LinBPQ
V6.0.25.40 + FlexNet v2.1.42 Bollate (MI) JN45NN` (cfg line 93) and
`IW2OHX-13 - LinBPQ V6.0.25.40 + FlexNet v2.1.42 Bollate (MI) JN45NN` (cfg
line 102). The versionless per-port telnet `CTEXT=` banners were left alone by
design. Both restarts also put the runtime logs back on the documented paths
(`/home/bpq/nohup.out`, `/tmp/ir2ufv.console`), ending the 13 Sep `/tmp/bpq*.log`
drift noted below.

Two deploy-script notes worth keeping: `grep -q` must never sit inside a
pipeline in these scripts — under `set -o pipefail` its early exit SIGPIPEs the
writer and fails the line even on a successful match (it aborted the first
IR2UFV run at pre-flight, before any backup or config edit, so nothing was
half-applied); and gw has **no `nc(1)`**, so node verification uses bash
`/dev/tcp` instead — an `nc`-based check there silently returns nothing rather
than failing.

**Upstream sync 2026-09-14 — `ac38bd6`, no version change.** The weekly
watcher flagged upstream tip `ac38bd6` ("Update makefile — Add backtrace to
LIBS", 10 Sep 2026), one commit past our v2.1.41 baseline `af79b9b`, touching
the `makefile` only (+1/-1) and none of the four source files we overlay.

The commit is G8BPQ fixing **our v2.1.41 defect 3 independently and
identically**: `-lbacktrace` appended to the Linux `all: LIBS` line. So that
hunk is no longer a local delta — the reconcile only reworded our makefile
comment to say so, and to record that the `flexdebug` target (ours, upstream
has no equivalent) still carries `-lbacktrace` for the same reason. The
remaining makefile delta is now exactly three things: `FlexNetCode.o` +
`flexnet_l3.o` in `OBJS`, the `flexdebug` target, and that comment. The one
cosmetic difference left is trailing whitespace on the `all:` lines, which we
trim and upstream does not.

**No rebuild or redeploy was needed, and this was verified rather than
assumed.** The changed line is link-time only and the existing prod objects
carry `-DFLEXNET_PROD=1`, so the build-tree artifact was stashed and relinked
from those objects: 0 errors, 0 warnings, `-lbacktrace` on the link line, and
the result is **byte-identical (`cmp`) both to the pre-link artifact and to the
deployed `/home/bpq/linbpq`**. Nothing on air changed; both nodes stay on
`6.0.25.40` / FlexNet v2.1.41 at that point. Watcher baseline `--ack`'d to
`ac38bd6`. The reconcile itself was shipped without a version bump; **the
operator then called for one, so the baseline was released as v2.1.42** —
see the v2.1.42 entry above.

Housekeeping observed while checking the live nodes: both instances were
restarted **Sun 13 Sep 11:29 / 11:31** (host uptime 4 d 19 h, so a manual
restart, not a reboot) using `setsid … >/tmp/bpq13.log` and
`>/tmp/bpqufv.log` — *not* the `nohup.out` append path the v2.1.41 notes
document, so that is where the current runtime logs are.

**v2.1.41 (2026-09-10) — upstream LinBPQ 6.0.25.40 rebase.** G8BPQ
released `6.0.25.40` (commit `af79b9b`, Sep 5 2026), 3 commits and 77 files
past our `6.0.25.36` base (`be1400c`); most of that is Windows build
scaffolding (`Win32bits/`, `.vcproj`, `PG/keps.txt`) that our tree does not
carry. Only two of the four files we overlay changed upstream (`Cmd.c` +5,
`bpqaxip.c` +5/-31); `L2Code.c`, `asmstrucs.h` and the `makefile` did not.
Re-applied by the usual 3-way merge with CRLF/LF normalisation — conflict-free,
and every added/removed line of our delta reproduced exactly (0 lost, 0 extra
on both files). **No FlexNet-logic changes** — a pure upstream-compatibility
update, like v2.1.35 and v2.1.40.

Three upstream defects had to be handled rather than merged verbatim:

1. **`Cmd.c` — crash-test null dereference, dropped.** Upstream's only change
   to `Cmd.c` inserts `char * ptr = 0; *(ptr) = 0;` at the top of `REBOOT()`,
   so the node `REBOOT` command segfaults instead of rebooting. It is
   evidently leftover test code for the new crash handler (it arrives with
   "Test recommit" and the new `Win32bits/StdExcept.c`). The hunk is dropped,
   so our `Cmd.c` is byte-identical to v2.1.40; `objdump` confirms `REBOOT`
   branches straight to `Reboot()`.
2. **`bpqaxip.c` — uninitialised buffer used as a format string, fixed.**
   Upstream collapsed `sprintf` + `OutputDebugString` into `Debugprintf` but
   passed the arguments in the wrong order at three sites:
   `Debugprintf(errmsg, "BPQAXIP Invalid Msg Len=%d ...", ...)` — `errmsg` is
   an uninitialised `char[100]` local, and `Debugprintf` takes the format
   first, so stack garbage became the format string. All three are on the AXIP
   receive error path (invalid length, bad CRC), reachable from a malformed
   AXUDP datagram, and every FlexNet peer we have runs over AXIP. Corrected to
   pass the literal format. The unrelated, legitimate `Debugprintf(errmsg)`
   later in the file (where `errmsg` *is* initialised) is left alone.
3. **`makefile` — missing `-lbacktrace`, added.** `LinBPQ.c` gained
   libbacktrace calls (`backtrace_create_state`, `backtrace_pcinfo`,
   `backtrace_print`) in 6.0.25.40, but upstream's makefile was not updated to
   link the library, so 6.0.25.40 does not link on Linux as shipped. Added to
   the Linux `all` and `flexdebug` targets only; libbacktrace ships with gcc.
   **Fixed upstream in `ac38bd6` (10 Sep 2026) the same way** — see the
   upstream-sync note below; defects 1 and 2 are still open upstream.

Clean build on iw2ohx-gw (`make clean` + full rebuild: 0 errors, 0 warnings);
binary reports `Version 6.0.25.40 ... FlexNet v2.1.41`. Build-tree rollback
copy at `linbpq-build.pre40-bak` on gw.

**Deployed 2026-09-10 to IR2UFV soak** (default/chatty build). Verified: single
clean process (pid 29055), production `/home/bpq/linbpq` untouched throughout,
telnet 2525 listening, `V` reports `Version 6.0.25.40 (64 bit) and FlexNet
v2.1.41`, and `FL` shows both FlexNet links CONNECTED — IW2OHX-14 (54 routes)
and IW2OHX-4 (12 routes). Main-CTEXT MOTD updated to `IR2UFV (0-8) - LinBPQ
V6.0.25.40 + FlexNet v2.1.41 Bollate (MI) JN45NN`; the versionless per-port
telnet `CTEXT=` was left alone by design. Rollback copies:
`/home/bpq-ufv/linbpq.pre-v2.1.41-2026-09-10` and the matching `bpq32.cfg`.
The two boot-log lines `not recognised - Ignored: FLEXNETSSIDRANGE / 
FLEXNETTRANSIT` and `Telnet Server bind(sock) failed port 8772 Error 98` are
the known pre-existing non-regressions (upstream ignores our custom keywords,
which the FlexNet layer parses itself; prod -13 owns 8772).

**Production IW2OHX-13 deployed 2026-09-10** (silent build,
`make clean && make EXTRA_CFLAGS=-DFLEXNET_PROD=1`, 0 errors / 0 warnings).
The deploy script refuses to install a chatty binary into production: it
greps the artefact for a `FlexNet_Info` format string and aborts if present
(absent in the silent build, present in the chatty one, absent in the known-
silent v2.1.40 prod binary). Verified: pid 30288 single instance, IR2UFV
untouched, all 4 ports initialised, telnet 2323 + HTTP 8080 listening, `V`
reports `Version 6.0.25.40 (64 bit) and FlexNet v2.1.41`, and **zero**
`FlexNet:` console lines after the restart banner in `nohup.out` (runtime
confirmation of the silent build). MOTD at `/home/bpq/bpq32.cfg` line 102
updated to `IW2OHX-13 - LinBPQ V6.0.25.40 + FlexNet v2.1.41 Bollate (MI)
JN45NN`. Rollback: `/home/bpq/linbpq.pre-v2.1.41-2026-09-10` + matching cfg.

Post-deploy convergence note: FlexNet routes re-split as 108 via IW2OHX-14 and
0 via IW2OHX-4, where before the restart it was 75 + 30. Not a regression —
IW2OHX-4 was already cycling before the deploy (15 min link uptime against
IW2OHX-14's 21 h), so the DLC7 hub reconnected first with a full table and won
every destination on metric; IW2OHX-4 carries only 3 destinations. Total
destinations are comparable (108 vs 105) and the `D` table is fully populated.
The FlexNet logic is byte-identical to v2.1.40 — only the version string
changed in `FlexNetCode.c` — so route handling cannot have regressed here.

**v2.1.40 (2026-08-11) — upstream LinBPQ 6.0.25.36 rebase.** G8BPQ
released `6.0.25.36` (commit `be1400c`, Jul 24 2026), advancing the tree
through `6.0.25.32/35/36` — 5 commits and 72 files past our `6.0.25.30`
base (`45dc77a`). All four files we overlay changed upstream
(`Cmd.c` +101, `L2Code.c` +36, `asmstrucs.h` +56, `bpqaxip.c` +2). The
FlexNet modifications were re-applied by a 3-way merge onto the new
upstream versions; every added/removed line of our delta was reproduced
exactly (verified line-for-line) and the merge was conflict-free once the
CRLF/LF normalisation was accounted for. The `makefile` did not change
upstream, so our FlexNet object + `flexdebug` additions carried forward
unchanged. **No FlexNet-logic changes** — a pure upstream-compatibility
update, like v2.1.35. Clean build on iw2ohx-gw (0 errors, 0 warnings);
binary reports `Version 6.0.25.36 ... FlexNet v2.1.40`. Deployed
2026-08-11 to IR2UFV soak (default/chatty build) and production
IW2OHX-13 (`-DFLEXNET_PROD=1` silent build); both show FlexNet links
CONNECTED to IW2OHX-4/-14. Node MOTDs (main CTEXT) updated to the new
version on both.

**v2.1.39 (2026-07-09) — clear stale `PENDING` on mid-life session
recreation.** The FL `Status` column derived `CONNECTED`/`INIT`/`PENDING`
solely from `got_peer_init`, which flips only on receipt of the peer's
CE **type-0 INIT**. The peer emits that INIT exactly once, right after
the AX.25 L2 session comes up (skill §1.3). Whenever BPQ recycles our
`LINKTABLE` slot mid-session (reaper / auto-recreate at
`flex_find_session`, or the new-LINK-same-callsign path) while the peer's
L2 link stays continuously up, our session object is reborn long after
that INIT — so `got_peer_init` can never flip and the link is pinned at
`PENDING` even though KA, link-time and compact-route frames all flow
normally and the peer's own L-table shows us fully converged. Observed
2026-07-09: IW2OHX-14 report `IW2OHX-13`/`IR2UFV` at `Q=4 RTT=2/5`,
rr+% ~0.1%, while both linbpq nodes showed `PENDING`. **Fix:** a new
`flex_est_inferred` flag, set by `flex_note_peer_established()` on the
first valid CE frame (KA / LT / compact / `3+`) over a healthy L2 when
the one-shot INIT was missed — the symmetric truth to v2.1.11's "INIT
receipt is sufficient evidence the FlexNet layer is up." `got_peer_init`
stays the literal "we saw the peer's INIT frame" signal; FL status, the
KA route-advert gate and the re-init guard all now test
`flex_is_established()` (`got_peer_init || flex_est_inferred`). Struct
field added in `asmstrucs.h` (the definition the `-DLINBPQ` build uses;
the FlexNetCode.c copy is compiled out). A genuine reconnect (new LINK)
still resets both flags and re-handshakes. No wire-behaviour change.

## Prior state: v2.1.38 in production

linbpq-flexnet participates in a FlexNet mesh alongside its existing
NET/ROM stack, advertising **only its own destinations**. v2.0.0 was the first GA tag;
the v2.1.x line adds **PC/Flexnet compatibility**, verified end-to-end
against IW2OHX-12 (PC/Flexnet V4.0). Built against **LinBPQ 6.0.25.30**.

**Production node IW2OHX-13 runs v2.1.38 with `FLEXNET_PROD=1` (silent).
Test bed IR2UFV runs v2.1.38 with default flags (chatty, for observability).**

**v2.1.38 (2026-06-03) — production-silence build switch.** Adds
`FLEXNET_PROD` compile-time switch. When set (`-DFLEXNET_PROD=1`),
all 55 unconditional `FlexNet_Info(...)` call sites are dead-code-
eliminated; the binary has zero `"FlexNet:"` strings in `.rodata`.
Default `FLEXNET_PROD=0` preserves the historical informational
logging for development. No protocol or wire-behaviour changes
from v2.1.37. See README §"Build flavours" for the matrix.

**v2.1.37 (2026-06-03) — version-string + docs roll-up.** Bumps
the user-facing `FLEXNET_VERSION_STR` from `v2.1.35` to `v2.1.37`
so the BPQ `V` command shows the correct release. README + ROADMAP
updated to reflect the LinBPQ 6.0.25.30 base and the v2.1.36 cost-
floor improvement. No code-logic changes from v2.1.36.

**v2.1.36 (2026-06-02) — PCF PID=F0 demotion bypass.** Closes a
case where PC/Flexnet sends FlexNet-shaped INFO (`'2'+spaces` KA,
`'1'+digits+'\r'` LT) with **PID=0xF0** instead of `PID=0xCE`.
The v2.1.27 non-CE/CF drop on FlexNet-flagged links was silently
swallowing these, leaving PCF's `L *` cost ring stuck at the pure
29-s-KA-cadence floor of ~280. v2.1.36 adds a CE-shape probe
(`FlexNet_ClassifyCEShape`) before the v2.1.27 drop; if the INFO
parses as a known CE sub-PID the buffer is re-dispatched through
`FlexNet_ProcessCE`. Banner-trigger user-traffic frames (the
original v2.1.27 case) still get dropped because they don't parse
as CE. 14h+ soak on IR2UFV: cost stable at ~180-200/5, zero L2
cycles, ~2.4 bypass firings/min. See `research/ir2ufv-pcf-v2.1.35-
capture-analysis-2026-06-02.md` for the wire decode + FCS
verification.

**v2.1.35 (2026-06-02) — upstream LinBPQ 6.0.25.30 rebase.** G8BPQ
released `6.0.25.30` (commit `45dc77a`, Jun 1 2026) introducing INP3
protocol enhancements that needed two new fields in `struct ROUTE`
(`TXRTTIncrement`, `STTAtLastChange`) and one in `struct DEST_LIST`
(`LastTT` replacing the old `RouteLastTT` pointer). Our patched
`asmstrucs.h` overlaid the upstream header and blocked those new
fields — `BPQINP3.c` failed to compile on the rebase. Fix: added
the three fields to our `asmstrucs.h` in the exact positions
upstream put them. No FlexNet-logic changes from v2.1.32; this
release was a pure upstream-compatibility update. The
[v2.1.33](https://github.com/onionuser79/linbpq-flexnet) and
v2.1.34 numbers are reserved for the failed `STATUS_10` pong
experiments documented in
`research/status_10_framing_investigation_2026-06-02.md`.

**v2.1.32 trade-off (2026-06-02), updated post-v2.1.36:** the
v2.1.30 experiment that removed the `g_flexnet_transit_enabled`
gate on the periodic route re-advertisement was reverted in
v2.1.32. The post-v2.1.36 stable floor is:

- `FLEXNETTRANSIT=OFF`: PC/Flexnet cost **~180/5** (was 279 pre-
  v2.1.36) with a 16-sample ring mixed across two populations:
  small samples (2-3) from bidirectional LT exchanges and large
  samples (~285-292) from 29-s KA inter-arrivals. L2 link stable
  indefinitely — 14h+ observed without DISC on IR2UFV after the
  v2.1.36 deploy. v2.1.36's PID=F0 bypass added the small-sample
  population that pulled the smoothed average down.
- `FLEXNETTRANSIT=ON`: cost ~2/5 with a few bad samples in the ring,
  L2 link cycles every 10-30 minutes but recovers automatically via
  the v2.1.25 + v2.1.26 + v2.1.27 adoption hooks (no process
  restart, no port re-bind).

The numeric-cost vs. stability trade-off is an open item — see
"Next steps" at the end of this document.


The PC/Flexnet compatibility stack is now complete across five
distinct symptom classes:

- **Link-cost saturation at 4095** (v2.1.13) — rate-limited
  outbound type-1 link-time replies so they land inside
  PC/Flexnet's expected-reply window, avoiding the negative-delta
  wrap that pinned every sample at the 12-bit RTT cap.
- **~90-min spurious session reconnects from L2STATE blips**
  (v2.1.14) — added hysteresis to the session reaper so a single
  transient `L2STATE != 5` blip during routine AX.25 state
  transitions no longer destroys a live FlexNet session slot.
- **Periodic session re-handshakes from spurious `FlexNetLink`
  clears** (v2.1.15) — the proactive CE-init scan was re-handshaking
  to peers whenever some BPQ-internal L2 maintenance path cleared
  `LINK->FlexNetLink` without actually closing the L2 link. That
  re-INIT made PC/Flexnet reseed its link-cost ring with the
  `600 …` outliers, re-introducing the cost spike v2.1.13 had
  already solved. Now: if our session for the LINK is already
  established (`got_peer_init == TRUE`), we just re-promote
  `LINK->FlexNetLink` without disturbing the peer.
- **Periodic session-reaps from BPQ recycling the LINKTABLE slot**
  (v2.1.16) — even after v2.1.15, IR2UFV still saw a fresh
  `session started` event every ~90 min on the IW2OHX-12 link
  (new-slot path, not "session reconnected"). Root cause: BPQ's
  L2-idle handling occasionally runs `CLEAROUTLINK` on a LINKTABLE
  slot we still reference, zeroing the entire struct in place. Our
  stale `sess->LINK` now points at memset bytes (LINKCALL[0]==0,
  L2STATE==0), which is *persistent* bad state — the v2.1.14
  reap-hysteresis 3-strike counter trips on it, the slot is reaped,
  and the next inbound CE frame on the BPQ-allocated *new* LINK
  triggers a fresh INIT+KA handshake. Now: the reaper first looks
  for a new LINK matching the stashed `peer_callsign` on the same
  port, and if found migrates `sess->LINK` to it without
  re-INITing. `peer_callsign` is captured in every InitSession
  path so the migration survives slot recycling.
- **Residual reseed when the migration scan loses the BPQ race**
  (v2.1.17) — the v2.1.16 migration only catches recycles whose
  *new* LINK has already reached `L2STATE == 5` at the moment the
  reaper ticks. When the new LINK is still mid-SABM (or hasn't
  yet been allocated) the migration finds nothing, the session
  gets reaped, and the next inbound CE frame triggers the
  new-slot path — which used to unconditionally emit a fresh
  INIT. Solution: persistent per-peer (callsign, port) INIT
  cooldown. We send INIT to a peer at most once per
  `FLEXNET_INIT_TX_INTERVAL` (1 hour); the cooldown survives
  session destruction. PC/Flexnet only reseeds its link-cost
  ring on received INIT, so no INIT = no reseed. When the peer
  sends *us* a fresh INIT, we clear our cooldown so the next
  refresh reciprocates.

What works today, from the v1.x line that shipped:

- Node identity preservation in outbound digi chain (v1.2).
- All six P1 protocol-correctness items: L3RTT counters, link-down
  guard, L3 INFO envelope on replies, IIR-smoothed link time,
  dtable RTT=0 skip, KA cadence (v1.3.x).
- M5 path discovery — CE type-6/7 PATH_REQ/PATH_REP with on-disk
  cache (v1.9.0 / v1.9.1).
- Multi-FlexNet-neighbour with cost-based routing (v1.9.2).
- AXIP byte-6 SSID normalisation + session-table hygiene (v1.9.3).
- `C <flexnet-neighbour>` fixes — no-digi when target ==
  neighbour + `case 0xcf` fall-through to NetROM L4 (v1.9.5).
- 3-column D output, `CE-UNKNOWN` log entry (2026-05-14 cosmetic
  commit).
- `CE_FRAME_STATUS_1N` classifier for the `"1n\r"` status family,
  cleaning up the `CE-UNKNOWN` log spam without changing on-wire
  behaviour (v1.9.8).
- L2Code.c `case 0xcf` no longer falls through to `flexnet_default`
  after `FlexNet_ProcessCF` returns 0 — the second memmove was
  reading from a now-corrupted source position and overwriting
  the PID byte with the L3 TTL. `C IW2OHX-4` and `C IW2OHX-14`
  from the BPQ console now print "Connected to" and the banner,
  closing the last visible asymmetry between FlexNet-link L2
  digi-chain connects (v1.9.5 path) and L4 NetROM connects (v1.9.9).
- v2.1.7 — proactive CE-init scan no longer auto-classifies user
  pass-through sessions as peer-to-peer FlexNet sessions. The scan
  matched on `LINKCALL` only, which also caught LINKs created when a
  telnet user issued `C <flexnet-peer>` (the user's session terminates
  at the peer's call too). The result was that linbpq pushed
  `pid=CE INIT` + `KA` frames into the user's L2 session, and
  PC/Flexnet stopped delivering reply data while still L2-ACKing.
  Two additional filters in the scan — `OURCALL` base must equal node
  `MYCALL` base, and `DIGIS[0]` must be zero — restrict auto-init to
  the direct AXIP peer tunnel.
- v2.1.8 — direct-neighbour `C <call>` now emits a **single-digi**
  chain `MYCALL*` (H-bit set) rather than the v1.9.5 zero-digi
  arrangement. Without any digi, the SABM arrived at the FlexNet
  peer as a bare user callsign that PC/Flexnet didn't recognise and
  DM'd:
  ```
  <R IW7EAS>IW2OHX-12 SABM+>
  <T IW2OHX-12>IW7EAS DM->
  ```
  With a single MYCALL digi the SABM becomes
  `<user> -> <peer> via <us>*` and PC/Flexnet accepts it. The reverse
  UA carries `MYCALL` as a pending digi; the existing L2-RX-DIGI
  handler matches the active LINK, marks the H-bit and delivers
  locally, so no L2 changes are needed.
- PC/Flexnet compatibility (v2.1.0 + v2.1.6 + v2.1.8). Three changes
  were needed in total, identified by direct comparison against live
  xnet-14 ↔ IW2OHX-12 wire captures used as the gold-standard
  reference:
  1. v2.1.0 — when a peer in the AXIP MAP table with the `F` flag
     (FlexNet-only, no NetROM) initiates the L2 SABM, the new hook
     in `L2Code.c` detects the F-flagged peer in the inbound
     SABM-accept path, marks the LINK as FlexNet, suppresses BPQ's
     default CTEXT banner (which PC/Flexnet rejects as non-protocol
     traffic), and drives `FlexNet_InitSession`. Without this the
     session DISC'd within seconds. The keepalive parser was also
     relaxed to accept the 201-byte PC/Flexnet shape (`'2'` + 200
     spaces) in addition to xnet's 241-byte form.
  2. v2.1.6 — `flex_send_own_routes` previously prefixed each
     outbound route advertisement with `"3+\r"`. Per spec §2.6,
     `"3+\r"` is a REQUEST sent to the peer ("please send me your
     routes"), not a marker that the sender is about to advertise.
     PC/Flexnet treated the spurious prefix as a malformed exchange
     and DISC'd the L2 session every few seconds. Removing the
     leading `"3+\r"` while keeping the trailing `"3-\r"` (the
     legitimate end-of-our-batch marker, per spec) restored the
     session and PC/Flexnet immediately began pushing its full
     compact route table — verified against IW2OHX-12, 19-entry
     compact batches arriving every ~5 sec, FL shows status
     `CONNECTED` with the peer's KAs counted.

- v2.1.9 — operator-UX additions to the `D` (destinations) command:
  `D /COST` / `D /CALL` / `D /AGE` sort modifiers, `D < <neighbour>`
  via-neighbour filter, and `D !` / `D ?` cached-path filters. No
  protocol or wire changes; pure local presentation.

- v2.1.10 — per-session keepalive shape mirror. The CE keepalive
  builder records the length and trailing byte of each accepted peer
  KA on the session struct and echoes a matching-shape frame on the
  next send. PC/Flexnet emits a `'2' + 200 spaces + CR` shape and was
  observed to silently discard our (X)Net-shape echo before this
  change. The mirror was later simplified in v2.1.13 once the actual
  saturation root-cause was identified — see below.

- v2.1.11 — three coupled changes that surfaced after IR2UFV was
  added as a second linbpq-flexnet test instance on the same gateway:
  1. Route emission no longer gates on the **first received peer KA**.
     PC/Flexnet does not reliably emit its own KAs after the initial
     SABM/UA on AXUDP-mapped peer sessions, so the gate at
     `case CE_FRAME_KEEPALIVE` never fired and our destination table
     never reached the peer's link table. Route emission now triggers
     on `case CE_FRAME_INIT` instead — INIT receipt is sufficient
     evidence that the FlexNet layer is up.
  2. Peer flavour is inferred from observed INIT length (PC/Flexnet
     V4 sends a 6-byte init, (X)Net sends 5 bytes); the keepalive
     shape and records-per-emit cap follow.
  3. Records-per-emit cap drops from 8 to 2 for PC/Flexnet peers (and
     1 for unknown flavour) — wire evidence showed PC/Flexnet
     issuing DISC the moment it received the 5th back-to-back
     I-frame in a 1-ms burst on its inbound queue.

- v2.1.12 — short-lived attempt that reactively answered PC/Flexnet
  keepalives with a CE_FRAME_STATUS_10 (`"10\r"`) pong instead of a
  KA echo, based on misreading the IW2OHX-14 ↔ IW2OHX-12 wire
  capture. The pong reply is what (X)Net IW2OHX-14 emits in response
  to PC/Flexnet's active probe; copying it from a peer that lives on
  a different code path didn't reproduce the live-peer measurement
  cycle. **Reverted by v2.1.13.** Documented here for posterity.

- v2.1.13 — **closes the PC/Flexnet link-cost saturation root cause.**
  Rate-limits outbound CE type-1 (link-time) replies based on peer
  flavour:

  - PC/Flexnet peers (KA terminator = CR): ≥ 320 s between sends.
  - (X)Net peers (KA terminator = space): ≥ 20 s between sends.
  - First LT during the session-start handshake passes unrestricted.

  PC/Flexnet computes an internal expected-reply timestamp after
  each CE link-time frame. With our advertised smoothed link-time
  value of `2` (the spec-recommended advertise, see `FLEXNET_WIRE_LT`),
  the expected next-reply lands roughly 19 seconds out; with a fully
  smoothed link the cap is 320 seconds. Replies arriving **before**
  that window underflow PC/Flexnet's RTT delta arithmetic and clamp
  the sample to its 12-bit saturation cap (= 4095), which is the
  value we'd observed pinning the IR2UFV link in PC/Flexnet's `L *`
  table across v2.1.0–v2.1.12. The rate limit moves every outbound
  LT into the valid window. The sibling flexnetd project uses the
  same strategy on its PC/Flexnet ports.

  Other v2.1.13 changes: dropped v2.1.10's per-session KA shape
  mirror in favour of the universal `'2' + 240 spaces` (no trailer)
  shape both (X)Net and PC/Flexnet accept; reverted v2.1.12's "PCF
  → `10\r` only" branch in `case CE_FRAME_KEEPALIVE` so we again
  echo the KA + send LT for **all** peer flavours, matching
  flexnetd's reference behaviour.

  Verified on the IR2UFV ↔ IW2OHX-12 link, 2026-05-27: cost in
  PC/Flexnet's `L *` table converged
  `4095/2 → 1566/2 → 941/2 → 315/2 → 258/2 → 2/2` over ≈ 85 minutes,
  with all 16 ring samples settling at `2 3` (i.e. 20–30 ms RTT,
  matching the (X)Net peer baseline). xnet IW2OHX-14's view of
  IR2UFV stayed at `F 2 2/2` throughout — no regression.

- v2.1.14 — **closes the ~90-min spurious session-reset cycle**
  that v2.1.13 still exhibited on the IR2UFV ↔ IW2OHX-12 link.

  Extensive monitoring on 2026-05-28 (3-hour wire capture + console
  trace) showed PC/Flexnet's `L *` entry for IR2UFV would rebuild
  from scratch (`600 …` seed pattern, cost climbing back to mid-
  thousands) approximately every 90 minutes — without a single
  AX.25 U-frame (SABM/DISC/UA/DM/FRMR) crossing the wire. The L2
  link was continuously up; only our internal FlexNet session
  slot was being cleared.

  Root cause: the session-reaper loop in `FlexNet_Timer`
  (`FlexNetCode.c:1696`) treated any single observation of
  `LINK->L2STATE != 5` as proof the link was gone, dropped the
  session slot in place, and let the next inbound CE frame auto-
  recreate it via `FlexNet_ProcessCE`. BPQ briefly takes
  `L2STATE` away from 5 during routine AX.25 internal state
  transitions (mod-128 negotiation, N(S) wrap, retry timing
  windows), and a single transient triggered the reap.

  The fresh session-start emitted INIT + KA to the peer, which
  PC/Flexnet reads as "new peer" and reseeds its link-cost ring
  with the `600 4095` outliers — driving the cost back up until
  the rate-limited LT cycle converged it again over the next
  hour. Cosmetically the link kept working, but PC/Flexnet's
  routing decisions and outbound cost advertisements about
  IR2UFV were inflated for half of every cycle.

  Fix: added a `reap_strikes` counter to `FLEXNET_SESSION`. The
  bad-state condition (`L2STATE != 5` OR `LINKCALL[0] == 0`)
  must now persist for `FLEXNET_REAP_STRIKES = 3` consecutive
  `FlexNet_Timer` ticks before the slot is destroyed. Any tick
  that observes a recovered state (L2STATE back to 5) resets the
  counter to zero. `LINK == NULL` still reaps immediately —
  that's not a transient.

  Verified on IR2UFV ↔ IW2OHX-12: zero `session reconnected`
  events and zero `reaping` messages in the 80+ min validation
  window after deploy, against the previous ≈ 90 min cadence
  observed pre-v2.1.14. The PC/Flexnet `L *` entry kept the
  same age counter the entire time, with the ring filling
  cleanly from `600 4095 2 2 …` through the standard convergence.

- v2.1.15 — **closes a residual session-reset path that v2.1.14 did
  not cover.** After v2.1.14 deploy, IR2UFV ↔ IW2OHX-12 still saw
  one `session reconnected (same LINK, re-sent init + keepalive)`
  event per ≈ 80 min — wire still showed no AX.25 U-frames. Same
  symptom on PC/Flexnet's side (cost spike back to mid-hundreds,
  ring reseeded with `600 …`), but the trigger was not the reaper.

  Root cause: the proactive CE-init scan in `FlexNet_Timer`
  (`FlexNetCode.c:1755`) iterates connected L2 links and calls
  `FlexNet_InitSession` whenever it finds `L2STATE == 5 &&
  !LINK->FlexNetLink`. Several BPQ-internal L2 maintenance paths
  (e.g. `CLEAROUTLINK` followed by silent slot reuse, internal
  state-machine resets) clear `LINK->FlexNetLink` to FALSE without
  actually closing the L2 link or sending DISC. The proactive
  scan reads that as "this link has no FlexNet session yet" and
  re-runs the INIT handshake. `FlexNet_InitSession`'s same-LINK-
  match branch then resets `sent_routes`, `got_peer_init`,
  `keepalive_count`, and `session_start`, and re-sends INIT + KA
  to the peer — which PC/Flexnet reads as "new peer" and reseeds
  its link-cost ring with the familiar `600 …` outlier pattern.

  Fix: split the same-LINK-match branch into "already established"
  vs "still in handshake". An established session (one where we
  have already received the peer's INIT — `got_peer_init == TRUE`)
  is now treated as authoritative: we re-promote
  `LINK->FlexNetLink = TRUE` and return silently, without
  re-sending INIT/KA or resetting session state. The original
  re-handshake flow is preserved for the mid-handshake case
  (`got_peer_init == FALSE`).

  This pairs with the v2.1.14 reaper hysteresis to provide
  defence-in-depth: even if some BPQ-internal path clears
  `FlexNetLink` mid-life, the peer's link-cost ring is not
  disturbed.

- v2.1.16 — **closes the residual `session started` (new-slot)
  cycle that survived v2.1.14 + v2.1.15.** First observation of
  v2.1.15 on IR2UFV showed PC/Flexnet's IR2UFV entry being
  recreated again ~21 min into the run; console showed
  `FlexNet: session started on port 2 with IW2OHX-12 (sent init
  max_ssid=8 + keepalive)` rather than `session reconnected`. The
  v2.1.14 reaper had fired and the next inbound CE frame on the
  *fresh* LINKTABLE slot ran the new-slot branch — which always
  sends INIT/KA, so PC/Flexnet still reseeded its ring.

  Root cause is BPQ-internal: `CLEAROUTLINK` (L2Code.c:4117) is
  called from several L2 maintenance paths (idle-timer N2-retry
  exhaustion, FRMR, DISC retry, …) and `memset`s the entire
  LINKTABLE struct in place. The slot can then be re-allocated by
  BPQ to the same peer when the next AXIP frame arrives. Our
  `sess->LINK` pointer is unchanged but now points at zeros
  (`LINKCALL[0]==0`, `L2STATE==0`) — *persistent* bad state, so
  v2.1.14's 3-strike hysteresis trips fast. The slot is reaped
  and the BPQ-allocated NEW LINKTABLE entry then hits the
  new-slot branch on the next CE frame.

  Fix: added `peer_callsign[7]` to `FLEXNET_SESSION` (set in
  every `FlexNet_InitSession` branch). Before the reaper destroys
  a bad-state session it scans `LINKS[0..MAXLINKS]` for an
  L2STATE==5 entry whose port and 7-byte LINKCALL match the
  stashed `peer_callsign`. If found, the session migrates to the
  new LINK pointer: `sess->LINK = new_LINK`,
  `new_LINK->FlexNetLink = TRUE`, `reap_strikes = 0`. The session
  stays `got_peer_init == TRUE` and `sent_routes == TRUE` — no
  INIT/KA is sent to the peer, so the peer's link-cost ring is
  untouched.

- v2.1.17 — **closes the residual reseed that survived v2.1.16
  when the migration scan loses the BPQ race.** v2.1.16 deploy
  showed PC/Flexnet's IR2UFV entry was still rebuilt ~92 min in,
  with the familiar `600 600 4095` seed pattern; console said
  `FlexNet: session started on port 2 with IW2OHX-12 (sent init
  max_ssid=8 + keepalive)`. The v2.1.16 reaper-time migration
  requires the *new* LINK to already be at `L2STATE == 5` at the
  exact reaper tick — when BPQ's L2-link maintenance and our
  reaper tick race the wrong way, the new LINK is still
  initialising and the migration scan finds nothing. The session
  is reaped, the next inbound CE frame from the now-L2STATE-5
  new LINK hits `ProcessCE` → `flex_find_session` returns NULL →
  `FlexNet_InitSession` new-slot path, which used to
  unconditionally fire INIT + KA.

  Fix: persistent per-peer INIT cooldown. A new static table
  `g_init_history[FLEXNET_INIT_HISTORY_SIZE]` (16 entries,
  ample for any realistic peer set) records the last outbound
  INIT timestamp per (callsign, port). The three
  `FlexNet_InitSession` branches now consult
  `flex_init_recently_sent()` before emitting INIT — if we've
  INIT'd this peer within `FLEXNET_INIT_TX_INTERVAL` (3600 s =
  1 hour) the INIT (and the kick-start KA after it) is
  suppressed; the console message reflects the suppression.
  `CE_FRAME_INIT` in `ProcessCE` calls
  `flex_clear_init_history()` so a peer that resets its own
  state and signals so with a fresh INIT will get our INIT
  back on the next refresh.

  The table lives outside `FlexNetSessions[]`, so the cooldown
  survives any number of reaper/recreate cycles. v2.1.13's
  rate-limited LT cycle continues to work regardless of
  session lifecycle — the link-cost ring on PC/Flexnet's side
  is now durably owned by linbpq-flexnet for as long as
  PC/Flexnet keeps the peer entry.

What was tried and reverted:

- v1.9.4 — transit-role D-table re-advertisement. Reverted in
  v1.9.7 because the L2 digipeat path it implied broke AX.25 V2
  reciprocity on the return frame, and the simpler chain-preserving
  variant could not be validated end-to-end. linbpq is back to
  advertising only its own destinations, with no transit forwarding.

For the full release timeline, test numbers, and investigation
narrative, see the `project_linbpq_v1_9_release.md` and
`project_linbpq_v1_9_5_test_results.md` memory files.

---

## v2.1 — open items (PCF L2-cycle residual) — **CLOSED in v2.2.0**

> **Resolved 2026-09-19.** The ~3 h cost-ring reseed described below was
> our own doing: BPQ recycles the peer's `LINKTABLE` slot during internal
> L2 maintenance (no wire event), and `FlexNet_InitSession`'s
> same-callsign/new-LINK path then reset the session and emitted a fresh
> CE-INIT — which PC/Flexnet treats as a reseed-the-ring trigger. The
> section below guessed the mechanism correctly but attributed it to the
> new-slot branch. v2.1.15's established-guard now covers the migration
> path too. Spurious re-INITs measured after the fix: **0**.
>
> Separately, the link was never *idle* because the advertisement queue
> never drained (one record per I-frame). Both are fixed; see
> `research/link_stability_2026-09-19/`.
>
> **Still open and not ours:** PC/Flexnet's own ~60 s evaluation tick,
> which cycles the L2 session at an unchanged 1.13/h.


The PC/Flexnet IW2OHX-12 link from IR2UFV still cycles
**approximately every 3 hours** even with v2.1.24's per-peer KA
cadence. The cycle is **cosmetic for routing** — the L2 link
itself stays up across the event (BPQ `L` shows `S=5`
throughout), and v2.1.13's LT rate-limit re-converges the cost
ring to `2/2` within ~5 minutes after each reseed. `max_ssid=0-8`
also stays correct (retained from the original handshake INIT).
Decision (2026-05-29 with operator): **accept as the v2.1.24
floor**, ship as production-stable, revisit when there is fresh
information about BPQ's L2 LINKTABLE behaviour for AXIP peers.

Empirical timeline of the iteration:

| Version | Effective cycle | Trigger characterised |
|---------|-----------------|------------------------|
| v2.1.13 + transit ON  | ~ 90 min  | PCF DM-cycle on transit advert content |
| v2.1.23 + transit OFF | ~ 2.5 h   | PCF AXIP-side idle behaviour |
| v2.1.24 + 30 s KA     | ~ 3 h     | BPQ-side LINKTABLE recycle on AXIP port |

The remaining trigger is a **BPQ-internal LINKTABLE recycle**
specifically for the AXIP port to IW2OHX-12 (`192.168.1.201:10075`).
When BPQ recycles its LINKTABLE entry under us, the next CE frame
arrives on the freshly-allocated slot, our session-table lookup
misses, the new-slot branch of `FlexNet_InitSession` allocates a
fresh slot and emits INIT — and PC/Flexnet reseeds its link-cost
ring on every received INIT. The v2.1.14 reaper hysteresis,
v2.1.15 proactive-scan guard, and v2.1.16 reaper-time
LINK-migration scan all close subsets of this race but don't
eliminate it.

Possible follow-on directions (none in flight):

1. **L2Code.c LINKTABLE-recycle audit.** The 13 `CLEAROUTLINK`
   call sites in `L2Code.c` cover N2-retry exhaustion (XID/SABM/
   DISC), FRMR, L2KILLTIME idle, etc. Identify which path fires
   for our AXIP IW2OHX-12 link at the 3 h interval and either
   suppress it or hook into it cleanly enough that we can migrate
   our session without sending a fresh INIT.
2. **flxnod32.dll RE deeper.** PCF V4's L2 timeout state machine
   is `fcn.10002aa0` (≈ 6 KB) with a 21-case switch dispatch
   table at `0x100043a4`. The "infobox timeout: %d minutes"
   threshold lives at `[0x10020f4c]`. Reverse-engineer the
   per-link counter (`[esi+4]` in the disasm) and identify
   whether PCF exposes any sysop command (none visible in the
   `flxnod32.dll` strings we dumped) or PE config to extend the
   threshold for AXIP peers.
3. **Match xnet's per-peer activity pattern more closely.**
   v2.1.24 raised our KA cadence to 30 s for PCF peers; xnet
   peers also emit periodic STATUS+ route records every ~21 s.
   Sending similar status frames to PCF would risk the
   "PCF DMs on unsolicited record" behaviour flexnetd v0.7.8
   documented — needs careful timing per PCF's token state.

For deeper context, the full investigation captures + decoded
wire traces + r2 RE notes are in
[[project_pcf_axip_disc_cycle]].

---

## v2.0 GA — outstanding items

Both GA items are now shipped — v1.9.8 closed item #1 and v1.10.0
closed item #2. The repo is feature-ready for the v2.0 tag.

### 1. `CE-UNKNOWN` investigation + parser entry — _shipped in v1.9.8_

Shipped on 2026-05-14 (v1.9.8). The previously-unclassified 3-byte
`"12\r"` frame is now part of a recognised `"1n\r"` (n=1..9) status
family, handled by the new `CE_FRAME_STATUS_1N` classifier as a
benign status notification.

What v1.9.8 did:

1. Added `CE_FRAME_STATUS_1N` enum + parser match in
   `flex_parse_ce_frame` for the 3-byte shape `'1' [1-9] '\r'`.
   `"10\r"` keeps its existing dedicated `CE_FRAME_STATUS_10` entry.
2. New `case CE_FRAME_STATUS_1N` in the `FlexNet_ProcessCE` switch
   logs `CE-STATUS-1n: from=<peer> digit=<n>` (under debug builds,
   via `FlexNet_Log`) and returns without further action — the
   wire-level behaviour is unchanged from the previous default
   branch.
3. The default `CE-UNKNOWN` branch is kept in place for any
   genuinely new frame shape future peers may emit.

Phase 1 inventory was satisfied by the prior multi-day debug
capture (see project memory): only `"12\r"` was observed on the
wire; the generic classifier covers the entire `1n` family without
needing per-digit handlers (Option A — see project memory). If a
new digit ever appears in a future debug-build capture, it surfaces
as a `CE-STATUS-1n` line with the digit identified, and the
operator can decide whether per-digit semantics need encoding.

Acceptance: met. `"12\r"` is no longer flagged as `CE-UNKNOWN`; it
is classified, named, and intentionally treated as benign.

### 2. SSID-range internal application binding — _shipped in v1.10.0_

Shipped on 2026-05-15 (v1.10.0). Operators declare an SSID range
in `bpq32.cfg` with one new directive:

```
FLEXNETSSIDRANGE 0-8
```

What v1.10.0 does:

1. A new `FLEXNETSSIDRANGE N-M` directive is parsed at FlexNet
   init time (lazy first-call from `FlexNet_InitSession`, since
   stock LinBPQ doesn't invoke FlexNet's own init hook).
2. The FlexNet CE INIT handshake declares `max_ssid = M` to peers
   (instead of the node's own SSID). Without this, xnet clamps
   incoming route adverts to the originator's declared max_ssid,
   which is why the range used to collapse to `(0-0)`.
3. The compact route record sent to peers encodes `ssid_lo = N,
   ssid_hi = M` so the cloud sees a single line, e.g.
   `IR2UFV  0-8  1`, instead of N separate per-SSID entries.
4. Inbound connects to MYCALL-N (N in the range) are dispatched
   by BPQ's existing `APPLICATION` mechanism — bound SSIDs (e.g.
   `APPLICATION 1,BBS,,IR2UFV-8,...`) reach their app, the node
   SSID reaches the command parser, and unbound intermediate
   SSIDs refuse cleanly. No new dispatch code was needed.

Verified live on iw2ohx-gw running a second IR2UFV instance
configured with `FLEXNETSSIDRANGE 0-8` and `APPLICATION 1,BBS,,
IR2UFV-8,UFVBBS,255`:

- `C IR2UFV-8` from xnet IW2OHX-4 (direct neighbour) → BBS.
- `C IR2UFV-8` from xnet IW2OHX-14 (direct neighbour) → BBS.
- `C IR2UFV-8` from production IW2OHX-13 (FlexNet path via -4) → BBS.
- `C IR2UFV-8` from IR2UFV's own BPQ console (local loopback) → BBS.

xnet's `D IR` shows `IR2UFV  0-8  cost=1` — the range encoding
works on the wire.

Future expansion: add another `APPLICATION 2,CHAT,...,IR2UFV-7,...`
line in `bpq32.cfg` and the cloud immediately reaches that app via
`C IR2UFV-7` (the SSID is already in the advertised range).

---

## Out of scope for v2.0 GA

The following items were considered earlier in the v1.9.x cycle and
are deliberately **not** on the GA path:

- **Transit-role re-advertisement (the reverted v1.9.4 mechanism).**
  Would need a re-design that preserves AX.25 V2 reciprocity (e.g.
  NetROM L3 forwarding rather than L2 digipeat). linbpq-flexnet is
  staying non-forwarding for v2.0 — operators who need a transit
  router run one of the three real FlexNet routers: **(X)Net**,
  **PC/Flexnet**, or **RMNC/Flexnet**. *(Superseded in v2.2.0, which
  adds opt-in transit scoped to direct neighbours.)*
- **Route withdrawal on `via_session_idx` failover.** Was paired
  with transit advertising; without that, nothing to withdraw.
- **Periodic RTT=0 TX refresh marker.** Also tied to advertising;
  not needed when nothing is re-advertised.
- **P2 #9 capacity resize (64 → 256 destinations).** The current
  64-slot table has been sufficient under live load. Listed as a
  quick-win instead.
- **Multi-day soak as a gating item.** Soak runs naturally
  in production usage; not a formal GA blocker.

### Code portability into `flexnetd`

The original plan was to factor the shared protocol surface
(CE type-6/7 build/parse, QSO allocator, probe table, L3RTT
counters, IIR filter) into a `flexnet_l3_proto.c` consumed by both
`linbpq-flexnet` and `flexnetd` (the Linux-daemon sibling project —
itself not a real FlexNet router; the three real routers are
(X)Net, PC/Flexnet, RMNC/Flexnet). This is set aside — not part of
v2.0 GA. If it ever happens it would be a sibling effort across
both repos, not a deliverable here.

---

_Document version: 2026-05-28 — v2.1.28 in production (both IW2OHX-13
and IR2UFV). The PC/Flexnet compatibility stack across the v2.1.x
line:_

- _v2.1.0 — CTEXT suppression on F-flagged inbound SABM + 201-byte
  KA shape accepted on receive._
- _v2.1.6 — removed the spurious leading `"3+\r"` from outbound
  route batches._
- _v2.1.7 — proactive-init scan filtered to peer-to-peer FlexNet
  sessions only._
- _v2.1.8 — single-digi MYCALL on direct-neighbour `C <call>`._
- _v2.1.11 — route emission moved to CE_FRAME_INIT trigger;
  per-flavour records-per-emit cap._
- _v2.1.13 — outbound CE link-time replies rate-limited per peer
  flavour (≥ 320 s for PC/Flexnet, ≥ 20 s for (X)Net) to land in
  PC/Flexnet's expected-reply window. Closes the link-cost
  saturation-at-4095 symptom that survived v2.1.10–v2.1.12._
- _v2.1.14 — session-reaper hysteresis. A single transient
  `L2STATE != 5` observation no longer destroys a live FlexNet
  session slot; bad state must persist for 3 consecutive
  `FlexNet_Timer` ticks. Closes the ~90-min spurious session-reset
  cycle observed on IR2UFV ↔ IW2OHX-12 in v2.1.13._
- _v2.1.15 — proactive-CE-init guard. An established session
  (`got_peer_init == TRUE`) is no longer re-handshaked when some
  BPQ-internal path clears `LINK->FlexNetLink`; we just re-promote
  the flag. Closes the residual ~80-min session-reconnect path
  that survived v2.1.14._
- _v2.1.16 — LINK-migration second chance. Before the reaper
  destroys a bad-state session it scans for a fresh LINKTABLE
  slot with the same callsign on the same port (BPQ may have
  recycled the old slot via `CLEAROUTLINK`); if found, the
  session is migrated to the new LINK pointer without re-INITing
  the peer. Closes the residual `session started` (new-slot)
  cycle that survived v2.1.14+v2.1.15. `peer_callsign` field
  added to `FLEXNET_SESSION`._
- _v2.1.18 — AX.25-aware callsign equality (mask SSID byte to bits
  4..1). Insufficient: still failed on IR2UFV ↔ IW2OHX-12 with a
  second session-start sending INIT. Superseded by v2.1.19._
- _v2.1.28 — bring PCF cost-row back to single digits.
  v2.1.24's 30 s KA cadence kept the link stable but PCF's
  per-sample math `sample = KA_arrival − (smoothed+4)*32` was
  producing ~100-tick samples (because we still advertised
  `FLEXNET_WIRE_LT = 2` → `link.ts` window = 19.2 s, our KA
  at 30 s → sample ≈ 108 ticks ≈ 10 s in PCF's L *).
  v2.1.28 advertises `FLEXNET_WIRE_LT = 5` → `link.ts` =
  28.8 s, and drops the KA threshold from 30 → 29 s, landing
  samples at 1-2 ticks (matching IW2OHX-4 and IW2OHX-14's
  cost=1/1 steady state)._
- _v2.1.27 — drop non-CE/CF PIDs on FlexNet-flagged links. Wire
  evidence 2026-05-30: PC/Flexnet sends a 7-byte PID=F0 frame on
  our FlexNet link, BPQ's L4/sysop layer interprets it as a user
  connect and echoes a 60-byte banner back; PCF receives the
  PID=F0 reply and DISC's the link 22 ms later. This was PCF's
  double-DISC pattern that v2.1.25/v2.1.26 couldn't catch via
  SABM-accept adoption — the second DISC fired regardless. Now:
  on a FlexNet-flagged LINK, non-CE/CF PIDs are dropped at L2
  level (after the L2 RR ack) so the banner is never sent and
  PCF doesn't see the wrong-PID trigger. Non-FlexNet links keep
  the original AX.25 V2.0 behaviour._
- _v2.1.26 — fix v2.1.25 skip-self bug. The skip-self check
  (`sess->LINK == new_link continue`) missed the case where BPQ
  reuses the same LINKTABLE memory slot after CLEAROUTLINK. Our
  session's `.LINK` pointer was unchanged across the recycle
  (slot reused), the check fired, no adoption, fresh INIT was
  sent. Removed the skip; adoption now handles both fresh-slot
  and reused-slot cases. Console message distinguishes the two
  for forensics._
- _v2.1.25 — adopt existing session on SABM-accept for PCF
  L2-cycle pattern. When PC/Flexnet runs its periodic DISC/SABM
  cycle, BPQ's CLEAROUTLINK + fresh-LINK allocation no longer
  triggers a fresh CE-INIT and PC/Flexnet ring reseed. The
  SABM-accept hook now calls `FlexNet_TryAdoptSession` first; if
  an active session for the peer's callsign exists, the LINK
  pointer is migrated in place (preserving got_peer_init /
  sent_routes / peer_max_ssid / peer_ka_term)._
- _v2.1.24 — per-peer-type proactive KA cadence. PC/Flexnet
  (peer_ka_term=='\r') gets KA every 30 s; (X)Net peers stay on
  300 s. Wire-capture evidence on iw2ohx-bpq 2026-05-29 showed PCF
  exchanges KAs with IW2OHX-4 every 16-32 s but with us every ~5 min
  — PCF cycles AXIP peers that go too quiet. Mimicking xnet's
  ~21 s cadence should keep PCF satisfied._
- _v2.1.23 — REVERT the v2.1.17→v2.1.22 INIT-cooldown stack.
  Wire-trace evidence (IR2UFV ↔ IW2OHX-12, 2026-05-28) showed
  PC/Flexnet intentionally `DISC+`/new-`SABM+` cycles the L2 link
  after every token-handover round and expects a full
  INIT→INIT→RTT→routes handshake on each new L2 session.
  Suppressing our INIT made PCF rebuild the peer entry with
  default `max_ssid=15`. v2.1.23 reverts: every InitSession path
  unconditionally emits INIT+KA on a fresh session, exactly like
  v2.1.16. The periodic `600 4095` cost-ring reseed is accepted
  as PCF's normal protocol behaviour — v2.1.13's LT rate-limit
  re-converges the ring to `2/2` within ~5 min after each reseed.
  All v2.1.14 (reaper hysteresis), v2.1.15 (proactive-scan guard
  for same LINK), and v2.1.16 (LINK-migration) fixes remain in
  place to handle the BPQ-internal LINK-recycle paths that don't
  involve a PCF DISC/SABM cycle._
- _v2.1.21 — drop the over-eager cooldown-clear on peer-INIT
  receive. v2.1.17's `flex_clear_init_history()` fired on every
  CE-INIT from the peer — including the routine handshake reply
  to our own outbound INIT, which immediately wiped the cooldown
  we'd just recorded. Diagnostic build v2.1.20 caught it on the
  wire (next session-recycle's cooldown lookup found a matching
  entry but with `last_tx == 0` → age ≈ 56 years → cooldown read
  as expired → INIT sent → PCF reseeded). v2.1.21 removes the
  clear. A peer that truly restarts no longer triggers an
  immediate re-INIT from us; the link survives on KAs alone until
  the natural `FLEXNET_INIT_TX_INTERVAL` cooldown expires._
- _v2.1.19 — callsign cooldown lookup via `ConvFromAX25`-normalized
  string. The 7-byte AX.25 representation of `LINKCALL` varies
  across BPQ code paths (L2Code.c:1059 masks byte 6 to 0x1E;
  L2Code.c:4823 masks to 0xFE; L2Code.c:2033 doesn't mask). The
  cooldown table now stores the human-readable callsign (e.g.
  `"IW2OHX-12"`) and compares via `strcmp`, bypassing every
  byte-level inconsistency. Same normalization applied to v2.1.16's
  reaper-time LINK-migration scan._
- _v2.1.17 — persistent per-peer INIT cooldown. Outbound CE-INIT
  is now rate-limited to once per (callsign, port) per
  `FLEXNET_INIT_TX_INTERVAL` (3600 s). Suppresses the reseed that
  occurs when v2.1.16's migration scan loses the BPQ race and a
  session is recreated via the new-slot path. The cooldown is
  cleared when the peer itself sends us a fresh CE-INIT (peer
  state was reset → we should reciprocate). History table lives
  outside `FlexNetSessions[]` so it survives reaper/recreate
  cycles._
