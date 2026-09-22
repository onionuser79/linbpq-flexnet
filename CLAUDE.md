# linbpq-flexnet

FlexNet **CE/CF** routing added to **LinBPQ 6.0.x**, so a BPQ node can join a
FlexNet mesh alongside its existing NET/ROM stack. C11. This repo is **public**.

> ## Link stability — the `-12` teardown is ROOT-CAUSED (v2.2.2-rc1, under test)
>
> **Read `research/link_stability_2026-09-22/ROOT_CAUSE_PCF_QUIESCE.md`
> before touching advertisement emission.** From the 20.9 h quiet capture
> of 2026-09-21/22 — the first with `station-dashboard`'s 15-min telnet
> of all five nodes switched off:
>
> **PC/Flexnet tolerates our unsolicited compact records until it has
> done a `3+` exchange, and treats them as a protocol error afterwards.**
> After the `3-` that closes our answer it accepts **at most two** more
> record frames and then DISCs — 10 transactions died on the 1st, 20 on
> the 2nd, **none on the 0th and none reached a 3rd, 30/30**. It reacts
> synchronously: 30 of 32 teardowns land within **0.06 s** of one of our
> record frames, on an L2 session that is healthy to the last ack.
>
> Two things this is *not*, both of which look compelling until measured:
> * **Not the content.** `IW2OHX-14 = 2` went out 614 times harmlessly
>   and 14 times fatally; `IR2UFV 0-8 = 1`, 617 vs 11.
> * **Not the `3-` placement.** A record arriving within 5 s after a `3-`
>   — the exact shape rc3's EOB quiet window was written for — happens
>   **549 times outside a transaction with zero teardowns.**
>
> Why: `PROTOCOL_SPEC.md` §2.6 exchanges routes *inside* a `3+`…`3-`
> transaction, and PC/Flexnet obeys it literally — **162** record frames
> to our **5583** over the same capture. The event-driven push rc4
> introduced is the outlier, not PC/Flexnet's reaction to it.
>
> Fix: **`FLEXNETPCFQUIESCE`** (default YES) — after answering a PCF
> peer's `3+`, send it no further records until its next `3+`. Scoped to the
> PCF family with `flex_peer_is_pcf()`: (X)Net sent no `3+` across the 20.9 h
> baseline, but it is not incapable of it (`IW2OHX-14` sent one at session
> setup 2026-09-22T07:56:52Z), so the gate is explicit rather than implied by
> the transaction.
>
> The section below is the investigation as it stood. Its defect analysis
> is sound and those defects were real; its "this is the mechanism"
> claims are superseded by the above.
>
> ## Link stability — a fifth cause: we answer `3+` with only what changed
>
> **2026-09-20, after the telnet fix below: the `-12` link still recycled
> every 70-87 min.** PC/Flexnet initiates **100%** of teardowns (18/18);
> we send no DISC and no SABM. They come in **pairs exactly 60 s apart**
> and the second of each is PCF's fresh-session seed, so there are **8
> independent** teardowns, not 18.
>
> **PCF sent exactly 8 `3+` full-table requests, and there were exactly 8
> teardowns — a 1:1 mapping in both directions, p ≈ 2.5e-13.** The `3+`
> walk passed `force=FALSE`, so an explicit request for the WHOLE table
> ran through the 10% change-detection threshold (a filter for
> *unsolicited* adverts). We answered with **3, 24, 45, 3, 38, 72, 39 and
> 36 records out of 204**, then `3-` end-of-batch, and PCF hung up 7-77 s
> later. **The short answer was fixed in v2.2.1 with `force=TRUE`; the
> teardowns did not stop.**
>
> The `force=FALSE` came from `flex_advertise_seed_peer()`, where it is
> correct *and documented*: "a fresh session's advertised[] is empty, so
> every entry fires on the never-advertised sentinel anyway". **That
> precondition does not hold mid-session.** Hence the tell — the seed
> dump after a restart is complete (345 records) while a `3+` minutes
> later returns three.
>
> Read `research/link_stability_2026-09-20/DESTINATION_EXCHANGE_CLIMB.md`.
>
> **Two red herrings that survive scrutiny until you look closely**, both
> recorded there: all 8 teardowns also follow an outbound **link-time
> frame** within 90 s (p≈4e-05) and an 8x **advertisement-volume** spike —
> but we answer a `3+` with an LT *and* the walk in the same instant, so
> those are the same events seen through the wrong frame. The giveaway
> that rate was never the mechanism: **two teardowns followed answers of
> just three records.**
>
> Separately real but NOT the cause: **43 of 204 destinations climb
> geometrically** (count-to-infinity), 35.7% of everything we advertise,
> which a purely *relative* 10% jitter floor cannot stop. Contained by
> `flex_climb_is_loop()`. Its floor must **persist** across a withdrawal —
> v2.2.1-rc1 reset it, the destination re-floored at its inflated cost,
> the ladder resumed and the climbing share was **28.0% after vs 27.8%
> before**.
>
> Four counting traps, each of which made this look smaller or different:
> * **`linkstab`'s `SESSION_RESTART` under-reports** — it only sees uptime
>   going backwards, so a link that *vanishes* from `FL` and returns is
>   not counted. 1 reported, 7 real.
> * **Count teardown pairs as one event.** 18 vs 8 inverts the stats.
> * **PCF's `600` is its session seed, not a symptom** of what preceded
>   it. Its reported LT is exactly `5295/n` — a 16-slot ring holding one
>   big session-start sample, diluted.
> * **A quiet hour proves nothing.** `3+` arrives every 75-90 min; rc1 ran
>   63 min teardown-free without ever being asked for a table. Confirm a
>   `3+` actually happened (`tools/`-side: `/tmp/plus.py` on gw).
>
> ## Link stability — the fourth cause was the telnet port, not FlexNet
>
> **2026-09-20: the remaining ~1 s stall is a LinBPQ telnet disconnect.**
> Closing a telnet session runs `Sleep(1000)` on the main thread *while
> holding the global `Semaphore`* (`TelnetV6.c:2521`, reached from
> `TIMERINTERRUPT()` under the lock taken at `LinBPQ.c:1790`). For that
> second no port is polled and no AX.25 frame is acked — longer than
> (X)Net's entire ~0.6 s retry budget, so the peer silently drops the
> session. Proven by 214 flat-1002 ms semaphore holds, 4/4 gdb
> backtraces, and a control on production `-13`: 2 telnet closes → 2
> freezes; IR2UFV with the fix, 10 closes → 0.
>
> **Fix: `DisconnectOnClose=0` on the Telnet port.** Applied to IR2UFV
> *and to production IW2OHX-13* (which was measured freezing 1004 ms each
> time the dashboard cron telnetted in). `IW2OHX-15` has the directive
> too and Windows BPQ32 has the same main-loop shape, but its `-14` link
> is stable at 4h 39m — **exposure scales with how busy the link is**, so
> it waits for a scheduled restart.
> Read `research/link_stability_2026-09-20/TELNET_SLEEP_FREEZE.md`.
>
> Two consequences worth carrying:
> * **The "PC/Flexnet 60 s tick" was ours.** The 60 s cadence is the `FL`
>   poll interval driving the freeze, not a PCF timer.
> * **The monitoring caused most of what it measured.** `linkstab`
>   reconnects telnet every 60 s. Never quote a stability rate without
>   saying what was polling the node.
>
> ### Earlier causes, fixed in v2.2.0
>
> IR2UFV's three FlexNet links used to reset constantly, which made the
> destination table untrustworthy. Settled 2026-09-18/19 from a 22 h
> capture of all three links. **Read
> `research/link_stability_2026-09-19/` before touching advertisement
> emission or session setup** — it is the reference for both.
>
> Three causes. Two were ours and are fixed:
>
> 1. **We hung up on a slow peer.** `FRACK=3000 × RETRIES=5` = 15 s of
>    patience against 59 s stalls. `RETRIES` → 25. Our teardowns to `-4`:
>    38 / 4.9 h → 1 / 17.2 h.
> 2. **One route record per I-frame** (v2.2.0). One `'3'` per *frame*,
>    then N records, is the format — we sent one record per frame, 15 of
>    236 `PACLEN` bytes. Queue to PC/Flexnet: non-empty 80 % → 6 %.
> 3. ~~**PC/Flexnet's own 60 s teardown tick — not ours.**~~
>    **Superseded 2026-09-20** — see the banner above. The 60 s cadence
>    was our own telnet-poll-induced freeze.
>
> Plus one found during the soak and fixed in the same release:
> **unsolicited re-INIT on a healthy link.** BPQ recycles a peer's
> `LINKTABLE` slot during internal L2 maintenance with nothing on the
> wire; `FlexNet_InitSession`'s same-callsign/new-LINK path then reset
> the session and sent INIT, reseeding the peer's cost ring with a `600`
> outlier. v2.1.15's established-guard now covers that path too.
>
> **IW2OHX-4 is a separate, open problem.** It flaps against PC/Flexnet
> as well, on a link we never touched — its churn is its own, and is
> under separate investigation. Don't read it as a regression.
> **2026-09-21: it is no longer ours to measure at all.** IW2OHX-4 was
> cut back to a single FlexNet link (to `IW2OHX-12`), so neither prod
> nor IR2UFV peers with it any more. Its PC/Flexnet churn continues out
> of our view; any `-4` figure in this repo predates that cut.
>
> Measurement traps, both of which cost real time:
> * **Pin `axudp_teardown.py --local-ip`.** It defaults to the most
>   frequent *source*, so where the peer out-talks us it adopts the
>   peer's address and reports every direction backwards.
> * **Never trust a before/after taken across a link reset.** Check the
>   process pid and link uptimes first, and read a negative counter delta
>   as a restart marker, not data.

**Scope changed on 2026-09-21: production IW2OHX-13 is now a FlexNet
ROUTER**, aligned with IR2UFV — `FLEXNETTRANSIT`, `FLEXNETL2TRANSIT`,
`FLEXNETPATHFORWARD` and `FLEXNETLT3BYTE` all `YES`, and `DIGIFLAG=1` on the
AXIP port. RFC §11 (which kept production non-forwarding) is superseded by
Marco's decision that day. **`FLEXNETSSIDRANGE` is the one thing NOT aligned
and must stay `13-13`**: IR2UFV can advertise `0-8` because that is its own
callsign, whereas `IW2OHX-13` shares its base call with other live nodes on
the same mesh (`-1`, `-4`, `-12`, `-14`, `-15`), so `IW2OHX (0-8)` would
claim nodes this one does not own.

All four directives still **default to NO** in the code — router behaviour is
opted into, never inherited. Rollback to leaf is
`sudo bash /tmp/rollback-prod-leaf.sh` on gw. This is still not a
replacement for the three real routers — (X)Net, PC/Flexnet, RMNC/Flexnet.

⚠ **Production was a loop candidate**: `-14 → us → -4 → -12 → -14` was a
real cycle and `flex_climb_is_loop()` is what contains it. **The cycle was
broken on 2026-09-21** when IW2OHX-4 was cut back to its `-12` link only —
prod's sole FlexNet peer is now `-14`, and IR2UFV's are `-14` + `-12`. The
containment code stays (the topology can change back, and `-14`'s own
neighbours can still form one); what changed is that this particular loop is
no longer live, so a clean `flex_climb_is_loop()` counter is not evidence that
it works. Standing watch in
`/tmp/prod-router-watch/` on gw (rotating capture of `udp port 10093` plus
`FL` sampled every 5 min). **The same config is NOT the same change on the
two nodes** — on IR2UFV transit is inert (it never wins a cost tie, its
forwarding counters never left 0/0/0), whereas prod won immediately: `-4`'s
destinations via us went 1 → 71 and via `-12` 187 → 134 within a minute,
because `-4` has no direct FlexNet link to `-14`. **Never reason about the
effect of a transit setting from the test bed alone; check whether the node
is cheap or expensive relative to the incumbent path.** (That measurement is
historical from 2026-09-21 onward — `-4` no longer peers with either node, so
prod's transit has no `-4` traffic to win.)

Before enabling any of them on a node carrying real users: the purely
**relative** 10% advertisement jitter threshold used to be a known-open item
here, and on 2026-09-20 it was measured letting **43 of 204 destinations climb
geometrically — 35.7% of everything we advertised.** A ×1.3 ladder clears a 10%
floor on every rung. It was a real defect, but not the `-12` teardown cause it
was first read as (see the banner). **v2.2.1's `flex_climb_is_loop()` closes
it** (3 consecutive rises *and* ≥4×
the cheapest cost seen → withdraw once); the accompanying
**hold-down on transitions to infinity** is the existing poison hold-down,
which that withdrawal now feeds. Both were in RFC §13.3 and
`research/path_query_2026-09-18/LINK_INSTABILITY.md`; the measurement is in
`research/link_stability_2026-09-20/DESTINATION_EXCHANGE_CLIMB.md`. The append/contract
asymmetry that used to be listed here is resolved — it was a cross-restart
measuring artefact; compare deltas, never cumulative counters.

The sibling `flexnetd` is the **protocol reference implementation** — cross-check
wire formats there. A live capture outranks both.

## This repo is an overlay, not a fork

It holds only the files that differ from upstream `g8bpq/linbpq`
(remote `upstream`), rsynced on top of a full BPQ32 source tree at build time.

| Ours only (new) | Modified from upstream |
|---|---|
| `FlexNetCode.c` — the FlexNet implementation | `Cmd.c` (`V`, `FL`, `D` commands) |
| `flexnet_l3.c` / `flexnet_l3.h` | `L2Code.c`, `asmstrucs.h`, `bpqaxip.c`, `makefile` |

Those five modified files are the **entire upstream-rebase conflict surface** —
the weekly upstream watcher flags exactly these. Everything else in a build
tree comes from upstream untouched.

## Commands

```bash
./sync-and-build.sh all         # production build (what prod runs)
./sync-and-build.sh flexdebug   # adds -DFLEXNET_DEBUG=1 (wire trace)
./sync-and-build.sh all EXTRA_CFLAGS=-DFLEXNET_PROD=1   # silent prod build
```

Two-stage sync: macmini → `iw2ohx-gw:…/linbpq-flexnet` (mirror, `--delete`),
then overlay → `…/linbpq-build/` (**no** `--delete`, so untouched upstream
sources survive), then remote `make`. **Never edit the remote trees** — stage 1
mirrors with `--delete` and will erase the work.

**Switching between `flexdebug` and `all` needs a `clean` first.** The
targets *assign* `CFLAGS`, so every `.o` carries the flags of whichever
target built it — and `make` then sees those objects as up to date and prints
`Nothing to be done for 'all'`. A prod build issued straight after a debug
build is the debug build, relinked, with no warning. Run
`./sync-and-build.sh clean` between flavours and check the flavour on the
binary, not on the make output.

Three compile-time switches, and the way to set them is not obvious:

- `FLEXNET_DEBUG=1` — `FlexNet_Log()` to `/tmp/flexnet_axudp.log` + verbose
  console. Use the **`flexdebug` target**, and switch back before tagging.
- `FLEXNET_PROD=1` — silences all informational console output. Pass via
  **`EXTRA_CFLAGS`**, never `CFLAGS+=` on the command line: `all:` and
  `flexdebug:` *assign* target-specific `CFLAGS`, so a command-line `CFLAGS`
  is overridden and silently does nothing. Only `EXTRA_CFLAGS` is appended.
- A silent prod build therefore looks identical whether or not the flag took.
  Verify with `strings <binary> | grep -c 'FlexNet: '` — measured at v2.2.1,
  **1** on the silent build against **81** on `flexdebug`. The one that
  survives is the deliberate `advertised[] full` operator warning, a bare
  `Consoleprintf` by RFC §15 Q4 design, not chatter. `/tmp/flexnet_axudp.log`
  also survives in `.rodata` on a silent build — `flexlog_open()` is still
  compiled, but `FlexNet_Log()` returns on `!FLEXNET_DEBUG` before calling
  it, so the file is never opened. Do *not* grep for `FlexNet_Info`: it is a
  macro, so it never reaches the binary and that check cannot fail.

## Live traps

- **`FLEXNETTRANSIT` compiles to `FALSE`** (`g_flexnet_transit_enabled`,
  `FlexNetCode.c`) — the §15 Q2 supersession of 2026-09-14, **shipped to
  production with v2.2.1 on 2026-09-21**. Transit is opted into, never
  inherited by omission. Both live cfgs still carry an explicit value and
  should keep it: an explicit line is what makes a node's role readable
  without knowing which build it is running. Don't reason from the compiled
  default about what a *deployed* node is doing.
- **v2.2.0 D1-D3 (was rc4), 2026-09-17** — `FlexNetAdvertised[]`,
  `flex_advertise_check()`, the per-peer token buckets, poison-reverse with
  hold-down and `learned[]` ageing are all in, and rc2's cap+cursor block is
  gone from `flex_send_own_routes`. What is still open is in RFC §13.3, and the
  two that will bite are: **poison-reverse can undo itself** (peers echo our
  withdrawal and we re-learn it — this made a count-to-infinity phantom), and
  **advertising a route we cannot carry creates a black hole** ((X)Net never
  sends CREQ; it digis and expects L2 routing, so `FLEXNETL2TRANSIT` is what
  makes a multi-hop advertisement honest).
- **`DIGIFLAG=1` is required on the port** for any advertised route to actually
  be carried. Without it the routes install on peers and every connect fails.
- **`README.md`'s banner must track `FLEXNET_VERSION_STR`** — it had drifted to
  v2.1.38 against a v2.1.42 constant. Check both in the same commit.
- **Two capture traps that silently produce an empty pcap** (both hit
  2026-09-17, cost ~40 min):
  1. `tcpdump` drops privileges to the `tcpdump` user, so it **cannot
     overwrite a pcap a previous run left behind** — it exits
     immediately with `Permission denied` into its own stderr log while
     the caller sees a stale file of the old size. Always write to a
     fresh path, and prove it is *writing* (size grows) rather than
     merely alive.
  2. `pgrep -f 'tcpdump.*<port>'` **matches the waiter's own command
     line**, so `until ! pgrep -f 'tcpdump.*10075'; do sleep; done`
     never terminates and reports the capture as running forever.
     Match the process name (`pgrep -x tcpdump`), not a pattern that
     appears in your own argv.
  `tools/capture-steady.sh`-style scripts should do both checks; a
  capture that reports "running" is not evidence that it is recording.
- `ConvFromAX25()` writes **more than 10 chars**. Normalised-callsign buffers
  are `char buf[20]`, never `char buf[FLEXNET_MAX_CALLSIGN]` — a real overflow
  was fixed from getting this wrong.

## Versioning and release

Two constants at the top of `FlexNetCode.c`:

- `FLEXNET_VERSION_STR` (currently `"v2.2.1"`) — user-facing, shown by `V`.
  Bump every release, **including version-string-only releases**: the string
  tracks the upstream baseline even when nothing functional changed.
- `FLEXNET_VERSION_PROTO` (currently `"linbpq-1.9"`) — wire-visible identity in
  the L3RTT version slot. Bump only when that identity changes; peers may key
  off it.

**Every release also updates the node MOTD/CTEXT** to the installed version, on
both the production node and the test instance. Never put "Digipeater" in that
string — this node is not a digipeater. Full release checklist: `AGENTS.md` §5.
Don't tag until production has run cleanly.

## Hard rules

1. **Capture first, then code.** Never guess a wire format; derive it from a
   capture of real peers and re-verify with a fresh capture after deploy. For
   forwarding questions run a **dual-port capture** — a shared QSO key across
   both captures is the only proof a frame propagated rather than being
   answered locally.
2. **Cite every wire constant** — a `flexnetd` file:line, a captured frame, or
   the spec. Otherwise it's a guess and stays out of the code.
3. **Observation language in public artefacts** (`AGENTS.md` §7 has the full
   table): "captures show…", "observed on the live network…" — never claims
   about a peer's internal code paths, offsets or executables.
4. **Don't add warnings; you can't yet demand zero.** The `makefile` is
   upstream's and enables **no** warning flags at all, so a clean
   `./sync-and-build.sh all` proves nothing. Measured with
   `-Wall -Wextra -Wshadow`: `flexnet_l3.c` is **clean (0)**, `FlexNetCode.c`
   has **56 pre-existing** warnings (`-Wpointer-sign` from `ConvFromAX25`
   callers, plus shadow/unused). Check your own change in isolation:
   `gcc -DLINBPQ -MMD -g -fcommon -Wall -Wextra -Wshadow -c -o /tmp/w.o <file>`
   in the build tree, and leave the count no higher than you found it.
   Keep `flexnet_l3.c` at zero. Otherwise the global C rules hold: fixed-width
   `stdint.h` types for anything on the wire, `stdbool.h` for booleans, no
   `strcpy`/`strcat`/`sprintf`/`gets`.
5. **Test both directions.** CE type-6/7 is symmetric: an originator-role test
   (we send, peer replies) does **not** cover the target role (peer sends, we
   reply). A TX-side-correct format has shipped with an accidentally-correct
   parser before.
6. Comments default to **none**; comment the *why* — an invariant, a wire
   constraint, a captured-byte observation. Never narrate the *what*.

## Deploy

Kill the running binary first (`cp` over a running image fails `Text file
busy`), copy, relaunch detached. **Relaunch with the absolute path**
(`nohup /home/bpq-ufv/linbpq &`, not `cd /home/bpq-ufv && nohup ./linbpq &`):
the documented kill recipe matches on the full path, and a `./linbpq` argv[0]
makes `pkill -9 -f /home/bpq-ufv/linbpq` silently match nothing — so the next
deploy would install over an instance that is still running. linbpq runs as root, so the `setcap` lines in
`makefile` are moot for the deployed binary. The production and test instances
live in **different directories with different telnet/AXIP ports** — always
kill by full path, never a bare pattern that would take both down.

## Deeper docs — read on demand, not by default

- `AGENTS.md` — the deep methodology reference (capture workflow, C
  conventions, wire discipline, release flow, public-language table, project
  history). Written at v1.9, so **treat its version-specific detail as
  historical** and this file as current.
- `RFC_TRANSIT_ROLE_V2.md` — v2.2 transit-role design. Behaviour is gated
  on it; §15 records superseded decisions.
- `ROADMAP.md` — gap analysis vs `flexnetd` v1.0.0. `QUICK_WINS.md` — small items.
- `research/` — wire-level investigations, indexed in `research/README.md` by
  what each one settled. Start there for any advertisement or session question.
- `tools/` — capture and query helpers (`xnet_agent.py`, `analyze_dual_capture.py`,
  `bpq_d_query.py`, `d_count_marks.py`, soak checks). Composable scripts, not a
  framework; extend as the question demands. For routing bugs reach for
  **`quad-watch.py`** first: it samples IR2UFV, -14, -4 and -12 within seconds
  of each other and cross-checks their tables, because a distance-vector
  inconsistency is a disagreement *between* two tables and is invisible from
  either end alone — which is why the single-sided `pcf-watch.sh` never found
  one.
- `research/` — captures and analyses backing past decisions, incl. `transit_v2/`.
