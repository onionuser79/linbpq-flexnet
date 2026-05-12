# AGENTS.md — Working notes for coding agents on linbpq-flexnet

This file is for any agent (human or AI) picking up work on this
repository. It captures the methodology, conventions, and pitfalls
the project has accumulated. Read it before you change code or
documentation.

The project is **a FlexNet routing module added to LinBPQ 6.0.x**.
LinBPQ is G8BPQ's packet-radio node implementation; FlexNet is a
distance-vector routing protocol that runs in parallel with NET/ROM.
The relevant wire formats live in the upstream LinBPQ source tree
(unchanged here) and in `flexnetd` (sibling project, also maintained
by the same author). Always cross-check against `flexnetd` when in
doubt — it's the protocol reference implementation.

---

## 1. Methodology — capture, then code

**Do not guess wire formats.** Every byte we emit on the wire was
derived from a packet capture of real peers, cross-checked against
`flexnetd`'s parser/builder, and re-verified by a follow-up capture
after deploy.

The recurring workflow is:

1. **Capture first.** Use `xnet_agent.py` (sibling project) on the
   relevant node + port to dump traffic as JSON. For forwarding
   questions, run a **dual-port capture** — one agent on each side
   of the suspected forwarder — and look for a shared QSO key
   across the captures. That's the only proof that a frame actually
   propagated rather than being answered locally.
2. **Decode against a reference.** Read `flexnetd/ce_proto.c`,
   `cf_proto.c`, `poll_cycle.c`. If the wire bytes match the
   reference, the implementation is right; if not, the reference is
   the canonical authority unless the live capture says otherwise.
3. **Implement minimum viable, then verify.** Make a single
   targeted change, build, deploy, run a fresh capture, confirm
   the new frames match what the captures show. Don't bundle
   speculative changes.
4. **State claims in observation terms, not inference terms.**
   See section 7.

**Anti-pattern caught in the v1.9 cycle:** the first PATH_REQ
implementation sent `<origin> <target>` because that's what the
spec text and `flexnetd` builder superficially suggested. The
dual-port capture between three real nodes showed peers actually
send `<origin> <next_hop> <target>`, accumulating the chain
hop-by-hop. The fix wasn't in the parser — it was in the format.
Always let captures override your reading of the code.

---

## 2. Build and deploy

The repo lives on the maintainer's dev host; builds run on a
remote build host (`iw2ohx-gw`) inside a sibling directory
(`linbpq-build/`) that overlays this repo's files onto a full
LinBPQ 6.0.x source tree. Use the helper:

```
./sync-and-build.sh all          # production (non-debug)
./sync-and-build.sh flexdebug    # adds -DFLEXNET_DEBUG=1
```

`all` and `flexdebug` are the two relevant make targets. They
differ by a single preprocessor flag — the debug build enables
`FlexNet_Log()` (writes `/tmp/flexnet_axudp.log`) and verbose
`Consoleprintf()` traces. **Production deployments use `all`.**
Switch to `flexdebug` only when you need wire-level traces to
investigate a regression; switch back before merging or tagging.

Deploy procedure (the running linbpq must die first or the `cp`
fails with `Text file busy`):

```
sudo pkill -x linbpq
# wait, escalate to -9 if still up
sudo cp /home/iw2ohx/xnet_investigation_agent/linbpq-build/linbpq /home/bpq/linbpq
sudo bash -c 'cd /home/bpq && nohup ./linbpq >/dev/tty2 2>&1 </dev/null &'
```

linbpq runs as root, so the `setcap` lines in `makefile:84` are
moot for the deployed binary — don't re-apply them after `cp`.

---

## 3. C code conventions

The project follows the global C standards documented in the
maintainer's `CLAUDE.md`. Highlights that bite most often here:

- **C11**, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`.
  The build must succeed with zero warnings before a change is done.
- **Fixed-width types from `<stdint.h>`** for anything that hits the
  wire. `int` is not portable on size.
- **`stdbool.h`** for booleans — never `int` as a stand-in.
- **Forbidden:** `gets`, `strcpy`, `strcat`, `sprintf`. Use the
  bounded versions with explicit lengths. The `flex_*` callsign
  handling in `FlexNetCode.c` is the reference.
- **Buffer sizing for normalised callsigns:** `ConvFromAX25()`
  writes more than 10 chars into its destination. Use
  `char buf[20]` for any normalised-callsign buffer, not
  `char buf[FLEXNET_MAX_CALLSIGN]`. There was a real overflow bug
  fixed in commit `5db6234` from getting this wrong.
- **`-fsanitize=address,undefined`** during development. UBSan
  reports are bugs, not warnings.

Style:

- 4-space indent, no tabs. 100-col lines max.
- Pointer `*` binds to the variable: `int *p`, not `int* p`.
- `snake_case` for functions and variables, `PascalCase` for
  types, `SCREAMING_SNAKE_CASE` for macros and constants.
- TU-local helpers are `static`.
- Functions ≤ 50 lines as a goal, 100 as a hard limit. Split
  longer ones.

Comments:

- **Default to none.** Identifiers should tell the story.
- Comment when the *why* is non-obvious — an invariant, a wire-
  format constraint, a flexnetd cross-reference, a captured-byte
  observation that drives the choice.
- Never narrate the *what*. `/* increment counter */` is noise.
- TODO / FIXME entries need a tracking reference or a short
  rationale; don't leave dangling.

---

## 4. Wire-protocol discipline

The FlexNet wire is unforgiving. A wrong byte produces silence,
not an error — there's no peer-side log to read. Discipline:

- **Document the byte layout above every build/parse function.**
  Header comment lists each field, its position, and its meaning.
  See `flex_build_path_req()` for the current form.
- **Cite the source of every wire constant.** Either a `flexnetd`
  file:line, a captured frame timestamp, or the protocol spec. If
  none of those apply, the constant is a guess and shouldn't be
  in the code yet.
- **HOP_BYTE on CE type-6/7 is `0x20 + hop_count`.** Originator
  sets `0x21` (1 = "one intermediate named in body"). Each
  forwarder increments before retransmitting.
- **QSO field is 5 ASCII chars, right-justified.** High bit of
  byte 0 is the TRACE flag — mask it before parsing the integer.
- **Test in both directions.** A symmetric protocol like CE type-
  6/7 needs an originator-role test (we send, peer replies) AND
  a target-role test (peer sends to us, we reply). The first
  v1.4.x cycle shipped the wire format right for the TX side but
  the parser was correct only by accident; a real PATH_REQ from
  the wire would have exposed it. Drive both directions with
  captures.

---

## 5. Versioning and releases

Two version constants live at the top of `FlexNetCode.c`:

- `FLEXNET_VERSION_STR` — user-facing, shown by `V` command in
  linbpq. Bump on every release.
- `FLEXNET_VERSION_PROTO` — protocol identity placed in the
  L3RTT version slot. Bump when the wire-visible identity
  changes; peers may key off this.

Release flow used to date:

1. Land all the code on the dev branch.
2. Build + deploy the production binary.
3. Bump both version constants in one commit titled
   `release: vX.Y …`.
4. Update `README.md` (banner + changelog), `ROADMAP.md`
   (mark items done, add the version section).
5. Fast-forward `main`, push.
6. Annotated tag `vX.Y` with a one-line summary, push the tag.
7. Delete the dev branch once `main` is at the same commit and
   no work is in flight on it.

**Don't tag until production runs cleanly.** The point of the
pre-GA `v1.9` was to let it soak for a multi-day window before
calling it GA.

---

## 6. Documentation

Three docs are load-bearing and must stay in sync with code:

- `README.md` — feature list, build guide, known limitations,
  changelog. The banner version must match `FLEXNET_VERSION_STR`.
- `ROADMAP.md` — gap analysis vs `flexnetd` v1.0.0, item status,
  recommended order. Update item rows when shipping; don't leave
  "DONE in vNext" hanging when vNext has shipped.
- `V1.x_DESIGN.md` — per-version design note. Created when a
  release introduces non-trivial wire-format or behaviour
  changes. Reference these from `ROADMAP.md` and commit messages
  so future agents can find the reasoning.

After **any** infrastructure or code change, update both the
local repo docs AND the corresponding GitHub-hosted copies.
Documentation is part of "done."

---

## 7. Language for peer-behaviour claims

Anything that ships to the public repo — README, ROADMAP, design
docs, code comments, commit messages — must describe peer
behaviour in **observation** terms, not **inference** terms.

| Avoid (inference) | Prefer (observation) |
|---|---|
| "Disasm of the peer shows…" | "Captures show that the peer…" |
| "Reverse engineering revealed…" | "Empirical testing shows…" |
| Function offsets, opcodes | Wire-format byte layouts |
| Names of peer executables | "The current peer implementation…" |
| "We figured out from the binary…" | "Observed on the live network…" |

This isn't pedantry: the project is framed as a protocol
implementation, not as analysis of someone else's binary. Keep
the framing clean. Internal notes are exempt; public artefacts
are not.

---

## 8. Investigation tooling

Lives under the parent `Xnet_investigation_agent/` repo:

- `xnet_agent.py` — telnet+SYS into an xnet node, take baseline
  snapshots (L, L\*, D, U, P), run `monitor +N` for a duration,
  save everything to JSON. Supports `--monitor-cmd` for
  arbitrary monitor scopes ("monitor" for all ports, or
  "monitor +N" for one port).
- `xnet_d_send.py` (ephemeral helper, recreate as needed) — log
  in to an xnet node, elevate to SYS, issue one or more `D <call>`
  queries. Used to drive specific queries while a separate
  capture is running.
- `bpq_d_query.py` (ephemeral helper) — same shape but for
  linbpq's BPQ telnet (port 2323). Used to verify D output
  end-to-end after deploys.
- `analyze_dual_capture.py` (ephemeral helper) — load two JSON
  captures, summarise PID histogram, extract CE type-6/7 frames,
  identify shared QSO keys across the two captures (forwarding
  evidence).
- `d_count_marks.py` (ephemeral helper) — counts marked vs
  unmarked rows from `D *` output. Used to track path-cache
  coverage over time after a restart.

Treat these as small composable tools, not as a framework.
Recreate or extend them as the question demands.

---

## 9. Project history milestones

A short index so the commit log makes sense:

- **v1.0 – v1.1**: basic CE/CF on the wire, outgoing connections,
  incoming acceptance, FL/D commands, debug build option.
- **v1.2**: node-identity preservation in outbound digi chain
  (LinBPQ equivalent of `AX25_IAMDIGI`).
- **v1.3.x**: P1 protocol-correctness sweep — L3RTT c1–c4
  counters, link-down guard, NetRom L3 INFO envelope on replies,
  link-time IIR filter, dtable RTT=0 skip, 180 s keepalive, 300 s
  proactive-KA threshold.
- **v1.9 (pre-GA)**: P2 M5 path discovery. CE type-6/7 PATH_REQ/
  REP with the corrected wire format (PATH_REQ names the next-
  hop neighbour, peers forward and accumulate). 4 h path cache
  TTL. D command renders cached chain or local-walk fallback. D
  list shows `!` in the new `Path` column for resolved entries.
- **v2.0 GA**: soak v1.9, lift the dest-table capacity side of
  #9, parity statement vs `flexnetd`.

---

## 10. Open items beyond GA (v2.x)

These are deliberately deferred past GA. Don't conflate them with
v2.0 polish:

1. **On-disk path cache** with ≤5 h freshness window. Persist
   `path_hops[] + path_updated` on shutdown, reload at startup.
   Eliminates the ~3 h post-restart re-probe warm-up.
2. **Full portability into `flexnetd`.** Extract CE type-6/7
   build/parse, QSO allocator, probe table into a shared
   protocol module consumed by both repos via per-repo adapters.
3. **Multiple FlexNet neighbours per port and across ports.**
   Today `flex_send_path_req` picks the first active session;
   needs target-driven port + neighbour selection touching the
   probe path, D list rendering, and the session table.
4. **SSID-range "application" mapping.** Each SSID 0–15 on the
   node call binds to an application (BBS, chat, mail gateway).
   Expose as a configurable bind table.

---

## 11. When in doubt

- Search this repo and `flexnetd` before asking the maintainer.
- A captured frame outranks a commit message; a commit message
  outranks a memory; a memory outranks a guess.
- Small commit, clear title, single concern. The git log is the
  audit trail.
- If you change wire behaviour, capture before and after. Attach
  the relevant frame summary to the commit message.

Author: IW2OHX. Updated alongside `v1.9`, 2026-05-12.
