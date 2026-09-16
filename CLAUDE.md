# linbpq-flexnet

FlexNet **CE/CF** routing added to **LinBPQ 6.0.x**, so a BPQ node can join a
FlexNet mesh alongside its existing NET/ROM stack. C11. This repo is **public**.

**Scope, and it matters:** this is a FlexNet **leaf node**. It does not
re-advertise other neighbours' destinations and does not act as an L2 digipeat
transit. It is not a replacement for the three real routers — (X)Net,
PC/Flexnet, RMNC/Flexnet. Don't let a change quietly grow into router
behaviour; that is the v2.2 transit-role work, gated by
`RFC_TRANSIT_ROLE_V2.md`.

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
- **The rest of rc4 D1 is not implemented** — `FlexNetAdvertised[]`,
  `flex_advertise_check()` (§5.3) and `FLEXNET_REFRESH_THRESHOLD` jitter
  suppression are still open, and rc2's cap+cursor block is still in
  `flex_send_own_routes`. Only the default flip landed.
- **`README.md`'s banner must track `FLEXNET_VERSION_STR`** — it had drifted to
  v2.1.38 against a v2.1.42 constant. Check both in the same commit.
- `ConvFromAX25()` writes **more than 10 chars**. Normalised-callsign buffers
  are `char buf[20]`, never `char buf[FLEXNET_MAX_CALLSIGN]` — a real overflow
  was fixed from getting this wrong.

## Versioning and release

Two constants at the top of `FlexNetCode.c`:

- `FLEXNET_VERSION_STR` (currently `"v2.1.42"`) — user-facing, shown by `V`.
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
  framework; extend as the question demands.
- `research/` — captures and analyses backing past decisions, incl. `transit_v2/`.
