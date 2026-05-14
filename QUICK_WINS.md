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

## Parser robustness

### 4. `CE_FRAME_STATUS_1x` parser entry for `"12\r"`
- **Effort**: 30 min.
- **Risk**: low — strict 3-byte match, no behaviour change beyond
  classification.
- **Value**: removes the only currently-observed `CE-UNKNOWN` log
  entry. Cleaner GA logs. (Also listed in GA work item #2 — pick it up
  there or here.)

### 5. Generic-`1n\r` status family parser
- **Effort**: 1 h.
- **Risk**: low — match the regex `1[0-9]\r` and treat as a status
  notification, with the digit logged for visibility.
- **Value**: defensive — catches any sibling status codes xnet emits
  that we haven't seen yet, without forcing each new digit to fall
  through to `CE-UNKNOWN`.

---

## Observability

### 6. Daily / size-based rotation for `/tmp/flexnet_axudp.log`
- **Effort**: 1 h.
- **Risk**: low — rotate at startup if file exceeds N MB or older than
  24 h; rename to `.1`, etc. No dependency on logrotate.
- **Value**: prevents unbounded growth in production. Currently the
  log can grow to many MB after weeks of uptime.

### 7. `CE-UNKNOWN` counter exposed in `FL` output
- **Effort**: 30 min.
- **Risk**: low — increment in the existing default branch in
  `FlexNet_ProcessCE`, display in the `FL` summary line.
- **Value**: at-a-glance check that our parser is keeping up with what
  peers send. A non-zero count is the operator's hint to investigate.

### 8. Periodic FlexNet session-table dump to log
- **Effort**: 1 h.
- **Risk**: low — `FlexNet_Timer` already runs every tick; emit a
  summary line every N minutes.
- **Value**: post-mortem analysis of session lifecycles when something
  goes wrong, without rebuilding with `FLEXNET_DEBUG`.

---

## Operator UX

### 9. `D` sort options
- **Effort**: 2 h.
- **Risk**: low — sort buffer of dest indices before emission. Don't
  touch the underlying `FlexNetDests[]` ordering.
- **Value**: `D /COST` sorts by ascending cost, `D /CALL` by callsign,
  `D /AGE` by `path_updated`. Operators currently scan a 200-row
  unsorted list — sorting is a big quality-of-life win.

### 10. `D !` and `D ?` filters
- **Effort**: 1 h.
- **Risk**: low — extends the existing filter parser in
  `FlexNet_CmdDest`.
- **Value**: `D !` shows only entries with a fresh cached path
  (`path_hops > 0 && now - path_updated < TTL`). `D ?` shows only
  uncached. Useful for verifying path-probe round-robin progress.

### 11. `FLEXPROBE <call>` BPQ console command
- **Effort**: 2 h.
- **Risk**: medium — adds a new admin command; need to plumb into
  `flex_send_path_req` and respect the 8-slot pending table; only
  allow if `Session->Secure_Session` (so it's sysop-only).
- **Value**: force a type-6 PATH_REQ to a specific destination on
  demand instead of waiting for the 60 s round-robin. Useful for
  debugging when a target's `path_hops[]` looks stale.

---

## Build & dev workflow

### 12. `make smoke-test` target
- **Effort**: 2 h.
- **Risk**: low — wraps `sync-and-build.sh all` + a minimal connect
  probe (`C IGATE-0` from BPQ-13 via `/tmp/raw_telnet_probe.py`),
  returns non-zero on failure.
- **Value**: catches regressions before a deploy. Could later run as
  a pre-commit hook or in CI.

### 13. Stash-management helper
- **Effort**: 30 min.
- **Risk**: none — a small shell script under `tools/`.
- **Value**: `tools/list-stashes.sh` lists git stashes with their
  full labels so investigators don't have to remember
  `abandoned-v1.9.6-transit-digipeat-wip-2026-05-14`.

---

## How to use this list

- **Cherry-pick freely.** No ordering implied.
- **One commit per item** unless two items touch the same file for the
  same reason (e.g. items 4 and 5 both touch the CE parser).
- **Don't block the GA timeline** for any of these. If the v2.0 GA
  release engineering is in progress, defer quick wins until after.
- **Strike items as they ship** — move to the `## Shipped` section
  below with the commit hash, so this file doesn't just grow.

---

## Shipped

_(empty — populate as items land on `main`)_

---

_Document version: 2026-05-14_
