# QA health — smoke — health-smoke-20261121T1717Z
Verdict: NO-GO by the scorer → triaged **CONDITIONAL** (baseline none; regressions: 0)
Pollers during the run: station-dashboard */15, xnet-web-status */10 (PF-04)

Context: health smoke after the IW2OHX-4 restart for the rhp-bridge cutover (cfg change APPLICATION
1/3/11 only, LinBPQ v2.11.1 unchanged on all 3 nodes). T0 17:34:52Z, 1.04 h, matrix 2026-11-18.1.

## Summary
- All 6 links CONNECTED in every sample, 0 restarts, 0 reaps during the window (LS-01/02/03/06 PASS).
- Coverage 100 % on all three nodes, cost ratio 1.23–1.28, transit TR-4-XPORT PASS, path queries 15/15.
- Both FAILs come from the environment, not the build: the IQ2LB observer was unreachable for the whole
  run (F-02), and WD-02 counted one user-session teardown burst on IR2UFV (F-06, see B-1 candidate).
- The -4 restart and the rhp-bridge cutover had no effect on FlexNet.

## Bugs (ours)
### B-1 (CONFIRMED by source read 2026-11-21) — replies we build from a received frame drop our NODECALL* mark
- Symptom: IR2UFV flexdebug 18:41:32 local (17:41:32Z). The session is the harness connect OB-NL
  IW7EAS@IR2UFV → PE1FAM via -14. PE1FAM sends DISC. IR2UFV answers `IW7EAS -> PE1FAM IR2UFV IW2OHX-14 ctl=UA`
  with **IR2UFV unmarked** (every I frame of the session carried `IR2UFV*`). -14 does not relay it, so PE1FAM
  retries DISC. The retries find no session: `L2FWD-DECLINE ... dest not in our table` ×10 (= WD-02 FAIL), and
  each one is digipeated back to -14.
- Mechanism (ours):
  - Our FlexNet connect builds `USER -> DEST via MYCALL* [NEIGHBOUR]` (`Cmd.c` ~2920-2935, v1.2 / v2.1.8
    chain shape).
  - Session frames keep `MYCALL*`, because they come from `LINK->DIGIS`.
  - A response built from the received frame does not. That covers `L2SENDRESP` → UA to DISC (`L2Code.c` ~1244-1249), DM and FRMR. These go
    through stock `L2SWAPADDRESSES`, which clears every H-bit.
  - The v2.3 repair hook `FlexNet_MarkLocalDigi()` (`FlexNetCode.c` ~11206, called at `L2Code.c` ~2046)
    sets `MYCALL*` back **only when the source is a FLEXNETLOCAL call**. A telnet user such as IW7EAS is not
    one. `tools/unit/test_local_calls.c` even pins this: "unbound source: untouched".
- Impact: every DISC from the far end of a FlexNet connect made by a node user gets an UA that cannot be
  delivered. The peer ends by N2 retries, about 20 extra frames per teardown, plus one stray digipeat per retry.
  Same shape for a DM (e.g. SABM to a dead session). Data transfer is not affected.
- Proposed fix (not implemented):
  - In `FlexNet_MarkLocalDigi()`, set the H-bit whenever the first digi of a frame **we originate** is
    MYCALL and the source is not MYCALL itself. We are the sender, so our own node call has by definition
    been "repeated". Drop the FLEXNETLOCAL-only condition.
  - Update the "unbound source" unit case to expect H set, and add cases for UA/DM via MYCALL* NEIGHBOUR.
- Verification: IR2UFV trace shows `UA ... IR2UFV* IW2OHX-14` after a remote DISC; WD-02 = 0 in a smoke on the rc.

## Not ours
- F-02 LS-04 IQ2LB sees IW2OHX-4: FAIL `None`. IQ2LB telnet 44.134.24.18:23 was unreachable for the whole run
  (snapshot errors ×4, RE-SKIP IQ2LB>F3KT TimeoutError). It was still unreachable after the run (ping 0/2,
  TCP 23 closed). Environment, the HAMNET/IQ2LB path. The RF links to IQ2LB stayed CONNECTED on -4.
- F-01 LS-04 -14 sees -4 port 1: rr+ 2.7 % (threshold 2 %), Q/T 2. A marginal one-sample peak on the AXUDP link
  as the peer sees it.
- RE-01 IW2OHX-4>C IQ2LB: one 90 s failure (6/7 from -4). RF path to IQ2LB.

## Known, unchanged
- F-03 RC-02 IR2UFV `IW2OHX 2-2`: this is the WAMPES IW2OHX-2 on the `F>` private link. Only IR2UFV holds it,
  by design (PLAYBOOK §11).
- F-05 RC-05 IR2UFV `IW2OHX`: same cause (extra 2-2 range from the private WAMPES link).
- F-04/F-05 RC-05 `IQ2LB` on -13 and IR2UFV: both hold an extra `IQ2LB 0-0` at cost 86 next to `0-5` and
  `6-6`; -4 does not hold it. Both learn only from -14, and after the run -14 `D IQ2LB` shows only `0-5`. This is a long-path
  advertisement learned via -14 that overlaps the direct range. Watch it in the next `standard` run. Not filed.
- RE-KNOWN I-1 NET/ROM-first `C` without FlexNet fallback (ROADMAP v2.12).

## Harness notes
- **Fixed 2026-11-21, after triage, and re-scored in place (scorecard now 68/3/1, SKIP 1):**
  - LS-04 now scores SKIP when the observer was unreadable in every snapshot. F-02 is now SKIP.
  - RC-02/RC-05 now exclude the peers of `"private": true` expected links. The flag is set on IR2UFV↔IW2OHX-2
    WAMPES. F-03 is gone, and F-05 drops to the IQ2LB item only.
  - `matrix_version` is unchanged: no topology change. The `qa/history.csv` row keeps the original
    67/4/2 numbers.
- The verdict stays NO-GO by the scorer because of WD-02 (B-1 candidate).

## Recommendation
The mesh is healthy after the -4 restart; no action is needed on the build. Proposed order:
1. The LS-04 SKIP harness fix and the `F>` exclusion.
2. B-1 is confirmed as ours; the fix is proposed (one function + unit test). Next step: an rc on IR2UFV, then a smoke.
3. The v2.11.x `standard` campaign (the first possible baseline) once IQ2LB is reachable again.
