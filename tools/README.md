# Investigation tools

Small composable Python helpers used during the development of
linbpq-flexnet. Each one targets a specific question; they are not
a framework, and you should not hesitate to fork or extend them.

All tools take credentials and hostnames on the command line (or
via `xnet.conf` for `xnet_agent.py`). No defaults are baked in.

## Inventory

| Tool | Purpose |
|------|---------|
| `xnet_agent.py` | Telnet+SYS into an (X)Net node. Takes baseline snapshots (L, L*, D, U, P), runs a `monitor +N` capture for a configurable duration, saves everything to a structured JSON log. |
| `xnet_d_send.py` | Telnet+SYS into an (X)Net node, issue one or more `D <call>` queries, print the responses, quit. Used to drive specific queries while a separate `xnet_agent.py` capture runs on a neighbouring port. |
| `bpq_d_query.py` | Connect to a LinBPQ telnet listener (default port 2323), issue `D <call>` queries. Originator-role test of FlexNet path discovery. |
| `d_count_marks.py` | Connect to a LinBPQ telnet listener, run `D *`, count how many destinations carry the `!` Path marker. Used to track path-cache coverage over time after a restart. |
| `analyze_dual_capture.py` | Load two `xnet_agent.py` JSON captures and look for the same QSO-keyed CE type-6/7 frames across both — that's the only positive evidence that a frame actually propagated through an intermediate node. |
| `axudp_teardown.py` | Decode an AXUDP pcap to AX.25 and report, per link, who sent DISC/SABM first. `--link-only` drops digipeated frames, without which transit connects are miscounted as our own link cycling. |
| `flexnet_transaction_audit.py` | Slice an AXUDP capture by `3+` TRANSACTION: the closing `3-`, then how many record frames we emitted before the teardown. The measurement that root-caused the `-12` teardown 2026-09-22 — averaging over all record frames hides the effect, because almost all of them are safe. |
| `flexnet_payload_trace.py` | Timestamped payload trace for one peer — frame type plus the FlexNet body — with inter-frame gaps. Use when the L2 view (`frames.py`) is clean and the answer has to be in what the I-frames carried. |
| `disc_context.py` | Print the frames immediately before each teardown. "Who hung up" narrows the fault to one end; this says whether it was N2 exhaustion, a peer answering DM, or a deliberate disconnect on a healthy link. |
| `frames.py` | Full AX.25 frame trace for one peer with the **command/response C bits** decoded. `axudp_teardown.py` ignores them by design; without them a poll and the answer to a poll look identical, and a retry burst cannot be read. |
| `poll_latency.py` | How fast we answer the peer's polls, and whether a poll burst was followed by a teardown. A peer with an adaptive T1 over AXUDP gives you about 0.6 s before it deletes the session; this measures against that budget. |
| `ack_latency.py` | Ack latency for every inbound I-frame — the node's response time with nothing to interpret — with each slow one annotated from a `proc_sampler.py` CSV: CPU consumed and the kernel wait channel during the stall. |
| `proc_sampler.py` | Samples `/proc/<pid>/{stat,wchan}` plus two file sizes 10x/s. Distinguishes a compute loop (CPU time advances) from a block (it does not, and `wchan` names the wait). Zero perturbation — no ptrace. |
| `linkwatch-start.sh` / `linkwatch-report.sh` | Arm and read out the standing link watch: one capture per peer, `linkstab.py`, and the `/proc` sampler. Edit `TAG` in the start script per experiment. |
| `advert_breakdown.py` | Split the `ADVERT-CHECK` stream by *why* each advertisement fired — first-time (a re-dump after re-init), jitter, withdrawal, restoration. Decides which advertisement fix is worth writing. |
| `linkstab.py` | Long-run (24 h) link-stability watch: polls `FL` every minute for uptimes and per-peer queue depth, tails the `flexdebug` log for true advertisement volume, and connect-probes a rotating sample of `D *` destinations to test whether the table is usable. |
| `linkstab_report.py` | Turn a `linkstab.py` run into decision numbers: session episodes reconstructed as a state machine (an uptime-delta check misses a link that vanishes from `FL` and returns), uptime as a distribution rather than a mean, and advertisement volume summed from per-interval deltas so a counter reset is a gap and not a negative. |
| `sem_watch.py` | Watch BPQ's global `Semaphore` at 200 Hz from `/proc/<pid>/mem` and log every hold longer than a threshold, with the acquiring `File:Line` out of `struct SEM`. Turns "the node went quiet for a second" into a counted, attributable event. Read-only, no ptrace. |
| `sem_stack.py` | Same watch, but the moment a hold passes a trigger it attaches gdb for one batch backtrace. `sem_watch.py` names the *acquirer*; this names the *callee that blocks*. Trigger low — the whole event is ~1 s and gdb needs a few hundred ms. |
| `xnet.conf.example` | Example INI config consumed by `xnet_agent.py --config`. |

## Typical workflow — dual-port forwarding capture

This is the recipe that drove the v1.9 wire-format correction.

You want to know whether a frame originated at node A actually
propagates through node M and reaches node Z. Pick two adjacent
sysop-accessible nodes — one upstream of M and one downstream —
and capture both sides simultaneously.

```bash
# Terminal 1 — capture on the upstream side facing M
python3 xnet_agent.py \
    --host NODE_A.example --port 23 \
    --user $U --password $P --syspass $S \
    --monitor-cmd "monitor +PORT_TO_M" \
    --duration 360 --snapshot-interval 60 \
    --output cap_upstream.json

# Terminal 2 — capture on the downstream side facing M
python3 xnet_agent.py \
    --host NODE_Z.example --port 23 \
    --user $U --password $P --syspass $S \
    --monitor-cmd "monitor +PORT_TO_M" \
    --duration 360 --snapshot-interval 60 \
    --output cap_downstream.json

# Once the captures finish, look for shared QSO keys
python3 analyze_dual_capture.py cap_upstream.json cap_downstream.json
```

A shared QSO key across the two captures is your forwarding
evidence. If you see the same `type-6/7 + QSO + origin` frame on
both sides and the second-side `HOP_BYTE` is exactly one higher,
the intermediate node forwarded the frame.

If the upstream side sees the frame and the downstream side does
not, the intermediate either rejected it (wire-format mismatch) or
answered it locally from cache.

## Typical workflow — D-command verification after a deploy

```bash
# Issue D queries against the linbpq-flexnet node
python3 bpq_d_query.py \
    --host YOUR_BPQ_HOST --port 2323 \
    --user $U --password $P \
    IR5S IR3UGM N2MH-5

# Periodically count how many destinations have a resolved path
python3 d_count_marks.py \
    --host YOUR_BPQ_HOST --port 2323 \
    --user $U --password $P
```

`d_count_marks.py` is meant to be invoked repeatedly (e.g. every
10 min from a cron or a small shell wrapper) to plot the
post-restart cache-fill rate over time.

## quad-watch.py — four-node consistency watch

A routing inconsistency in a distance-vector protocol is a **disagreement
between two tables**. Both ends can look internally consistent while
disagreeing with each other, so no single-sided sampler can find one — that
is why `pcf-watch.sh` (IR2UFV's own counters) never did.

`quad-watch.py` samples all four nodes of the transit triangle within a few
seconds of each other and cross-checks them:

| Node | Access |
|---|---|
| IR2UFV | BPQ telnet `127.0.0.1:2525`, `FL` |
| IW2OHX-14 | xnet telnet `44.134.24.2:23`, `L` |
| IW2OHX-4 | xnet telnet `44.134.24.3:23`, `L` |
| IW2OHX-12 | PC/Flexnet — no direct telnet; chained `C IW2OHX-12` from -14 |

Read-only: it issues only `FL` / `L` / `C` / `B`, and never elevates to SYS,
so it cannot hold a privileged session open for hours. The -12 hop crosses the
live mesh, hence `--pcf-every` (default: one sample in four).

```bash
# on iw2ohx-gw — only it reaches the xnet hosts
export QW_USER_14=... QW_USER_4=... QW_PW_XNET=... QW_SYS_XNET=...
export QW_USER_UFV=... QW_PW_UFV=...
python3 quad-watch.py --interval 900 --pcf-every 4 --out /tmp/quad-watch
```

Outputs `samples.jsonl`, `watch.log` and — read this one first —
`alerts.log`. The checks:

| ID | Fires when |
|---|---|
| A1 | we advertise N to a peer but that peer installed a different count via us |
| A3 | split horizon: we advertise back to the peer we learned from |
| A4 | a peer's uptime went backwards — the session restarted between samples |
| A6 | `contracted` advanced while `extended` did not (reverse frames for chains we never appended) |
| A7 | an advert queue did not move between samples — the drain stalled |
| A8 | a burst of zero-RTT skips: routes arriving unusable |
| A9 | a link is not CONNECTED |
| A10 | we advertise routes but the peer has no row for us at all |
| A11 | the peer's row for us is not flagged `F` (FlexNet) |
| A12 | the peer's cost to us moved by more than 10 |
| I6 | informational: per-sample `extended`/`contracted`/`declined` deltas |

A missing node reads as *absent*, never as zero — a zero would fire false
inconsistencies against the three nodes that did answer.

## Sanitising for sharing

These tools never embed credentials. When pasting JSON captures or
log excerpts elsewhere, double-check that callsign-as-credentials
do not leak (some sysop usernames mirror the operator's callsign).
The frame payloads themselves are amateur-radio traffic and are
intended to be public.
