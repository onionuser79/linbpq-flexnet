# QA v2.7.0 — soak — v2.7.0-soak-20261016T0903Z
Verdict: CONDITIONAL  (baseline none; regressions: 0)
Pollers during the run: 2 on gw (station-dashboard */15, xnet-web-status */10), as in every campaign since 2026-10-12.

**Stopped early by the operator** at 3.33 h of the 24 h soak profile
(11 snapshots), to move on to the next release. The numbers below are
valid for 3.3 h; the soak duration criterion is not met.

## Summary
- 87 PASS, 6 WARN, 0 FAIL. All three LinBPQ nodes on v2.7.0, silent and flexdebug flavours as expected.
- Reachability clean: 27/27 connects first try, transit 4/4 legs extended, 15/15 path queries valid.
- Routing clean: coverage 100 % on all nodes, median cost ratio 1.33–1.36, no orphans, no vanish-and-return.
- Links: one drop in 3.3 h (-12 ↔ IQ2LB RF KISS port 3), explained by RF; every other link never left CONNECTED.
- None of the six WARNs is ours. Nothing in v2.7.0 (FLEXPROBE, FL Parser line) touches the paths involved.

## Bugs (ours)
none

## Not ours
- **F-01 LS-03 / F-04 LS-06 / LS-02 (-12 > IQ2LB:3)** — one event: the RF KISS link to IQ2LB went to `L2STATE=4` at 10:28:33Z, was reaped (the v2.6.1 B-5 rule, working as designed) and re-opened by the keeper at 10:29:36Z. IQ2LB reports rr+ 10–13 % on the same path (F-02/F-03). Environment (RF).
- **F-06 RC-07 K1YMI 0-15 climb (123 → 1603)** — the climb starts upstream: IW2OHX-14 (X)Net's own cost goes 128 → 1423 over the run, and every node follows it at +1..+180. One -13 outlier (S8: 2458 against 457 elsewhere) lasted a single sample. Peer / far-end destination.

## Known, unchanged
- **F-05 RC-05 N2MH** — `N2MH 2-2` at cost 4607 held only by -13: the stale row above the finite clamp listed in PLAYBOOK §10 (filed at harness creation, still not triaged to a fix).
- **F-02 / F-03 LS-04** — IQ2LB ↔ IW2OHX-12 RF quality, informational (PLAYBOOK §10).
- RE-KNOWN I-1: NET/ROM-first `C` without FlexNet fallback (ROADMAP), still fails.

## Harness notes
- `qa.sh stop` then `fetch` scores a stopped run as if it were complete; the scorecard does not say "stopped" (the `STOPPED` marker is in the run directory only). The history row (3.33 h, soak) must not be used as a soak baseline.

## Recommendation
Release v2.7.0 (tag + GitHub release): no defect of ours in 3.3 h, all reachability and routing checks pass. The full 24 h soak was waived by the operator.
