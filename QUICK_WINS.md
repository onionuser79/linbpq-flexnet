# linbpq-flexnet — Quick Wins

Opportunistic improvements that can be picked up at any time without
affecting the v2.0 GA timeline. Each item is small (≤ a few hours of
work), low-risk, and additive.

Each entry has:
- **Effort** — rough size band.
- **Risk** — what could go wrong, how to mitigate.
- **Value** — why it's worth doing.

If an item is picked up, move it from this list to a follow-up commit
message; don't bundle multiple items into one commit unless they touch
the same file for the same reason.

---

## Code hygiene

### 1. Remove dead `flexnet_l3.c` / `flexnet_l3.h` module
- **Effort**: 30 min.
- **Risk**: low — the module compiles to `flexnet_l3.o` but is not
  linked from `FlexNetCode.c`. Verified dead since 2026-05-10. Risk
  is in missing some `extern` reference; build will catch it.
- **Value**: removes ~700 lines of unused source, ~1 KB of unlinked
  object. Reduces confusion for new readers ("what does this do?").
  Drops a build-time artefact from the Makefile.
- **Note**: the protocol surface ideas in those files (CREQ/CACK/INFO
  builders, connection table) remain valid as a future shared module;
  preserve them in a tag or branch before deletion if anyone may want
  to revive.

### 2. Doxygen-style comments on public `FlexNet_*` API in `asmstrucs.h`
- **Effort**: 1 h.
- **Risk**: none — comments only.
- **Value**: IDE hover docs, easier onboarding, makes the public
  surface (`FlexNet_FindRoute`, `FlexNet_GetNeighborCall`,
  `FlexNet_CheckIncoming`, `FlexNet_IsPeerFlexNetMapped`,
  `FlexNet_CmdDest`, `FlexNet_CmdLinks`, `FlexNet_ProcessCE`,
  `FlexNet_ProcessCF`, `FlexNet_Init`, `FlexNet_Timer`) self-documenting.

### 3. Convert magic numbers in `FlexNetCode.c` to `#define` constants
- **Effort**: 1 h.
- **Risk**: very low — purely textual.
- **Value**: e.g. `60` (path-probe interval seconds), `300`
  (re-advertisement interval — now unused after v1.9.7), `45`
  (link-time fold), `8` (max digis), `14400` (path-cache TTL) — naming
  these makes intent obvious and tuning safer.

---

## Observability

### 4. Daily / size-based rotation for `/tmp/flexnet_axudp.log`
- **Effort**: 1 h.
- **Risk**: low — rotate at startup if file exceeds N MB or older than
  24 h; rename to `.1`, etc. No dependency on logrotate.
- **Value**: prevents unbounded growth in production. Currently the
  log can grow to many MB after weeks of uptime.

### 5. `CE-UNKNOWN` counter exposed in `FL` output
- **Effort**: 30 min.
- **Risk**: low — increment in the existing default branch in
  `FlexNet_ProcessCE`, display in the `FL` summary line.
- **Value**: at-a-glance check that our parser is keeping up with what
  peers send. A non-zero count is the operator's hint to investigate.

### 6. Periodic FlexNet session-table dump to log
- **Effort**: 1 h.
- **Risk**: low — `FlexNet_Timer` already runs every tick; emit a
  summary line every N minutes.
- **Value**: post-mortem analysis of session lifecycles when something
  goes wrong, without rebuilding with `FLEXNET_DEBUG`.

---

## Operator UX

### 7. `D` sort options — _shipped in v2.1.9_

### 8. `D !` and `D ?` filters — _shipped in v2.1.9_

### 9. `FLEXPROBE <call>` BPQ console command
- **Effort**: 2 h.
- **Risk**: medium — adds a new admin command; need to plumb into
  `flex_send_path_req` and respect the 8-slot pending table; only
  allow if `Session->Secure_Session` (so it's sysop-only).
- **Value**: force a type-6 PATH_REQ to a specific destination on
  demand instead of waiting for the 60 s round-robin. Useful for
  debugging when a target's `path_hops[]` looks stale.

---

## Build & dev workflow

### 10. `make smoke-test` target
- **Effort**: 2 h.
- **Risk**: low — wraps `sync-and-build.sh all` + a minimal connect
  probe (`C IGATE-0` from BPQ-13 via `/tmp/raw_telnet_probe.py`),
  returns non-zero on failure.
- **Value**: catches regressions before a deploy. Could later run as
  a pre-commit hook or in CI.

### 11. Stash-management helper
- **Effort**: 30 min.
- **Risk**: none — a small shell script under `tools/`.
- **Value**: `tools/list-stashes.sh` lists git stashes with their
  full labels so investigators don't have to remember
  `abandoned-v1.9.6-transit-digipeat-wip-2026-05-14`.

---

## How to use this list

- **Cherry-pick freely.** No ordering implied.
- **One commit per item** unless two items touch the same file for the
  same reason.
- **Don't block the GA timeline** for any of these. If the v2.0 GA
  release engineering is in progress, defer quick wins until after.
- **Strike items as they ship** — move to the `## Shipped` section
  below with the commit hash, so this file doesn't just grow.

---

## Shipped

- **#7 — `D` sort options** (v2.1.9). `D /COST` (ascending RTT),
  `D /CALL` (alphabetical), `D /AGE` (freshest cached path first).
  Combinable with all other filters.
- **#8 — `D !` and `D ?` filters** (v2.1.9). `!` = cached-path only,
  `?` = uncached only. Combinable with sort, callsign filter, and
  the new via-neighbour filter.
- **`D < <neighbour>` via filter** (v2.1.9, not originally in the
  quick-wins list but added in the same batch). Lists only routes
  whose chosen neighbour matches the given call. Accepts an optional
  SSID (`D < IW2OHX-14`) or base call (`D < IW2OHX` matches any SSID).

---

_Document version: 2026-05-17 — items #7 and #8 shipped in v2.1.9 along with the `D < <neighbour>` via filter._
