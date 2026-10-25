# FlexNet QA Playbook

Station QA for **linbpq-flexnet** and **flexnetd** on the IW2OHX mesh. One
procedure, run the same way every time, producing a scorecard whose
WARN/FAIL lines become the bug list for the next release.

This directory is the station's test harness, not product documentation:
it names real nodes. Product docs stay in the five root files.

| File | Role |
|---|---|
| `PLAYBOOK.md` | this document: what is measured, why, and how results become fixes |
| `qa_config.json` | topology, expected links, test matrix, schedules, thresholds |
| `qa.sh` | the only entry point (macmini): `deploy`, `start`, `status`, `stop`, `fetch`, `list` |
| `qa_run.py` | one campaign on gw, on a fixed schedule |
| `qa_score.py` | campaign directory → `scorecard.json` + `SCORECARD.md` (stdlib only) |
| `qa_lib.py` | parsers (`FL`, `D`, (X)Net `L`, flexnetd state files) and collectors |
| `history.csv` | one row per fetched campaign — the trend across releases |

Results land in `research/qa_runs/<run-id>/`.

---

## 1. Principles — each one cost us a wrong conclusion once

1. **The observer changes the system.** Every telnet session is load, and on
   LinBPQ with `DisconnectOnClose=1` each close froze the node for 1 s —
   longer than (X)Net's ~0.6 s retry budget. "PC/Flexnet's 60 s teardown
   tick" was our own poller. → `DisconnectOnClose=0` is a preflight FAIL;
   pollers are recorded with every campaign (PF-04); the harness polls
   every 20 min, never faster; one campaign at a time.
2. **A post-restart window is not a test bed.** A pinned transit connect
   failed 75 s after a restart and succeeded at convergence. → T0 starts
   only when every expected link has been up ≥ 15 min (PF-05).
3. **Never compare across a restart or a topology change.** Counters reset,
   tables re-seed, cost landscapes move. → process pids are recorded per
   snapshot; a counter that goes backwards is a restart marker, not data;
   `matrix_version` must match before two scorecards are compared.
4. **"Connected" is not evidence; the far end and the counter are.**
   (X)Net acknowledges hop by hop, so "connected" can be the first leg
   only. → success = far-end banner bytes; transit tests require the via
   node's counter to move (`extended` for multi-hop, `xport` for an
   adjacent target on another port) and `declined` not to.
5. **Pin the path.** Unpinned, the peer picks its own best route and the
   test exercises someone else's code. → transit tests name the via node.
6. **Judge by the outlier, not the median.** Ack latency median 17 ms was
   fine; one 1 s stall per hour killed links. → stall watch reports every
   semaphore hold ≥ 250 ms, FAIL on any ≥ 500 ms.
7. **Count events the way they happen.** A link that vanishes from `FL` and
   returns is a restart (a tool that only watched uptime saw 1 of 7);
   teardowns came in pairs 60 s apart (18 = 8 independent). → LS-02
   counts vanish+return as one event and excludes restarts of either end.
8. **A quiet window proves nothing** unless the triggering event happened
   inside it (a `3+` every 75–90 min). → profiles are long enough for the
   behaviour under test; smoke is a go/no-go, not a stability verdict.
9. **Distance-vector bugs are disagreements between tables.** A single
   node's view never shows them. → every snapshot takes all six tables
   within seconds of each other and RC checks compare them.
10. **Classify ownership before blaming the code.** IW2OHX-4 flapped on its
    own; an RF link is lossy; a destination left the mesh. → every finding
    is triaged ours / peer / environment / harness (§7).

---

## 2. What to run, when

| Trigger | Profile | Nodes on the new build | Gate |
|---|---|---|---|
| Release candidate on IR2UFV | `smoke` (1 h) | IR2UFV | no FAIL; WARNs explained |
| Release (any version bump) | `standard` (6 h) | all three LinBPQ nodes | **GO**, or CONDITIONAL with every WARN triaged as not-ours |
| Change to session lifecycle, advertisement or forwarding code; minor/major release | `soak` (24 h) | all three | GO |
| flexnetd release | `standard` | IW2OHX-3 | FD checks PASS, no LinBPQ regression |
| Topology change (new/removed peer, port, node role) | edit `qa_config.json`, bump `matrix_version`, run `standard` as the new baseline | — | — |
| Weekly health (optional) | `standard` | as deployed | trend in `history.csv` |

Release rule: **no tag until the gate passes on the build being tagged.**
A build that changes after the campaign started is a new campaign.

---

## 3. The campaign, minute by minute

Every campaign of a profile performs the same actions at the same offsets
from **T0**. T0 is when the convergence gate passes, not the launch time.

```
launch ─ preflight ─ convergence gate (poll 5 min, max 60 min) ─ T0
         │                                                       │
         versions, binaries, cfg invariants, pollers             snapshots every 20 min (S0..Sn)
                                                                 connect rounds, path-query rounds
                                                                 stall watch (whole window)
                                                                 └ end: wire audit → scorer → DONE
```

| Profile | Duration | Snapshots | Connect rounds at | Path-query rounds at |
|---|---|---|---|---|
| smoke | 60 min | 0, 20, 40, 60 (4) | T0+5 | T0+30 |
| standard | 360 min | every 20 min (19) | T0+10, T0+340 | T0+40, T0+300 |
| soak | 1440 min | every 20 min (73) | T0+10, +360, +720, +1080, +1420 | T0+40, +700, +1380 |

A snapshot = `FL +` (stored under the key `FL`) + `D` on each LinBPQ node, `L` + `D` on each (X)Net node,
flexnetd's `linkstats` + `destinations`, gw process pids — all in parallel,
~5 s. A connect round = every outbound and transit test in config order,
10 s apart. A path-query round = `D <dest>` for 5 destinations from each
LinBPQ node, 30 s cap each.

---

## 4. Check catalogue

Status rules: values at or better than `pass` → PASS, at or better than
`warn` → WARN, otherwise FAIL. Thresholds live in `qa_config.json`; this
table explains them. `·` = informational, never part of the verdict.

### 4.1 Preflight (PF)

| ID | Measures | FAIL when | Why |
|---|---|---|---|
| PF-01 | `V` FlexNet version on every LinBPQ node | ≠ `--expect-version` | testing the wrong build is the costliest mistake |
| PF-02 | `'FlexNet: '` strings in each gw binary | silent > 5, flexdebug ≤ 5 | a `-D` flag change without `clean` silently builds the other flavour |
| PF-03 | `DisconnectOnClose=0` (FAIL), `OnlyVer2point0=1` (WARN) in every LinBPQ cfg | missing | 1 s freeze per telnet close; XID costs one teardown per peer per restart |
| PF-04 · | pollers in gw crontab + macmini LaunchAgents | — | state them with every number |
| PF-05 | convergence gate | links not up ≥ 900 s within 60 min | principle 2 |

### 4.2 Link stability (LS)

| ID | Measures | PASS / WARN | Why |
|---|---|---|---|
| LS-01 | samples where an expected link is not CONNECTED | 0 / 1 | the link matrix is the contract |
| LS-02 | unexplained session restarts per link, per 6 h (vanish+return or uptime reset; restarts of either end's process excluded) | AXUDP/KISS 0 / 1 · RF 2 / 6 | the headline stability number |
| LS-03 | `FL Sessions: reaped` delta per node | 0 / 1 | reaper firing under a live L2 (v2.6.0 B-5) |
| LS-04 | (X)Net's view of our link, per port: Q/T p90, rr+ max | AXUDP Q/T ≤ 10, rr+ ≤ 2 % · RF informational | the peer's opinion is the one that tears links down |
| LS-05 | share of samples with `Queued == 0` per peer | ≥ 90 % / ≥ 75 % | a queue that never drains was the 2026-09 advertisement storm |
| LS-06 | keeper `opens` delta on keeper-managed links | 0 / 2 | each open is a link that dropped and was re-opened |
| LS-07 | semaphore holds ≥ 250 ms (gw nodes); FAIL on any ≥ 500 ms | 0 / 2 | principle 6 |

### 4.3 Routing coherency (RC)

Reference table: `IW2OHX-14` ((X)Net, the main route source). Rows a node
legitimately lacks are excluded: its own call, the reference's own call
((X)Net never advertises itself to a neighbour), its local/external calls.

| ID | Measures | PASS / WARN | Why |
|---|---|---|---|
| RC-01 | coverage of the reference table (median over samples) | ≥ 99 % / ≥ 97 % | missing routes = unreachable stations |
| RC-02 | rows held by this node and by no other node | 0 / 3 | stale routes after a withdrawal (B-2), phantoms |
| RC-03 | \|Σ FL Routes − D rows\| / D rows | ≤ 1 % / ≤ 3 % | the two internal views must agree (B-2) |
| RC-04 | median cost ratio vs reference; outliers (> 3× and > 20) | ≤ 2.0 and ≤ 2 / ≤ 3.0 and ≤ 6 | inflated costs push traffic away from us |
| RC-05 | base calls whose SSID-range set differs from the reference | 0 / 3 | neighbour entry clobbering a range (B-3) |
| RC-06 | rows that vanish and return between samples, per 6 h | 0 / 2 | false withdrawals (B-1: 139 prunes/h) |
| RC-07 | rows climbing ≥ 3 samples in a row to ≥ 4× their minimum | 0 / 2 | count-to-infinity |
| RC-08 | a node holding a route to itself | 0 | never offer a neighbour its own call |
| RC-09 | drift of the reference's cost for each of our nodes | ≤ 10 / ≤ 50 | our link-time / keepalive cadence as (X)Net sees it |

### 4.4 Reachability (RE)

| ID | Measures | PASS / WARN | Why |
|---|---|---|---|
| RE-01 | first-try connect success on gated tests (far-end banner) | ≥ 95 % / ≥ 85 % | the user-visible result |
| RE-02 | connect latency p90 per link class | AXUDP ≤ 2 s / ≤ 5 s · RF ≤ 10 s / ≤ 30 s | slow = retries somewhere |
| RE-03 | pinned transit with the expected counter moving and `declined` flat | 100 % / ≥ 80 % | principle 4–5 |
| RE-04 | `D <dest>` route lines that end at the destination with ≤ 8 digis | ≥ 90 % / ≥ 70 % | path discovery |
| RE-KNOWN · | known-issue tests (e.g. NET/ROM-first `C`) | — | tracks a ROADMAP item until it flips to PASS |
| RE-SKIP · | tests skipped because the destination left the reference table | — | environment, not us |

### 4.5 flexnetd (FD) and flexdebug wire audit (WD)

| ID | Measures | PASS / WARN | Why |
|---|---|---|---|
| FD-01 | flexnetd destinations vs reference | ≥ 95 % / ≥ 90 % | IW2OHX-3 participates correctly |
| FD-02 | `server already sent` lines in flexnetd's journal | 0 | the unanswered re-INIT fixed in v1.0.1 |
| WD-01 | IR2UFV path probes answered | ≥ 70 % / ≥ 50 % | baseline 74–89 % |
| WD-02 | `CE-UNKNOWN` + `L2FWD-DECLINE` + `LEARNED-AGE` lines | 0 / 5 | parser gaps, refused forwarding, a removed prune coming back |
| WD-INFO · | REINIT, RESEED, POISON, HOLDDOWN, CLIMB-*, L2-SABM-NEW, PCF-QUIESCE, PATH-TIMEOUT counts | — | context for triage |

---

## 5. The test matrix

Defined in `qa_config.json` (`outbound`, `transit`, `path_queries`).
Chosen so that each code path the releases touched has at least one test.

| ID | From | Target | Exercises |
|---|---|---|---|
| OB-NBR | all LinBPQ | IW2OHX-14 | single-digi chain to a direct neighbour |
| OB-DE / NL / IT | all LinBPQ | DK0WUE / PE1FAM / IR3UGM | multi-hop FlexNet, three directions |
| OB-IGATE | all LinBPQ | IGATE | the interconnect gateway |
| OB-PCF | all LinBPQ | IK1NHL-4 | a PC/Flexnet far end |
| OB-LONG · | IR2UFV | DB0FFL | 8 hops, the digipeater limit |
| OB-RF | -12 | IQ2LB | (X)Net neighbour on RF KISS |
| OB-EXT | -12, IR2UFV | IQ2LB-6 | `FLEXNETEXTERNAL` station |
| OB-KISS | IR2UFV | IW2OHX-13 | FlexNet over KISS, target not in NODES |
| OB-KNOWN · | -13 | IR1UAW-10 | known issue I-1 (NET/ROM-first `C`) |
| TR-LOCALSSID-14 | -14 | IR2UFV-8 | own-range SSID delivered locally |
| TR-LOCALSSID-4 | -4 | IR2UFV-8 | same, through -12's transit (`extended`) |
| TR-12-DE | -4 | DK0WUE via -12 | pinned multi-hop L2 transit (`extended`) |
| TR-12-RF | IQ2LB | F3KT via -12 | RF → AXUDP transit (`extended`) |
| TR-UFV-XPORT | -14 | IW2OHX-13 via IR2UFV | AXUDP → KISS cross-port (`xport`) |

A test whose destination is absent from the reference table at the
preceding snapshot is SKIPped as environment (RE-SKIP), never failed.

---

## 6. Reading the scorecard

`SCORECARD.md` has four parts: the verdict, regressions against the
baseline campaign, the findings to triage (every WARN/FAIL, numbered F-nn,
with a "look first" pointer), and the full check table.

**Verdict.** GO = no WARN/FAIL in gated checks. CONDITIONAL = WARNs only.
NO-GO = at least one FAIL. A CONDITIONAL release needs every WARN triaged
as peer/environment, or accepted by the operator in writing in the
campaign report.

**Regressions.** `qa.sh fetch <run> <baseline-run>` marks every check whose
status got worse than in the baseline (normally the last GO campaign of
the previous release with the same `matrix_version`). A regression is
always a finding, even when its status is only WARN.

**Trend.** `history.csv` keeps one row per campaign: verdict, counts,
connect / transit / path success, restarts per 6 h.

---

## 7. From finding to fix

Every F-nn in a scorecard goes through the same five steps. The output is
`research/qa_runs/<run-id>/QA_REPORT.md`, modelled on
`research/qa_v2.6.0_2026-10-11/QA_REPORT.md`.

**1. Ownership.** Decide which one, with evidence:

| Owner | Typical evidence |
|---|---|
| ours | wrong behaviour visible in our `FL`/`D`/trace; reproducible against a healthy peer |
| peer | the peer misbehaves against other peers too (e.g. -4 flapping against PC/Flexnet) |
| environment | RF loss, a destination that left the mesh, a peer restart, Internet path |
| harness | parser or matrix error — fix the harness, re-score; never patch the node for it |

**2. Rule out the usual non-bugs** before touching code:

- a restart inside the window (pids, `Sessions:` history, peer uptimes);
- a poller or stall (PF-04, LS-07);
- for connects: read the `route:` line and the reference `D` first — four
  known non-bug causes of a missing route line: > 8 digis, probe timeout,
  peer prefers another port, stale path cache;
- for transit: was the path pinned, did the counter move?

**3. Reproduce narrowly.** One deep-dive at a time, on IR2UFV where possible:

| Question | Tool (in `tools/`) |
|---|---|
| who tore the link down, and what preceded it | `capture-steady.sh` → `axudp_teardown.py --local-ip … --link-only`, `disc_context.py`, `frames.py` |
| was the node slow to answer | `ack_latency.py`, `poll_latency.py` |
| is the node stalling, and where | `sem_watch.py`, then `sem_stack.py` (gdb, names the callee) |
| what did we advertise, to whom | `parse_advertise.py`, `advert_breakdown.py`, `flexnet_transaction_audit.py` |
| did a frame really propagate | dual-port capture + `analyze_dual_capture.py` (shared QSO key) |
| what happened in the trace | IR2UFV flexdebug log — slice from the last `PATH-CACHE-LOAD` (time-only stamps span days). Rotated nightly on gw since 2026-10-16: older days are in `.log.1` / `.log.N.gz`; `wire_audit` stitches `.1` itself |

Captures run only when the operator agrees (they were stopped on
2026-10-12); `/tmp` on gw is RAM — copy anything worth keeping.

**4. Record the bug** in `QA_REPORT.md`:

```
### B-n (HIGH|MEDIUM|LOW) — one-line statement of the defect
Check: <ID> <scope> (F-nn in SCORECARD.md)    Owner: ours
Symptom: <numbers from the scorecard>
Evidence: <trace lines / table rows / capture times>
Mechanism: <what the code does, file:function>
Fix direction: <what to change; what NOT to change>
Verification: <check ID(s) that must turn PASS> + <unit test name>
```

Severity: HIGH = loses routes or links for users; MEDIUM = inconsistency
that can become an outage; LOW = cosmetic or efficiency.

**5. Fix and close the loop.**

- Fix on a branch; add a unit runner in `tools/unit/` that fails without
  the fix (mutation-check it).
- Deploy to IR2UFV (flexdebug) → `smoke` campaign → the named check(s)
  must turn PASS.
- Merge, release build on all three → `standard` with
  `fetch <run> <baseline>` → GO and no regressions → tag.
- Wire lesson learned? Update `PROTOCOL_SPEC.md` (byte-identical in
  flexnetd). New directive? `README.md`. Every release: `RELEASE_NOTES.md`
  + node MOTDs.
- A finding not fixed in this release goes to `ROADMAP.md` (product
  wording, fictional calls) and stays visible as RE-KNOWN or a tracked WARN.

Fix order is the operator's decision; propose by severity, and fix one
mechanism per build so the next campaign can attribute the change.

---

## 8. Rules of engagement

- **Production restarts are the operator's call.** IW2OHX-12 (Windows) can
  only be restarted by the operator. The harness never restarts anything.
- **One campaign at a time** (`qa.sh start` refuses a second).
- **Disruptive tests are out of the harness** and need explicit approval
  each time: withdrawal tests (`d2d3-poison-test.sh`), link drops, peer
  restarts, `ro fl del` on (X)Net.
- **No ad-hoc node polling during a campaign** — every extra telnet session
  is in the measurement. Read the campaign files instead.
- **Never `pkill -f` over ssh** with a pattern in the command line — it
  matches the ssh's own shell. `qa.sh stop` signals the process group.
- A campaign started across a deploy is void: stop it, start a new one.

---

## 9. Maintaining the harness

- Topology change → edit `expected_links`, `xnet_views`, the matrix; bump
  `matrix_version`; run `standard` to set a new baseline.
- New check → add it to `qa_score.py` with a `TRIAGE` hint, a threshold in
  `qa_config.json`, and a row in §4 with *why it exists*. A check without a
  lesson behind it is noise.
- Threshold change → note it in the next campaign report; earlier
  scorecards keep their own thresholds embedded.
- `qa.sh deploy` after every harness change; `fetch` re-scores locally, so
  an old campaign can be re-read with a newer scorer.

## 10. Known state at harness creation (2026-10-12, v2.6.2)

Read-only selftest on the live mesh, 2 snapshots: all 11 expected links
CONNECTED, coverage 99.5–100 %, median cost ratio 1.34–1.38, (X)Net Q/T 3
for every LinBPQ link. Already-known items — do not re-file them:

- IW2OHX-13 lacked `OnlyVer2point0=1` (PF-03 WARN) — added to its cfg
  2026-10-12, active since the -13 restart at 16:01:49Z.
- IW2OHX-13 holds `N2MH 2-2` at cost 4609, held by no other node (RC-02,
  RC-05): a stale row above the 4095 finite clamp — first real finding of
  the harness, to triage in the first campaign.
- IQ2LB ↔ IW2OHX-12 RF port 1: Q/T 73, rr+ 34 % (LS-04, informational).
- I-1 NET/ROM-first `C` without FlexNet fallback (RE-KNOWN).
- Stock-LinBPQ SABM collision half-open link after a peer restart (ROADMAP).
- `-13 FL Direct 2` for IR2UFV (cosmetic, v2.6.1).
- IW2OHX-4 has a single FlexNet link (to -12); it is an observer only.
