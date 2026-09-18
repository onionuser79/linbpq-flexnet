# linbpq-flexnet

FlexNet **CE/CF** routing added to **LinBPQ 6.0.x**, so a BPQ node can join a
FlexNet mesh alongside its existing NET/ROM stack. C11. This repo is **public**.

> ## ⛔ BLOCKED — read before writing any code
>
> **IR2UFV's links to IW2OHX-12 and IW2OHX-4 keep resetting**, so its
> destination table is not trustworthy and connects succeed or fail
> depending on timing. Marco's directive, 2026-09-18: *fix this before
> implementing any other feature.*
>
> **Start at `research/link_stability_2026-09-18/TEARDOWN_DIRECTION.md`.**
> It settles the "who tears down first" question from the wire and shows
> the two links fail for **opposite** reasons, so they need different
> fixes:
>
> * **IW2OHX-4 — we hang up on it**, 33 outbound DISCs to 1 inbound.
>   `FRACK=3000 x RETRIES=5` gave 15 s of patience against measured 59 s
>   stalls. Changed to `RETRIES=25` (75 s) on 2026-09-18; **unverified
>   until the post-fix capture shows the DISC count collapse.**
> * **IW2OHX-12 — PC/Flexnet hangs up on us**, 6 inbound DISCs to 0
>   outbound, each arriving 0.02–0.22 s *after* it acked our traffic on a
>   healthy link. Timers cannot fix this; only advertisement volume can.
>
> It also corrects `fix_finder_2026-09-18/PHASE_CONCLUSION.md`: the jitter
> threshold is **not** the dominant cost. 80 % of fires to -4 and 50 % to
> -12 are `last=-1` — the full-table re-dump after each re-init.
>
> `research/OPEN_NEXT_link_instability.md` has the original uptime table
> and PCF's cost rings.
>
> Corollary for measurement: **never trust a before/after taken across a
> link reset.** Check the process pid and the link uptimes first, and
> read a negative counter delta as a restart marker, not data. Several
> of 2026-09-18's measurements had to be discarded for this.

**Scope, and it matters:** **production is a FlexNet leaf node** and RFC §11
keeps it that way. Router behaviour now *exists* but is opted into, never
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
- **rc4 D1-D3 landed 2026-09-17** — `FlexNetAdvertised[]`,
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

- `FLEXNET_VERSION_STR` (currently `"v2.2.0-rc4"`) — user-facing, shown by `V`.
  Bump every release, **including version-string-only releases**: the string
  tracks the upstream baseline even when nothing functional changed.
- `FLEXNET_VERSION_PROTO` (currently `"linbpq-1.9"`) — wire-visible identity in
  the L3RTT version slot. Bump only when that identity changes; peers may key
  off it.

**Every release also updates the node MOTD/CTEXT** to the installed version, on
both the production node and the test instance. Never put "Digipeater" in that
string — this is a leaf, not a digi. Full release checklist: `AGENTS.md` §5.
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
- `RFC_TRANSIT_ROLE_V2.md` — v2.2 transit-role design, rc4. Behaviour is gated
  on it; §15 records superseded decisions.
- `ROADMAP.md` — gap analysis vs `flexnetd` v1.0.0. `QUICK_WINS.md` — small items.
- `tools/` — capture and query helpers (`xnet_agent.py`, `analyze_dual_capture.py`,
  `bpq_d_query.py`, `d_count_marks.py`, soak checks). Composable scripts, not a
  framework; extend as the question demands. For routing bugs reach for
  **`quad-watch.py`** first: it samples IR2UFV, -14, -4 and -12 within seconds
  of each other and cross-checks their tables, because a distance-vector
  inconsistency is a disagreement *between* two tables and is invisible from
  either end alone — which is why the single-sided `pcf-watch.sh` never found
  one.
- `research/` — captures and analyses backing past decisions, incl. `transit_v2/`.
