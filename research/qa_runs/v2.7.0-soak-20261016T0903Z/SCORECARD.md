# QA scorecard — v2.7.0 (soak)

**Verdict: CONDITIONAL** · T0 2026-10-16T09:18:45Z · 3.33 h · 11 snapshots · matrix 2026-10-12.1

| PASS | WARN | FAIL | SKIP | INFO |
|---|---|---|---|---|
| 87 | 6 | 0 | 0 | 8 |

## Findings to triage (WARN/FAIL)

### F-01 WARN LS-03 — IW2OHX-12

- value: `1`
- evidence: reaped 0 -> 1
- look first: reaper fired: L2STATE >= 5 rule and grace (QA v2.6.0 B-5); FL Sessions history names the reason.
- owner (ours / peer / environment / harness): _triage_

### F-02 WARN LS-04 — IQ2LB sees IW2OHX-12 (port 0)

- value: `{'qt_p90': 92, 'rr_max': 10.2}`
- evidence: Q/T p90 92 (max 109), rr+ max 10.2%
- look first: link quality as the peer sees it: ack latency / stalls (tools/poll_latency.py, ack_latency.py on a capture).
- owner (ours / peer / environment / harness): _triage_

### F-03 WARN LS-04 — IQ2LB sees IW2OHX-12 (port 1)

- value: `{'qt_p90': 21, 'rr_max': 13.3}`
- evidence: Q/T p90 21 (max 21), rr+ max 13.3%
- look first: link quality as the peer sees it: ack latency / stalls (tools/poll_latency.py, ack_latency.py on a capture).
- owner (ours / peer / environment / harness): _triage_

### F-04 WARN LS-06 — IW2OHX-12>IQ2LB:3

- value: `1`
- evidence: keeper opens 1 -> 2
- look first: KISS keeper re-opened the link: flex_port_links_keep; check RF and SABM collision (ROADMAP).
- owner (ours / peer / environment / harness): _triage_

### F-05 WARN RC-05 — IW2OHX-13

- value: `1`
- evidence: last sample: N2MH
- look first: SSID range mismatch: neighbour entry <link SSID>-<INIT max> (B-3), flex_call_in_range.
- owner (ours / peer / environment / harness): _triage_

### F-06 WARN RC-07 — IW2OHX-13

- value: `1`
- evidence: K1YMI 0-15 123->1603
- look first: count-to-infinity climb: flex_climb_is_loop floor must persist (research/link_stability_2026-09-20/DESTINATION_EXCHANGE_CLIMB.md).
- owner (ours / peer / environment / harness): _triage_

## All checks

| ID | Scope | Status | Value | Detail |
|---|---|---|---|---|
| PF-01 | all | PASS | `{'IW2OHX-13': 'v2.7.0', 'IR2UFV': 'v2.7.0', 'IW2OHX-12': 'v2.7.0'}` | expected v2.7.0; mismatched {} |
| PF-02 | IW2OHX-13 | PASS | `5` | flavour silent: 'FlexNet: ' strings 5 (silent <= 5); sha256 0784583dfea2 |
| PF-02 | IR2UFV | PASS | `99` | flavour flexdebug: 'FlexNet: ' strings 99 (silent <= 5); sha256 37b5d7c4169c |
| PF-03 | IW2OHX-13 | PASS | `DisconnectOnClose=0` |  |
| PF-03 | IW2OHX-13 | PASS | `OnlyVer2point0` |  |
| PF-03 | IR2UFV | PASS | `DisconnectOnClose=0` |  |
| PF-03 | IR2UFV | PASS | `OnlyVer2point0` |  |
| PF-03 | IW2OHX-12 | PASS | `DisconnectOnClose=0` |  |
| PF-03 | IW2OHX-12 | PASS | `OnlyVer2point0` |  |
| PF-04 | gw | INFO · | `2` | pollers active during the campaign (declare them with every number): */15 * * * * /home/iw2ohx/station-dashboard; */10 * * * * /home/iw2ohx/xnet-web-status |
| PF-05 | all | PASS | `{'passed': True, 'waited_s': 917}` | waited 917 s for links >= 900 s up |
| LS-01 | IW2OHX-13>IW2OHX-14:3 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IW2OHX-13>IW2OHX-14:3 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-13>IR2UFV:5 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IW2OHX-13>IR2UFV:5 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IR2UFV>IW2OHX-14:2 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IR2UFV>IW2OHX-14:2 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IR2UFV>IW2OHX-12:2 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IR2UFV>IW2OHX-12:2 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IR2UFV>IW2OHX-13:3 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IR2UFV>IW2OHX-13:3 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-12>IW2OHX-14:2 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IW2OHX-12>IW2OHX-14:2 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-12>IW2OHX-4:2 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IW2OHX-12>IW2OHX-4:2 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-12>IR2UFV:2 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IW2OHX-12>IR2UFV:2 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-12>IQ2LB:3 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IW2OHX-12>IQ2LB:3 | PASS | `1.8` | 1 unexplained in 3.3 h (per 6 h: 1.8); explained: 0; 2026-10-16T10:38:45Z uptime reset |
| LS-01 | IW2OHX-12>IQ2LB:4 | PASS | `0` | 0/11 samples not CONNECTED |
| LS-02 | IW2OHX-12>IQ2LB:4 | PASS | `0.0` | 0 unexplained in 3.3 h (per 6 h: 0.0); explained: 0;  |
| LS-01 | IW2OHX-3>IW2OHX-14:0 | PASS | `0` | 0/11 samples without the link |
| LS-03 | IW2OHX-13 | PASS | `0` | reaped 0 -> 0 |
| LS-03 | IR2UFV | PASS | `0` | reaped 1 -> 1 |
| LS-03 | IW2OHX-12 | WARN | `1` | reaped 0 -> 1 |
| LS-04 | IW2OHX-14 sees IW2OHX-13 (port 2) | PASS | `{'qt_p90': 3, 'rr_max': 0.0}` | Q/T p90 3 (max 3), rr+ max 0.0% |
| LS-04 | IW2OHX-14 sees IR2UFV (port 14) | PASS | `{'qt_p90': 3, 'rr_max': 0.0}` | Q/T p90 3 (max 3), rr+ max 0.0% |
| LS-04 | IW2OHX-14 sees IW2OHX-12 (port 1) | PASS | `{'qt_p90': 3, 'rr_max': 0.0}` | Q/T p90 3 (max 3), rr+ max 0.0% |
| LS-04 | IW2OHX-14 sees IW2OHX-3 (port 6) | PASS | `{'qt_p90': 1, 'rr_max': 0.0}` | Q/T p90 1 (max 1), rr+ max 0.0% |
| LS-04 | IQ2LB sees IW2OHX-12 (port 0) | WARN · | `{'qt_p90': 92, 'rr_max': 10.2}` | Q/T p90 92 (max 109), rr+ max 10.2% |
| LS-04 | IQ2LB sees IW2OHX-12 (port 1) | WARN · | `{'qt_p90': 21, 'rr_max': 13.3}` | Q/T p90 21 (max 21), rr+ max 13.3% |
| LS-05 | IW2OHX-13>IR2UFV#0 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IW2OHX-13>IW2OHX-14#1 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-06 | IW2OHX-13>IR2UFV:5 | PASS | `0` | keeper opens 1 -> 1 |
| LS-05 | IR2UFV>IW2OHX-13#0 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IR2UFV>IW2OHX-14#1 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IR2UFV>IW2OHX-12#2 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-06 | IR2UFV>IW2OHX-12:2 | PASS | `0` | keeper opens 1 -> 1 |
| LS-06 | IR2UFV>IW2OHX-13:3 | PASS | `0` | keeper opens 1 -> 1 |
| LS-05 | IW2OHX-12>IR2UFV#0 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IW2OHX-12>IW2OHX-4#1 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IW2OHX-12>IW2OHX-14#2 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IW2OHX-12>IQ2LB#3 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-05 | IW2OHX-12>IQ2LB#4 | PASS | `1.0` | Queued==0 in 100% of samples, max queued 0 |
| LS-06 | IW2OHX-12>IR2UFV:2 | PASS | `0` | keeper opens 0 -> 0 |
| LS-06 | IW2OHX-12>IQ2LB:3 | WARN | `1` | keeper opens 1 -> 2 |
| LS-06 | IW2OHX-12>IQ2LB:4 | PASS | `0` | keeper opens 1 -> 1 |
| LS-07 | IW2OHX-13 | PASS | `0` | 0 holds >= 250 ms, 0 >= 500 ms, max 0 ms |
| LS-07 | IR2UFV | PASS | `0` | 0 holds >= 250 ms, 0 >= 500 ms, max 0 ms |
| RC-01 | IW2OHX-13 | PASS | `1.0` | median coverage of IW2OHX-14 100.0%, worst sample 100.0% |
| RC-01 | IR2UFV | PASS | `1.0` | median coverage of IW2OHX-14 100.0%, worst sample 100.0% |
| RC-01 | IW2OHX-12 | PASS | `1.0` | median coverage of IW2OHX-14 100.0%, worst sample 100.0% |
| FD-01 | IW2OHX-3 | PASS | `1.0` | median coverage of IW2OHX-14 100.0%, worst sample 100.0% |
| RC-02 | IW2OHX-13 | PASS | `0` | last sample:  |
| RC-02 | IR2UFV | PASS | `0` | last sample:  |
| RC-02 | IW2OHX-12 | PASS | `0` | last sample:  |
| RC-05 | IW2OHX-13 | WARN | `1` | last sample: N2MH |
| RC-05 | IR2UFV | PASS | `0` | last sample:  |
| RC-05 | IW2OHX-12 | PASS | `0` | last sample:  |
| RC-04 | IW2OHX-13 | PASS | `{'median_ratio': 1.33, 'outliers': 0}` | median cost ratio 1.33, outliers 0:  |
| RC-04 | IR2UFV | PASS | `{'median_ratio': 1.33, 'outliers': 0}` | median cost ratio 1.33, outliers 0:  |
| RC-04 | IW2OHX-12 | PASS | `{'median_ratio': 1.36, 'outliers': 0}` | median cost ratio 1.36, outliers 0:  |
| RC-03 | IW2OHX-13 | PASS | `0.0` | median /sum(FL Routes) - D/ / D = 0.0% |
| RC-06 | IW2OHX-13 | PASS | `0.0` | 0 vanish-and-return:  |
| RC-07 | IW2OHX-13 | WARN | `1` | K1YMI 0-15 123->1603 |
| RC-08 | IW2OHX-13 | PASS | `0` |  |
| RC-03 | IR2UFV | PASS | `0.0` | median /sum(FL Routes) - D/ / D = 0.0% |
| RC-06 | IR2UFV | PASS | `0.0` | 0 vanish-and-return:  |
| RC-07 | IR2UFV | PASS | `0` |  |
| RC-08 | IR2UFV | PASS | `0` |  |
| RC-03 | IW2OHX-12 | PASS | `0.0` | median /sum(FL Routes) - D/ / D = 0.0% |
| RC-06 | IW2OHX-12 | PASS | `0.0` | 0 vanish-and-return:  |
| RC-07 | IW2OHX-12 | PASS | `0` |  |
| RC-08 | IW2OHX-12 | PASS | `0` |  |
| RC-09 | IW2OHX-13 | PASS | `2` | IW2OHX-14 cost for IW2OHX-13: min 1 max 3 |
| RC-09 | IR2UFV | PASS | `1` | IW2OHX-14 cost for IR2UFV: min 1 max 2 |
| RC-09 | IW2OHX-12 | PASS | `1` | IW2OHX-14 cost for IW2OHX-12: min 1 max 2 |
| RC-09 | IW2OHX-3 | PASS | `0` | IW2OHX-14 cost for IW2OHX-3: min 1 max 1 |
| RE-KNOWN | IW2OHX-13>IR1UAW-10 | INFO · | `False` | I-1 NET/ROM-first C without FlexNet fallback (ROADMAP): still fails |
| RE-01 | all | PASS | `1.0` | 27/27 first-try; failed:  |
| RE-01 | IQ2LB | INFO · | `1.0` | 1/1 |
| RE-01 | IR2UFV | INFO · | `1.0` | 8/8 |
| RE-01 | IW2OHX-12 | INFO · | `1.0` | 8/8 |
| RE-01 | IW2OHX-13 | INFO · | `1.0` | 6/6 |
| RE-01 | IW2OHX-14 | INFO · | `1.0` | 2/2 |
| RE-01 | IW2OHX-4 | INFO · | `1.0` | 2/2 |
| RE-02 | axudp | PASS | `0.5` | p90 0.5s, max 0.5s over 23 connects |
| RE-02 | rf | PASS | `1.5` | p90 1.5s, max 1.5s over 2 connects |
| RE-02 | kiss_local | PASS | `0.1` | p90 0.1s, max 0.1s over 2 connects |
| RE-03 | all | PASS | `1.0` | TR-LOCALSSID-4 r0: ok=True extended+4 declined+0; TR-12-DE r0: ok=True extended+7 declined+0; TR-12-RF r0: ok=True extended+4 declined+0; TR-UFV-XPORT r0: ok=Tr |
| RE-04 | all | PASS | `1.0` | 15/15 valid;  |

`·` = informational, not part of the verdict.
