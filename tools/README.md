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

## Sanitising for sharing

These tools never embed credentials. When pasting JSON captures or
log excerpts elsewhere, double-check that callsign-as-credentials
do not leak (some sysop usernames mirror the operator's callsign).
The frame payloads themselves are amateur-radio traffic and are
intended to be public.
