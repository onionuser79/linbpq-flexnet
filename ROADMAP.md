# linbpq-flexnet — Roadmap

**Current: v2.2.4** (2026-09-28) · both nodes · LinBPQ baseline 6.0.25.41 (`4b7a47b`)

Everything shipped so far makes this node a **correct FlexNet participant**.
Everything still open makes it a **useful FlexNet router**. That is the whole
plan in one sentence; the rest of this document is what stands between the two.

---

## Where we are

| | IR2UFV (test bed) | IW2OHX-13 (production) |
|---|---|---|
| version | v2.2.4 (6.0.25.41) | v2.2.4 (6.0.25.41) |
| build | `flexdebug` (83 `FlexNet: ` strings) | silent, `-DFLEXNET_PROD=1` (1) |
| role | router | **router** since 2026-09-21 |
| `FLEXNETTRANSIT` / `L2TRANSIT` / `PATHFORWARD` / `LT3BYTE` | YES | YES |
| `DIGIFLAG` on the AXIP port | 1 | 1 |
| `FLEXNETSSIDRANGE` | `0-8` | **`13-13` — must not be aligned** |
| FlexNet peers | `-14`, `-12` (PCF) | `-14` |

`IW2OHX-13` shares its base call with `-1/-4/-12/-14/-15`, so `IW2OHX (0-8)`
would claim nodes it does not own. Rollback to leaf: `sudo bash
/tmp/rollback-prod-leaf.sh` on gw — the binary stays, only the role reverts.

**Advertisement scope is derived, not configured.** `flex_advertise_direct_only()`
returns `!g_flexnet_l2_transit_enabled`: with `FLEXNETL2TRANSIT NO` a node
advertises its own calls and its *direct* neighbours only; with `YES` it
advertises every learned destination, because L2 forwarding can carry them.
Both nodes run `YES` (verified in the live cfgs 2026-09-25), so **both advertise
multi-hop today**. The coupling exists because advertising a destination we
cannot carry is a black hole — it made 67 of them once already.

---

## Open work at a glance

```
 v2.2.4 ── L2 frame routing hardened, both nodes ────────────────────────────►
   │
   ├─► v2.3   local APPLICATION calls        know ▓▓▓▓▓  build ░░░░░
   │          unlocks: advertise SR4BBX-style app calls, not just own SSIDs
   │          gate: none — designed, 0 open questions, external validator ready
   │
   ├─► v2.4   per-link routing options       know ▓▓▓▓░  build ░░░░░
   │          unlocks: per-link transit scope (- > ! =) + tunnel penalty (+)
   │          gate: 3 things to measure on (X)Net first
   │
   └─► ✔ MILESTONE  FlexNet L2 frame routing know ▓▓▓▓▓  build ▓▓▓▓▓
              v2.2.0  digi-chain rewriting ⇒ multi-hop advertised
              v2.2.4  per-circuit hop pin, teardown lifecycle, loop drop
                      — field-verified, both nodes 2026-09-28
              closed from 11 days of wire: 2nd ingress shape (never sent),
                      egress cross-check ((X)Net ×2 + PCF match ours)
              deferred: cross-port circuits — trigger: a 2nd FlexNet port
              candidate: per-hop acknowledgement, as (X)Net does
```

| # | Item | Size | Depends on | Risk if wrong |
|---|------|------|-----------|---------------|
| ✔ | **L2 frame routing** — **done in v2.2.4**. Left only the deferred cross-port case and the per-hop-ack candidate | — | a 2nd FlexNet port / an I-frame-loss capture | — |
| 1 | **v2.3** local `APPLICATION` calls | small | nothing | advertising an unbound call = black hole |
| 2 | **v2.4** per-link options | medium | 3 measurements | operator mis-scopes a link, silently |

The two open releases are independent and can ship in any order. v2.3 is the only one
that is pure gain with no new failure mode of its own, and it is the only one
somebody outside the station is waiting for — so it goes first.

**Knowledge is not the constraint any more.** For most of this project the
blocker was "we do not know what the wire does". Today only v2.4 carries open
questions, and they are three cheap observations on a node we control.
Everything else on this page is implementation risk.

---

## ★ MILESTONE — FlexNet L2 frame routing

**The single most important piece of work after GA**, because it is what makes
a multi-hop advertisement honest. **The core shipped in v2.2.0** (`435ea5b`,
`b6ab4cd`, 2026-09-17); **the hardening shipped in v2.2.4** (2026-09-28, both
nodes). The milestone is complete apart from one deferred sub-case.

**FlexNet is a link-layer routing network, not a NetROM overlay.** For a
destination many hops away, (X)Net sends the *same* two-digi AX.25 chain it
uses one hop out (`<peer>* <us>`) and expects the neighbour to forward it at
L2. Zero PID=CF frames across every attempt — RFC §4.3's CREQ premise is wrong
for an (X)Net peer (it may still be right for a BPQ/linbpq peer; that path has
never been exercised and is a much smaller job). Stock LinBPQ can only
digipeat by address — the chain is then consumed with the destination still
remote, `*** link failure` — or hand off to NetROM L3/L4, which (X)Net never
uses. Neither carries a multi-hop FlexNet destination.

Both halves of the mechanism are captured and both primitives ship:

| Settled | How | Shipped as |
|---|---|---|
| **Transit = symmetric digi-chain rewriting** (2026-09-17) — forward: set own H-bit, append next hop; reverse: remove what we appended, set own H-bit | captured **on PCF `-12` itself** while it forwarded a session | `FLEXNETL2TRANSIT` → `FlexNet_L2Transit()`, hooked in `L2Code.c` before the stock `Digipeat()`. Also switches the advertisement scope to every learned destination |
| **A CE type-6 is a traversal, not a query** (2026-09-18) — a node that cannot finish it inserts its own next hop and passes it on; the chain says who asked, so there is no per-traversal state | separate capture, answered a destination we never could before | `FLEXNETPATHFORWARD` |

`flexnetd/PROTOCOL_SPEC.md` §5.1 had ruled digi-chain rewriting **illegal**,
which is why it was never built, and **v1.9.4 failed because it did the
extension without the contraction**. §5.2 documents it now.

**Verified:** `IW2OHX-14 → IR2UFV → IW2OHX-4 → IQ2LB-6` (`C IQ2LB-6 IR2UFV`
returns the DXSpider banner), and real third-party frames forwarded with no
test traffic (`L2FWD IW7TY-15->IW2OHX-13 via IW2OHX-4`). `FL` shows the
`extended` / `contracted` / `declined` counters — compare deltas, never
cumulative values.

### What ships today, and its limits

| Piece | Implemented as | Limit |
|---|---|---|
| Ingress | we are the **last, unrepeated** digi and the destination is in `FlexNetDests[]` via a live session (`flex_session_for_call()` heals a stale `via_session_idx`) | a frame arriving with its chain **already consumed** and a remote destination is not handled |
| Egress | append the next hop's callsign as a new unrepeated digi; plain digipeat when the next hop *is* the destination | wire shape taken from the PCF capture; not yet cross-checked on a dual-port capture of two (X)Net nodes carrying transit for a third |
| Reverse path | `FlexNetL2Transit[]`, **128** slots keyed on (user, dest, **port**). **v2.2.4:** the next hop is **pinned** on the circuit's first frame and kept while its FlexNet session lives; the replaced pin, if any, stays contractible (`prev_appended`). Only a hop we recorded appending is ever removed | key still assumes both sides of the circuit are on the **same port** (true on the single AXIP port today); one slot per (user, dest) pair |
| Slot lifetime | **v2.2.4:** from the control byte — DISC opens the teardown, its UA (or any DM) closes it, the slot lingers **120 s** for retransmits. An open circuit keeps its slot for **2 h** of silence (was 900 s) and is reclaimed after 900 s idle **only** when the table is full, closed slots first; a live one never | — |
| Loop / TTL | never append a call already in the chain (doubles as split-horizon); **v2.2.4:** a frame on a circuit we forward that already carries our own call repeated is dropped (`L2FWD-LOOP`); `PORTMAXDIGIS` and AX.25's 8-digi ceiling | the 8-digi ceiling is still FlexNet's only hop limit |

**v2.2.4-rc1 field test** (`research/l2_circuit_2026-09-28/`): `-4 → IR2UFV
→ -14 → -13` with every forward frame extended and every reply contracted, the
slot freed 120 s after the teardown, and a reconnect re-resolving its hop.
The test needed `-4` re-peered with IR2UFV for the window: on the normal test
bed every drivable session enters at `-14`, which (X)Net and PC/Flexnet keep in
the chain, so the next hop is always already there and the loop guard
(correctly) declines. **A mid-circuit route change is unit-tested only** —
nothing live could be made to re-route inside a session.

### Status of the four hardening items (2026-09-28)

Items 2-4 were settled from **eleven days of captures already on disk**
(`research/l2_circuit_2026-09-28/WIRE_ARCHIVE.md`, ~200 000 inbound frames
incl. a full day of production transit) — no new capture was needed.

1. **Reverse-path state** — **done in v2.2.4** (pin + lifecycle). One
   sub-case **deferred, deliberately**: a circuit whose two sides are on
   **different ports**. Every FlexNet peer of both nodes sits on the one AXIP
   port, so it could not be exercised once, and frame-plane code that has
   never run is the kind that corrupts a stranger's session. The build is
   small when it is needed — stock `Digipeat()` already takes a `toPort` (the
   hook passes 0), and the circuit key would carry both ports. **Trigger:** a
   second port with a FlexNet peer on either node.
2. **The second ingress shape** — **closed, not built.** Across eleven days no
   peer, (X)Net or PC/Flexnet, sent a connected-mode frame with a consumed
   chain and a remote DEST: 13 candidates, all a delivery to a local user or a
   stray DM. Peers always name the next hop as a pending digi.
   `tools/ingress_shapes.py` counts the shape if it ever appears.
3. **Egress cross-check** — **closed.** Two (X)Net nodes transiting mid-chain
   (`-14`: `[IW2OHX-3* IW2OHX-14* IR2UFV]`, 73 frames; `-4`: 53) and PC/Flexnet
   (122) emit exactly what `flex_l2_append_digi()` emits, and the reverse
   frames they return carry our hop where `flex_l2_is_our_hop()` removes it.
4. **Loop safety for the frame plane** — **done in v2.2.4, and now
   grounded**: 92 real forward frames bounced back to us (`[IR2UFV* IW2OHX-4*
   IR2UFV]` ×68, via PC/Flexnet ×18, via `-14` ×6). Followed end to end, the
   looped circuit never completed — the originator retried its SABM for 45 s
   and gave up — so the `L2FWD-LOOP` drop costs nothing.

**Released as v2.2.4** on both nodes, 2026-09-28: production 08:21Z (silent),
IR2UFV 08:26Z (`flexdebug`).

### Candidate — per-hop acknowledgement (not on the milestone)

Captures show **(X)Net terminates L2 at each hop**: `-14` acknowledged our
I-frames in 0-1 ms, addressed as the destination, while the far end's replies
took 41-48 ms (`xnet_hop_ack_via_14.txt`). We digipeat end to end, which is
legal and works, but a user through us pays the whole path's round trip in
every T1. Large change (per-hop link state in the transit path); needs its own
capture of an I-frame loss through (X)Net before any design.

Evidence: `research/l2_forwarding_2026-09-17/`, `research/path_query_2026-09-18/`,
`research/l2_circuit_2026-09-28/`.

---

## v2.3 — local `APPLICATION` callsigns as FlexNet destinations

GitHub issue [#1](https://github.com/onionuser79/linbpq-flexnet/issues/1),
**Tom SQ4BJA** (SR5DDD / SR4DON, AXUDP FlexNet links to SR6DWH-11 and
SR1DSZ). Accepted 2026-09-20; design confirmed by Tom on the issue.

`FLEXNETSSIDRANGE` only covers SSIDs of the node's own base call, so a node
whose applications use unrelated callsigns cannot advertise them at all:

```
NODECALL=SR4DON
APPLICATION 1,FBB,,SR4BBX,OLNBBS,255              ; unreachable from the cloud
APPLICATION 3,DX,ATTACH …,SR4DXC,DXCLUS,255       ; unreachable from the cloud
```

**It is cheap because the receive half already works.** `L2Code.c:537-575`
matches an inbound SABM against every `APPLCALLTABLE[]` entry — full callsign,
not just SSIDs of `MYCALL` — gated only on the port's `PERMITTEDAPPLS`. A
frame for `SR4BBX` with its digi chain consumed is delivered to FBB *today*.
Same result as v1.10.0's SSID range: **advertisement-side only**.

| # | Build | Where |
|---|---|---|
| 1 | `FLEXNETLOCAL <CALL>[-SSID]`, repeatable, 16 slots, base call ≤ 6 chars (`%-6.6s` record field) — plus `FLEXNETLOCALAPPS YES` to auto-walk `APPLCALLTABLE[]`. **Both ship**: auto for the common case, explicit for a node that wants to advertise *less* than it binds | `flex_load_config()`, beside `flex_parse_ssidrange_line()` |
| 2 | Emit them at rtt=1 — `flex_send_own_routes()` becomes a multi-record frame built the way `flex_advertise_drain()` already does it: **one `'3'` per frame**, records via `flex_build_route_rec()`, single trailing `'\r'` | `flex_send_own_routes()` |
| 3 | **Answer path queries for them** — the easy half to miss. `flex_target_is_us()` compares against `MYCALL` only, so a type-6 for `SR4BBX` falls through and the peer sees no route to something we just advertised | `FlexNetCode.c:3214` |
| 4 | Scope guards — local entries are not transit: advertise regardless of `FLEXNETTRANSIT` / `flex_advertise_direct_only()`, never enter `learned[]`, never hold-down | advertisement walk |
| 5 | **Don't create black holes** — validate each entry against `APPLCALLTABLE[]` at init; unbound ⇒ loud warning + skip the record | init |
| 6 | Flag local entries in `FL` / `D` so an operator can see what the node claims | `Cmd.c` |
| 7 | Unit tests (`tools/unit/`, extract-from-source): N local calls into one frame, `flex_target_is_us()` vs the local list, unbound entry rejected | — |

**Risk: low.** Additional compact records of a shape we already emit. The one
interaction to watch is a `FLEXNETLOCAL` whose base call equals `NODECALL` —
reject it with a pointer to `FLEXNETSSIDRANGE` rather than emitting a
duplicate row.

**Validation.** IR2UFV with a distinct application call bound, checked from
(X)Net `-4` and `-14`: `D <call>` at cost 1, `C <call>` reaches the app, and
the type-7 answer carries a chain ending at it. Production stays untouched
until that passes. Then a **second, independent field validation on SR4DON**,
offered to Tom and accepted — a different implementation's worth of real
peers is worth more than two instances sharing one gateway, one operator and
one set of habits. Don't close the issue before that run reports back, and
remind Tom to check `PERMITTEDAPPLS` on the FlexNet port first: the mask must
include FBB and DX or the connect is refused *after* we advertised it.

<details>
<summary><b>Interim workaround for operators hitting this today</b> (and the correction to what was first posted on the issue)</summary>

Move the application onto an SSID of the node call and widen the range. It
trades away exactly the identity the feature is about, but it keeps the
service reachable:

```
APPLICATION 1,FBB,,SR4DON-8,OLNBBS,255     # was SR4BBX
FLEXNETSSIDRANGE 0-9
```

**The first answer posted on the issue was wrong** and was corrected there on
2026-09-23. It suggested a *second* `APPLICATION 1` line carrying `SR4DON-8`
alongside the `SR4BBX` one. `config.c`'s `ProcessAPPLDef()` resolves the
leading number to `xxcfg.C_APPL[n-1]`, a single fixed slot, so the second line
silently **overwrites** the first line's `ApplCall` — and because the field is
written with `memcpy`, a shorter replacement leaves trailing characters of the
old callsign behind. One `APPLICATION` number carries exactly one `APPLCALL`.

The only way to give an application a second L2 identity is the 7th field,
`L2ALIAS` (`L2Code.c:577`, via `CompareAliases`):

```
APPLICATION 1,FBB,,SR4BBX,OLNBBS,255,SR4DON
```

`CompareAliases` ignores the SSID, so this claims *every* SSID of `SR4DON` the
node did not already answer, tried in application order — it works for exactly
one application and would swallow the SSIDs any other application wanted.
Usable for a single BBS; another reason to build the feature properly.
</details>

---

## v2.4 — per-link routing options

The first item that gives an operator **policy** control over FlexNet routing
rather than an on/off switch, and the piece that makes `FLEXNETTRANSIT` safe
on a node with a mixed set of links. Independent of v2.3 and of the milestone.

**Source: the (X)Net 1.38 manual §4.3.24.3.1, page 37** —
<https://xnet.swiss-artg.ch/pdf/xnet138.pdf>. Documented behaviour, not
inference: the rare case where we implement against a written contract.

### The options

| `<opt>` | Effect |
|---|---|
| *(none)* | Partner **and** its subnet forwarded. Today's only behaviour. |
| `-` | Partner **not** advertised; its subnet **is**. For test attachments. |
| `>` | Advertise **neither**. For internal house networks. |
| `!` | Advertise the **partner only**, not what is behind it. |
| `)` | Link row hidden from non-sysop users. **Display only.** |
| `+` | Partner and subnet degraded by **+2000** run-time points (≈ 200 s). For Internet links. |
| `=` | `!` **plus**: send this peer no destinations at all. |

`!` and `=` share inbound semantics and differ only outbound — `=` makes the
link one-way. That distinction is easy to lose when skim-reading the table and
it is the whole reason both exist.

### Why it matters here

- **`+` independently confirms our wire unit.** 2000 points ≈ 200 s puts one
  run-time point at **100 ms** — exactly what `flex_build_route_rec()`
  (`FlexNetCode.c:5779`) emits. A manual printed years before this project
  agreeing with a value we derived from captures is worth writing down.
- **`>` and `=` are the missing scope guard.** `FLEXNETTRANSIT` is node-wide.
  An operator with one RF neighbour and one Internet tunnel has no way to say
  "carry the RF side, keep the tunnel private" short of turning transit off.
- **`+` is the honest answer to a tunnel.** Our cost is a measured link-time,
  so AXUDP-over-fibre beats 1k2 RF to the same destination in
  `flex_expected_rtt()` — right for latency, wrong for policy.

### Config shape and where each option lands

Our FlexNet links are an `F` flag on an AXUDP `MAP` entry
(`bpqaxip.c:2402` → `arp_table[].FlexNetFlag`), so the option is a suffix
there — (X)Net's vocabulary, policy next to the link it governs:

```
MAP IW2OHX-14  44.134.24.4    UDP 10093  F     ; unchanged — full transit
MAP IZ2XYZ-7   10.8.0.9       UDP 10093  F+    ; Internet tunnel, +2000
MAP IW2OHX-12  192.168.1.144  UDP 10093  F>    ; house network, invisible
MAP DB0XYZ-1   44.225.1.1     UDP 10093  F=    ; one-way: tell it nothing
```

`_stricmp(p_UDP,"F")` becomes a prefix match, remainder parsed as an option
**set** (`F+)` is meaningful), bare `F` unchanged. An unrecognised character
is a **loud warning and the link still comes up with default policy** — the
failure mode of a dropped `MAP` line is a dead node, not a mis-scoped one.
Store as a bitmask beside `FlexNetFlag`, expose via
`FlexNet_GetPeerLinkOpts()` built next to `FlexNet_IsPeerFlexNetMapped()`
(`bpqaxip.c:3432` — same lookup, plumbing already written), cached into
`FLEXNET_SESSION` at `FlexNet_InitSession()` time (the struct is memset there,
so the cache cannot survive a reconnect stale).

Every option is a gate on a path that already exists — no new wire format, no
new frame type. That is why this is v2.4-sized and not a milestone.

| Option | Hook | Notes |
|---|---|---|
| `+` | `flex_dtable_merge()` (`:5440`), after the RTT=0 skip, before `flex_learned_add()` | **At ingest, deliberately** — it degrades our own route selection too, which is what *verschlechtert* means. ⚠ The 4095 wire clamp eats ordering above 2095; ⚠ never add 2000 to the `60000` withdrawal sentinel. |
| `-` `!` `>` | `flex_advertise_walk_for_peer()` (`:6609`) — it already iterates sessions as sources and already has `is_direct_neighbour` at `:6625` | Three combinations of a two-bit (partner, subnet) decision. Must also gate `flex_advertise_neighbours()` (`:6873`) and `flex_advertise_seed_peer()` (`:6845`) or a suppressed neighbour reappears on the 120 s refresh. |
| `=` | early return in `flex_advertise_check()` (`:6163`), beside the PCF-quiesce gate | The only option that gates the peer as a *destination* of advertisement. **Our own record still goes out** — otherwise the peer cannot reach us and the link is pointless. Say so in the README: it is the one place our reading of the manual is a choice, not a translation. |
| `)` | `FlexNet_CmdLinks()` (`:5040`), `FlexNet_CmdDest()` (`:4633`) — both get `TRANSPORTENTRY *Session`, so test `Session->Secure_Session` | **Changes no routing.** Must not touch any advertisement path. |

**Composition rules.** `FLEXNETTRANSIT NO` still wins — per-link options
narrow transit, never widen it, so leave the existing
`if (!g_flexnet_transit_enabled) return;` guards first in every function.
direct-only scope (`flex_advertise_direct_only()`) ∩ `!` = `!`; no special case, but pin it with
a unit test so a later change to either does not quietly widen the other.
**`>` must not create a phantom**: withdraw once if we ever told this peer a
finite cost, *then* fall silent — otherwise adding `>` to a live link is the
rc4 phantom-destination failure mode, self-inflicted by a config edit.
Options are read at **config load only**; a runtime `FLEXNET LINKOPT` command
is not worth the live-state-mutation risk in the first cut.

### Three things to measure before building

The manual gives semantics, not wire behaviour. All three are observable on
`IW2OHX-14`:

1. **Does (X)Net *withdraw* on an option change, or just fall silent?** Add
   `>` to a live link and watch for an RTT=60000 record. The phantom risk
   above turns on this.
2. **Is `+` applied at ingest or at re-advertisement?** If at ingest, `+`
   changes which path (X)Net uses for its *own* connects — visible in its own
   `D <call>` cost. **The whole design above assumes ingest.**
3. **Does `<opt>` accept more than one character?** Our parser should accept a
   set regardless; (X)Net's answer does not constrain us either way.

### Validation

IR2UFV, one option at a time, from (X)Net `-4` and `-14` with `D` and `L`:

| Option | Pass condition at the peer |
|--------|----------------------------|
| `-` | partner absent from `D`; a destination behind it present |
| `>` | neither present; **and** no other peer's view changed |
| `!` | partner at cost 1; nothing behind it |
| `)` | non-secure `FL` omits the row; sysop session shows it |
| `+` | cost rises by exactly 2000 ticks, or sits at 4095 if it would exceed |
| `=` | `!` result, **and** the peer gains nothing from us but still lists us |

Production stays on default until every row passes — `-13` is a router now, so
a mis-scoped link there is visible to the whole cloud, not to a test bed.

**Risk: low on the wire, moderate in configuration.** Nothing new is emitted.
The real risk is operator error: `>` on the wrong link silently removes a
chunk of the cloud's reachability and will not look like a fault. Mitigate by
logging the resolved option set for every link once at init, at
`FlexNet_Info` level, **in plain words rather than as a bitmask**.

---

## Carried forward — open, not scheduled

| Question | State | Why it is not on the plan |
|---|---|---|
| Does PC/Flexnet **age our destinations out** between `3+` requests? | open | Needs a `D` on `-12` — a chained telnet across the live mesh, i.e. exactly the poller a quiet run exists to remove. Check once, deliberately, after the soak. Expect "no": its `3+` cycle *is* its refresh. |
| Does PCF treat **(X)Net's** pushes the way it treated ours? | open | `-14` pushed 2483 spontaneous record frames in 20.9 h and also peers with `-12`. We cannot see `-12 ↔ -14` from here. First thing to look at if the quiesce holds but the mechanism still feels under-explained. |
| **One `3+`-less teardown** in 32 | unexplained | Too few to characterise. Watch whether it recurs. |
| **`IW2OHX-4` flaps** against PCF too | out of view | Cut back to a single `-12` link on 2026-09-21, so neither of our nodes peers with it. Any `-4` figure in this repo predates that cut. |
| **Poison-reverse can undo itself** — peers echo our withdrawal and we re-learn it | contained, not solved | Made a count-to-infinity phantom in rc4. (X)Net cannot delete a learned destination; you starve it. `flex_climb_is_loop()` contains the ladder. |
| **Jitter floor + explicit →∞ hold-down** (RFC §13.3) | minor | The 43-of-204 geometric climb it was blamed for is closed by `flex_climb_is_loop()`; the drain being 19× too small was the real cost. |
| **RTT=0 semantics** (RFC OQ1) | open | Treated as "skip, don't advertise". Safe either way. |
| **CREQ / NetROM L4 transit toward a BPQ peer** (RFC §6, T40-T43) | never exercised | May well be right for a linbpq peer; small job, no demand yet. |

---

## Shipped

### v2.2.x — transit role and link stability

| Release | Date | What it closed |
|---|---|---|
| **v2.2.4** | 2026-09-28 | **L2 frame routing hardened.** The next hop is pinned per circuit — up to v2.2.3 a route change mid-circuit left returning frames with a digi the originator never sent. Slot life follows the AX.25 teardown (120 s linger, 2 h idle, eviction never takes a live circuit); frames that loop back are dropped; table 64 → 128; `FL` shows `circuits/repinned/looped/evicted`. 207 unit checks. Field-verified on IR2UFV through a temporary `-4` peering; the milestone's other items were closed from 11 days of captures (`research/l2_circuit_2026-09-28/`). Production cut over 08:21Z. |
| **v2.2.3** | 2026-09-28 | Rebase to LinBPQ **6.0.25.41** (`4b7a47b`). Upstream touched only `Cmd.c` among our overlays (new `NPING` NetROM-ping command); merged conflict-free, our delta reproduced exactly. The known upstream defects are **still present in 6.0.25.41** and still handled: `REBOOT()` null-deref dropped, `bpqaxip` format-string fix kept. No FlexNet-logic change. IR2UFV 06:54Z, production 07:06Z (silent build). |
| **v2.2.2** | 2026-09-22 | **The `-12` teardown, for real.** PCF accepts **at most 2** record frames after the `3-` closing a `3+` answer, then DISCs — 30/30, reacting within 0.06 s on a healthy L2. Not the content (the same record went out 614× harmlessly, 14× fatally) and not the `3-` placement (549 violations, 0 teardowns). Fix: `FLEXNETPCFQUIESCE` (default YES), scoped by `flex_peer_is_pcf()`. **Verified 0/3 on IR2UFV** against 30/30. Also pinned PCF's AXIP cycle as a **fixed 5445 s link lifetime, not an idle timeout** (4 for 4, to the second) and made the restart path re-seed in the same second. |
| **v2.2.1** | 2026-09-21 | `3+` answered with the **whole** table (`force=TRUE`; it had been running an explicit full-table request through the 10 % change filter — 3 of 204); end-of-batch requires a sustained empty queue; `flex_climb_is_loop()` with a **persisting** floor; wire clamp at 4095 (33 over-limit records in 10.9 h → 0 in 13.4 h). Production promoted from leaf to **router** the same day. |
| **v2.2.0** | 2026-09-19 | Transit role D1-D3 (`FlexNetAdvertised[]`, per-peer token buckets, poison-reverse + hold-down, `learned[]` ageing), `FLEXNETL2TRANSIT`, `FLEXNETPATHFORWARD` — all opt-in, all default NO. Plus **packed advertisements** (one `'3'` per frame; we were sending 15 of 236 `PACLEN` bytes — queue to PCF non-empty 80 % → 6 %, re-seed 17.6 min → under 100 s) and **no re-INIT on a healthy link**. First unit tests (`tools/unit/`, extract-from-source). |

### v2.1.x — PC/Flexnet compatibility and upstream rebases

| Release | Date | What it closed |
|---|---|---|
| v2.1.42 | 2026-09-14 | Version marker for the `ac38bd6` baseline. No logic change. |
| v2.1.41 | 2026-09-10 | Rebase to 6.0.25.40 + **3 upstream defects**: `REBOOT()` null-deref dropped, `bpqaxip` uninitialised-buffer-as-format-string fixed (reachable from a malformed AXUDP datagram), missing `-lbacktrace` added (G8BPQ fixed it identically in `ac38bd6`). |
| v2.1.40 | 2026-08-11 | Rebase to 6.0.25.36. Pure upstream compatibility. |
| v2.1.39 | 2026-07-09 | Stale `PENDING` when BPQ recreates the session mid-life and the peer's one-shot INIT is long past — `flex_est_inferred`. |
| v2.1.38 | 2026-06-03 | `FLEXNET_PROD=1` silent build (all 55 `FlexNet_Info` sites dead-code-eliminated). |
| v2.1.36 | 2026-06-02 | PCF sends FlexNet-shaped INFO with **PID=0xF0**; v2.1.27's non-CE/CF drop was swallowing it. CE-shape probe before the drop. |
| v2.1.35 | 2026-06-02 | Rebase to 6.0.25.30 (INP3 added three struct fields our overlaid `asmstrucs.h` was blocking). |
| v2.1.24-33 | 2026-05/06 | KA cadence per peer family, `FLEXNET_WIRE_LT = 5` to land PCF samples at 1-2 ticks. **v2.1.33/34 reserved for the failed `STATUS_10` pong experiments — do not re-try.** |
| v2.1.13-23 | 2026-05 | The session-lifecycle stack: **LT rate-limit** (PCF cost `4095/2 → 2/2`, the saturation root cause), reaper hysteresis, proactive-scan guard, reaper-time LINK migration, INIT cooldown — **and v2.1.23 reverting that cooldown** once the wire showed PCF *wants* a full handshake per L2 session. |
| v2.1.0-12 | 2026-05 | First PCF interop: CTEXT suppression on F-flagged inbound SABM, removal of the spurious leading `"3+\r"` (a *request*, not an advert marker), single-digi `MYCALL*` on direct-neighbour `C <call>`, per-flavour records-per-emit cap. |

### v1.x — to GA

| Release | Date | What it closed |
|---|---|---|
| v2.0.0 | 2026-05-15 | **GA.** |
| v1.10.0 | 2026-05-15 | `FLEXNETSSIDRANGE N-M` — declares `max_ssid` in INIT and encodes `ssid_lo/hi` in one compact record. No new dispatch code was needed; BPQ's `APPLICATION` mechanism already delivered. |
| v1.9.8 | 2026-05-14 | `CE_FRAME_STATUS_1N` classifier — `"1n\r"` is a benign status family, ending the `CE-UNKNOWN` log noise. |
| v1.9.0-9.9 | 2026-05 | CE type-6/7 path discovery with on-disk cache, multi-neighbour cost-based routing, AXIP byte-6 SSID normalisation, `C <neighbour>` fixes, the `case 0xcf` fall-through that was corrupting the PID byte. |
| v1.2.0 | 2026-04-22 | Node identity preservation in the outbound digi chain. |
| ~~v1.9.4~~ | 2026-05-13 | **Reverted.** Transit re-advertisement over an L2 digipeat that broke AX.25 V2 reciprocity on the return frame — it did the chain extension without the contraction. What v2.2.0 got right three months later. |

Full narratives, measured numbers and reverted experiments:
**[`RELEASE_HISTORY.md`](RELEASE_HISTORY.md)**.

---

## Lessons that outlived their release

The expensive ones, each paid for once:

1. **A test bed proves nothing about production's transit.** The same config
   was inert on IR2UFV (never won a cost tie, forwarding counters never left
   `0/0/0`) and made prod a real transit path within a minute — `-4`'s
   destinations via us went **1 → 71**. Check whether the node is cheap or
   expensive relative to the incumbent path; never reason from the test bed.
2. **The monitoring caused most of what it measured.** A telnet disconnect
   froze LinBPQ for 1 s under the global semaphore, and the "PC/Flexnet 60 s
   tick" turned out to be our own `FL` poll. Never quote a stability rate
   without saying what was polling the node.
3. **Fixing a real defect is not the same as fixing the symptom.** v2.2.1's
   `3+` short-answer defect was real, verified, and the teardowns continued
   **23 of 23**. Confirm the mechanism, not the correlation.
4. **When a wire-format bug turns up in a tool, check the emitter for the
   same assumption.** "One `'3'` per frame" was fixed in the parser and
   written down as an observation; the emitter carried the same
   misunderstanding for **another four months**.
5. **Never advertise what you cannot carry.** (X)Net never sends CREQ — it
   digis and expects L2 routing — so a multi-hop advertisement without L2
   forwarding is a black hole. That is 67 of them, once.
6. **Capture first.** The L2 forwarding mechanism sat declared *illegal* in
   our own protocol spec until a capture of a PCF node forwarding somebody
   else's session proved otherwise.

---

## Out of scope

- **Being a fourth real FlexNet router.** The three are (X)Net, PC/Flexnet
  and RMNC/Flexnet. This is a BPQ node that participates correctly and
  carries what it honestly can.
- **Sharing a `flexnet_l3_proto.c` with `flexnetd`.** Set aside — it would be
  a sibling effort across both repos, not a deliverable here. `flexnetd`
  remains the protocol reference to cross-check against; a live capture
  outranks both.
- **Route withdrawal on `via_session_idx` failover** and the **periodic RTT=0
  TX refresh marker** — both were paired with the reverted v1.9.4 advertising.
- **P2 #9 capacity resize (64 → 256 destinations)** — 64 slots have held under
  live load. It is in `QUICK_WINS.md` if it ever bites.

---

| Where to look | For |
|---|---|
| `CLAUDE.md` | Current state, live traps, hard rules. **Wins over anything here on a conflict.** |
| `research/README.md` | What each wire investigation settled, so you can tell whether to open it |
| `RFC_TRANSIT_ROLE_V2.md` | v2.2 transit-role design; §15 records superseded decisions |
| `RELEASE_HISTORY.md` | The full release narrative this file used to carry |
| `QUICK_WINS.md` | Small, low-risk, additive items |
