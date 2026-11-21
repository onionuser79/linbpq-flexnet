# QA scorecard — health (smoke)

**Verdict: NO-GO** · T0 2026-11-21T17:34:52Z · 1.04 h · 4 snapshots · matrix 2026-11-18.1

| PASS | WARN | FAIL | SKIP | INFO |
|---|---|---|---|---|
| 67 | 4 | 2 | 0 | 9 |

## Findings to triage (WARN/FAIL)

### F-01 WARN LS-04 — IW2OHX-14 sees IW2OHX-4 (port 1)

- value: `{'qt_p90': 2, 'rr_max': 2.7}`
- evidence: Q/T p90 2 (max 2), rr+ max 2.7%
- look first: link quality as the peer sees it: ack latency / stalls (tools/poll_latency.py, ack_latency.py on a capture).
- owner (ours / peer / environment / harness): _triage_

### F-02 FAIL LS-04 — IQ2LB sees IW2OHX-4

- value: `None`
- evidence: no F row for us in the observer's L table
- look first: link quality as the peer sees it: ack latency / stalls (tools/poll_latency.py, ack_latency.py on a capture).
- owner (ours / peer / environment / harness): _triage_

### F-03 WARN RC-02 — IR2UFV

- value: `1.0`
- evidence: last sample: IW2OHX 2-2
- look first: orphan rows only we hold: stale dest after source withdrew (QA v2.6.0 B-2) or a phantom loop.
- owner (ours / peer / environment / harness): _triage_

### F-04 WARN RC-05 — IW2OHX-13

- value: `1.0`
- evidence: last sample: IQ2LB
- look first: SSID range mismatch: neighbour entry <link SSID>-<INIT max> (B-3), flex_call_in_range.
- owner (ours / peer / environment / harness): _triage_

### F-05 WARN RC-05 — IR2UFV

- value: `2.0`
- evidence: last sample: IQ2LB, IW2OHX
- look first: SSID range mismatch: neighbour entry <link SSID>-<INIT max> (B-3), flex_call_in_range.
- owner (ours / peer / environment / harness): _triage_

### F-06 FAIL WD-02 — IR2UFV

- value: `10`
- evidence: {'CE-UNKNOWN': 0, 'L2FWD-DECLINE': 10, 'LEARNED-AGE': 0}
- look first: flexdebug trace: CE-UNKNOWN = parser gap, L2FWD-DECLINE = forwarding refused, LEARNED-AGE = removed prune came back.
- owner (ours / peer / environment / harness): _triage_

## All checks

| ID | Scope | Status | Value | Detail |
|---|---|---|---|---|
| PF-01 | all | PASS | `{'IW2OHX-13': 'v2.11.1', 'IR2UFV': 'v2.11.1', 'IW2OHX-4': 'v2.11.1'}` | expected v2.11.1; mismatched {} |
| PF-02 | IW2OHX-13 | PASS | `5` | flavour silent: 'FlexNet: ' strings 5 (silent <= 5); sha256 54e76adc97dd |
| PF-02 | IR2UFV | PASS | `100` | flavour flexdebug: 'FlexNet: ' strings 100 (silent <= 5); sha256 41108f219066 |
| PF-03 | IW2OHX-13 | PASS | `DisconnectOnClose=0` |  |
| PF-03 | IW2OHX-13 | PASS | `OnlyVer2point0` |  |
| PF-03 | IR2UFV | PASS | `DisconnectOnClose=0` |  |
| PF-03 | IR2UFV | PASS | `OnlyVer2point0` |  |
| PF-03 | IW2OHX-4 | PASS | `DisconnectOnClose=0` |  |
| PF-03 | IW2OHX-4 | PASS | `OnlyVer2point0` |  |
| PF-04 | gw | INFO · | `2` | pollers active during the campaign (declare them with every number): */15 * * * * /home/iw2ohx/station-dashboard; */10 * * * * /home/iw2ohx/xnet-web-status |
| PF-05 | all | PASS | `{'passed': True, 'waited_s': 1004}` | waited 1004 s for links >= 900 s up |
| LS-01 | IW2OHX-13>IW2OHX-14:3 | PASS | `0` | 0/4 samples not CONNECTED |
| LS-02 | IW2OHX-13>IW2OHX-14:3 | PASS | `0.0` | 0 unexplained in 1.0 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IR2UFV>IW2OHX-14:2 | PASS | `0` | 0/4 samples not CONNECTED |
| LS-02 | IR2UFV>IW2OHX-14:2 | PASS | `0.0` | 0 unexplained in 1.0 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IR2UFV>IW2OHX-2:2 | PASS | `0` | 0/4 samples not CONNECTED |
| LS-02 | IR2UFV>IW2OHX-2:2 | PASS | `0.0` | 0 unexplained in 1.0 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-4>IW2OHX-14:2 | PASS | `0` | 0/4 samples not CONNECTED |
| LS-02 | IW2OHX-4>IW2OHX-14:2 | PASS | `0.0` | 0 unexplained in 1.0 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-4>IQ2LB:3 | PASS | `0` | 0/4 samples not CONNECTED |
| LS-02 | IW2OHX-4>IQ2LB:3 | PASS | `0.0` | 0 unexplained in 1.0 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-4>IQ2LB:4 | PASS | `0` | 0/4 samples not CONNECTED |
| LS-02 | IW2OHX-4>IQ2LB:4 | PASS | `0.0` | 0 unexplained in 1.0 h (per 6 h: 0.0); explained: 0;  |
| LS-03 | IW2OHX-13 | PASS | `0` | reaped 0 -> 0 |
| LS-03 | IR2UFV | PASS | `0` | reaped 0 -> 0 |
| LS-03 | IW2OHX-4 | PASS | `0` | reaped 2 -> 2 |
| LS-04 | IW2OHX-14 sees IW2OHX-13 (port 2) | PASS | `{'qt_p90': 1, 'rr_max': 0.0}` | Q/T p90 1 (max 1), rr+ max 0.0% |
| LS-04 | IW2OHX-14 sees IR2UFV (port 14) | PASS | `{'qt_p90': 1, 'rr_max': 0.0}` | Q/T p90 1 (max 1), rr+ max 0.0% |
| LS-04 | IW2OHX-14 sees IW2OHX-4 (port 1) | WARN | `{'qt_p90': 2, 'rr_max': 2.7}` | Q/T p90 2 (max 2), rr+ max 2.7% |
| LS-04 | IQ2LB sees IW2OHX-4 | FAIL · | `None` | no F row for us in the observer's L table |
| LS-05 | IW2OHX-13>IW2OHX-14#0 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IR2UFV>IW2OHX-2#0 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IR2UFV>IW2OHX-14#1 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IW2OHX-4>IQ2LB#0 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IW2OHX-4>IQ2LB#1 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IW2OHX-4>IW2OHX-14#2 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-06 | IW2OHX-4>IQ2LB:3 | PASS | `0` | keeper opens 1 -> 1 |
| LS-06 | IW2OHX-4>IQ2LB:4 | PASS | `0` | keeper opens 1 -> 1 |
| LS-07 | IW2OHX-13 | PASS | `0` | 0 holds >= 250 ms, 0 >= 500 ms, max 0 ms |
| LS-07 | IR2UFV | PASS | `0` | 0 holds >= 250 ms, 0 >= 500 ms, max 0 ms |
| RC-01 | IW2OHX-13 | PASS | `1.0` | median coverage of IW2OHX-14 100.0%, worst sample 100.0% |
| RC-01 | IR2UFV | PASS | `1.0` | median coverage of IW2OHX-14 100.0%, worst sample 100.0% |
| RC-01 | IW2OHX-4 | PASS | `1.0` | median coverage of IW2OHX-14 100.0%, worst sample 100.0% |
| RC-02 | IW2OHX-13 | PASS | `0.0` | last sample:  |
| RC-02 | IR2UFV | WARN | `1.0` | last sample: IW2OHX 2-2 |
| RC-02 | IW2OHX-4 | PASS | `0.0` | last sample:  |
| RC-05 | IW2OHX-13 | WARN | `1.0` | last sample: IQ2LB |
| RC-05 | IR2UFV | WARN | `2.0` | last sample: IQ2LB, IW2OHX |
| RC-05 | IW2OHX-4 | PASS | `0.0` | last sample:  |
| RC-04 | IW2OHX-13 | PASS | `{'median_ratio': 1.27, 'outliers': 0.0}` | median cost ratio 1.27, outliers 0.0:  |
| RC-04 | IR2UFV | PASS | `{'median_ratio': 1.28, 'outliers': 0.0}` | median cost ratio 1.28, outliers 0.0:  |
| RC-04 | IW2OHX-4 | PASS | `{'median_ratio': 1.23, 'outliers': 0.0}` | median cost ratio 1.23, outliers 0.0:  |
| RC-03 | IW2OHX-13 | PASS | `0.0` | median /sum(FL Routes) - D/ / D = 0.0% |
| RC-06 | IW2OHX-13 | PASS | `0.0` | 0 vanish-and-return:  |
| RC-07 | IW2OHX-13 | PASS | `0` |  |
| RC-08 | IW2OHX-13 | PASS | `0` |  |
| RC-03 | IR2UFV | PASS | `0.0` | median /sum(FL Routes) - D/ / D = 0.0% |
| RC-06 | IR2UFV | PASS | `0.0` | 0 vanish-and-return:  |
| RC-07 | IR2UFV | PASS | `0` |  |
| RC-08 | IR2UFV | PASS | `0` |  |
| RC-03 | IW2OHX-4 | PASS | `0.0` | median /sum(FL Routes) - D/ / D = 0.0% |
| RC-06 | IW2OHX-4 | PASS | `0.0` | 0 vanish-and-return:  |
| RC-07 | IW2OHX-4 | PASS | `0` |  |
| RC-08 | IW2OHX-4 | PASS | `0` |  |
| RC-09 | IW2OHX-13 | PASS | `0` | IW2OHX-14 cost for IW2OHX-13: min 1 max 1 |
| RC-09 | IR2UFV | PASS | `0` | IW2OHX-14 cost for IR2UFV: min 1 max 1 |
| RC-09 | IW2OHX-4 | PASS | `0` | IW2OHX-14 cost for IW2OHX-4: min 1 max 1 |
| RE-KNOWN | IW2OHX-13>IR1UAW-10 | INFO · | `False` | I-1 NET/ROM-first C without FlexNet fallback (ROADMAP v2.12): still fails |
| RE-01 | all | PASS | `0.957` | 22/23 first-try; failed: IW2OHX-4>C IQ2LB (90.2s: ) |
| RE-01 | IR2UFV | INFO · | `1.0` | 5/5 |
| RE-01 | IW2OHX-1 | INFO · | `1.0` | 2/2 |
| RE-01 | IW2OHX-13 | INFO · | `1.0` | 6/6 |
| RE-01 | IW2OHX-14 | INFO · | `1.0` | 3/3 |
| RE-01 | IW2OHX-4 | INFO · | `0.857` | 6/7 |
| RE-SKIP | all | INFO · | `1` | IQ2LB>F3KT: harness error: TimeoutError(110, "Connect call failed ('44.134.24.18', 23)") |
| RE-02 | axudp | PASS | `0.4` | p90 0.4s, max 2.3s over 21 connects |
| RE-02 | rf | PASS | `1.4` | p90 1.4s, max 1.4s over 1 connects |
| RE-03 | all | PASS | `1.0` | TR-4-XPORT r0: ok=True xport+10 declined+0 |
| RE-04 | all | PASS | `1.0` | 15/15 valid;  |
| WD-01 | IR2UFV | PASS | `0.73` | 46/63 path probes answered |
| WD-02 | IR2UFV | FAIL | `10` | {'CE-UNKNOWN': 0, 'L2FWD-DECLINE': 10, 'LEARNED-AGE': 0} |
| WD-INFO | IR2UFV | INFO · | `{'REINIT': 0, 'RESEED': 0, 'POISON': 0, 'HOLDDOWN': 0, 'CLIMB-WITHDRAW': 0, 'CLIMB-SUPPRESS': 0, 'L2-SABM-NEW': 2, 'PCF-QUIESCE': 0, 'PATH-TIMEOUT': 16}` | 131438 log bytes in window |

`·` = informational, not part of the verdict.
