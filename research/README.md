# Research index

Wire-level investigations behind the implementation. Each entry says
what it settled, so you can tell from here whether you need to open it.

**Hard rule for everything in this directory: capture first, then code.**
Never guess a wire format — derive it from a capture of real peers and
re-verify with a fresh capture after deploying.

## Current

| Investigation | Settled |
|---|---|
| [`link_stability_2026-09-19/`](link_stability_2026-09-19/) | **Start here.** Two defects found and fixed in v2.2.0 — one route record per I-frame, and unsolicited re-INIT on a healthy link. Also documents what is *not* fixed: PC/Flexnet's own 60 s teardown tick, and IW2OHX-4's independent flapping. |
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
| `../tools/unit/` | C unit tests, with the functions extracted verbatim from the source so they cannot drift. |
