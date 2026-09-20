# linbpq-flexnet

FlexNet **CE/CF** routing added to **LinBPQ 6.0.x**, so a BPQ node can join a
FlexNet mesh alongside its existing NET/ROM stack. C11. This repo is **public**.

> ## Link stability — the last cause was the telnet port, not FlexNet
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
> time the dashboard cron telnetted in). `IW2OHX-15` not yet checked.
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
>
> Measurement traps, both of which cost real time:
> * **Pin `axudp_teardown.py --local-ip`.** It defaults to the most
>   frequent *source*, so where the peer out-talks us it adopts the
>   peer's address and reports every direction backwards.
> * **Never trust a before/after taken across a link reset.** Check the
>   process pid and link uptimes first, and read a negative counter delta
>   as a restart marker, not data.

**Scope, and it matters:** **production re-advertises nothing** — it carries
only its own destinations — and RFC §11 keeps it that way. Router behaviour now *exists* but is opted into, never
inherited: `FLEXNETTRANSIT` (re-advertise other neighbours' destinations),
`FLEXNETL2TRANSIT` (L2 digi-chain forwarding) and `FLEXNETPATHFORWARD` (relay
CE type-6 path traversals) all default to **NO**. All three are v2.2/v2.3 work,
live on the IR2UFV test instance only, and gated by `RFC_TRANSIT_ROLE_V2.md`.
This is still not a replacement for the three real routers — (X)Net,
PC/Flexnet, RMNC/Flexnet.

Before enabling any of them on a node carrying real users: the two known-open
items are the purely **relative** 10% advertisement jitter threshold (high-RTT
destinations never settle, so they re-advertise forever) and the absence of a
**hold-down on transitions to infinity**. Both are in RFC §13.3 and
`research/path_query_2026-09-18/LINK_INSTABILITY.md`. The append/contract
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

Three compile-time switches, and the way to set them is not obvious:

- `FLEXNET_DEBUG=1` — `FlexNet_Log()` to `/tmp/flexnet_axudp.log` + verbose
  console. Use the **`flexdebug` target**, and switch back before tagging.
- `FLEXNET_PROD=1` — silences all informational console output. Pass via
  **`EXTRA_CFLAGS`**, never `CFLAGS+=` on the command line: `all:` and
  `flexdebug:` *assign* target-specific `CFLAGS`, so a command-line `CFLAGS`
  is overridden and silently does nothing. Only `EXTRA_CFLAGS` is appended.
- A silent prod build therefore looks identical whether or not the flag took —
  verify by grepping the binary for the `FlexNet_Info` string.

## Live traps

- **`FLEXNETTRANSIT` now compiles to `FALSE`** (`g_flexnet_transit_enabled`,
  `FlexNetCode.c`) — the §15 Q2 supersession of 2026-09-14. Transit is opted
  into, never inherited by omission. **This is on `main` but unreleased:** the
  binary running in production was built before the flip and still defaults to
  transit-on, which is why both live cfgs carry an explicit value. Keep setting
  `FLEXNETTRANSIT` explicitly on both sides; don't reason from the default
  about what a *deployed* node is doing.
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

- `FLEXNET_VERSION_STR` (currently `"v2.2.0"`) — user-facing, shown by `V`.
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
busy`), copy, relaunch detached. linbpq runs as root, so the `setcap` lines in
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
