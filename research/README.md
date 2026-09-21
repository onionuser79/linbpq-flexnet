# Research index

Wire-level investigations behind the implementation. Each entry says
what it settled, so you can tell from here whether you need to open it.

**Hard rule for everything in this directory: capture first, then code.**
Never guess a wire format — derive it from a capture of real peers and
re-verify with a fresh capture after deploying.

## Current

| Investigation | Settled |
|---|---|
| [`link_stability_2026-09-20/`](link_stability_2026-09-20/) | **Start here.** Two causes, found the same day. `TELNET_SLEEP_FREEZE.md`: closing a telnet session makes LinBPQ `Sleep(1000)` on the main thread holding the global semaphore (`TelnetV6.c:2521` under `DisconnectOnClose`), freezing all AX.25 for a second — fixed with `DisconnectOnClose=0`, and it also corrects the "PC/Flexnet 60 s tick" (that cadence was our own `FL` poll). `DESTINATION_EXCHANGE_CLIMB.md`: with that fixed the `-12` link still recycled every 70-87 min — 43 of 204 destinations were climbing geometrically (count-to-infinity), 35.7 % of everything we advertised, which a *relative* 10 % jitter threshold cannot stop. Also records the teardown **pairing** (18 teardowns = 8 real), that PC/Flexnet initiates 100 % of them, and two plausible hypotheses the data killed. **`V2_2_1_OUTCOME.md` is the verdict — read it before re-opening this:** the `3+` defects are fixed and verified, and the `-12` teardown is **not** fixed (23 of 23 inbound `3+` still end the session, 0.83/h before against 1.12/h after). It also carries the rc1/rc2/rc3/v2.2.1 cutover timestamps for slicing the captures, and names the one thing never yet tested: what PC/Flexnet does with a *correct but large* answer. |
| [`link_stability_2026-09-19/`](link_stability_2026-09-19/) | Two defects found and fixed in v2.2.0 — one route record per I-frame, and unsolicited re-INIT on a healthy link. Also documents what is *not* fixed: PC/Flexnet's own 60 s teardown tick, and IW2OHX-4's independent flapping. Its follow-up `POLL_BUDGET_AND_STALLS.md` covers the post-`-4`-removal watch: (X)Net's 0.6 s retry budget vs our rare ~1 s stall. |
| [`link_stability_2026-09-18/`](link_stability_2026-09-18/) | Who tears down each link, from the wire. Split the problem into two unrelated faults and proved the `-4` `RETRIES` fix. Superseded in conclusions by the 09-19 work, still the reference for the teardown census method. |
| [`l2_forwarding_2026-09-17/`](l2_forwarding_2026-09-17/) | FlexNet multi-hop transit = symmetric digi-chain rewriting. Implemented as `FLEXNETL2TRANSIT`. |
| [`path_query_2026-09-18/`](path_query_2026-09-18/) | CE type-6 is a *traversal*, not a query: non-adjacent nodes insert their next hop and forward. Implemented as `FLEXNETPATHFORWARD`. |
| [`transit_v2/`](transit_v2/) | Transit-role behaviour study. Multi-hop transit rides NetROM L4 CREQ over PID=CF; 1-hop is AX.25 V2 with 2 digis. |

## Superseded — kept for the evidence, not the conclusions

| Investigation | Note |
|---|---|
| [`fix_finder_2026-09-18/`](fix_finder_2026-09-18/) | Named the relative 10 % jitter threshold as "the one defect left". **Wrong** — see `link_stability_2026-09-19/`. The drain was 19× too small; the jitter floor is minor by comparison. |
| [`OPEN_NEXT_link_instability.md`](OPEN_NEXT_link_instability.md) | The original uptime table and PC/Flexnet cost rings that opened the investigation. |
| [`ir2ufv-pcf12-2026-05-25/`](ir2ufv-pcf12-2026-05-25/) | Early PC/Flexnet link study. |
| [`status_10_framing_investigation_2026-06-02.md`](status_10_framing_investigation_2026-06-02.md) | `STATUS_10` pong does not reduce PCF's cost. Don't re-try it. |
| [`ir2ufv-pcf-v2.1.35-capture-analysis-2026-06-02.md`](ir2ufv-pcf-v2.1.35-capture-analysis-2026-06-02.md) | v2.1.35-era capture analysis. |

## Tools

In [`../tools/`](../tools/). The ones that earned their keep:

| Tool | Use |
|---|---|
| `axudp_teardown.py` | Who tore a link down, from an AXUDP pcap. **Always pass `--local-ip` and `--link-only`.** |
| `disc_context.py` | The frames immediately before each teardown — distinguishes N2 exhaustion from a peer that forgot the session. |
| `linkstab.py` | 24 h watch: `FL` sampling, advertisement volume, connect probes. Needs `LS_USER_UFV` / `LS_PW_UFV`. |
| `linkstab_report.py` | Episodes, distributions and volume from a `linkstab` run. |
| `advert_breakdown.py` | Why each advertisement fired. |
| `frames.py` | Frame trace with the **command/response bits** decoded — the only way to read a poll burst. |
| `poll_latency.py` | Our reply latency against the peer's retry budget, and which bursts ended in a teardown. |
| `ack_latency.py` + `proc_sampler.py` | Ack latency per inbound I-frame, annotated with what the process was doing (CPU vs `wchan`). |
| `sem_watch.py` | **Counts the node's global-semaphore holds and names the acquiring call site**, read-only from `/proc/<pid>/mem`. The before/after number for any whole-node stall. |
| `sem_stack.py` | Catches a long hold in the act and attaches gdb for one backtrace — turns "the lock was held" into the exact blocking callee. |
| `linkwatch-start.sh` / `linkwatch-report.sh` | Arm and read out the standing watch (captures + linkstab + sampler). |
| `../tools/unit/` | C unit tests, with the functions extracted verbatim from the source so they cannot drift. |
