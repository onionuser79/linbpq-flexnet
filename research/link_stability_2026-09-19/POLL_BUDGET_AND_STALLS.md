# Why IW2OHX-14 still drops the link — the peer's poll budget

**Run: IR2UFV on v2.2.0 GA, with the IW2OHX-4 link removed by the
operator on 2026-09-19. Watch armed 19:59 local / 18:59Z, 24 h.**

Removing `-4` did **not** make `-14` stable: the first teardown came
2 minutes into the watch, with the same signature as every teardown in
the 22 h pre-GA capture. This file records the signature and what is
still unproven.

## The signature, end to end

```
19:01:33.182 In  CMD     I N(S)=1 N(R)=1  PID=CE  "3WA2UPK241921 ."
                 ... IR2UFV says nothing for 981 ms ...
19:01:33.316 In  CMD P/F I N(S)=1         retransmit
19:01:33.377 In  CMD P/F I N(S)=1         +62 ms
19:01:33.439 In  CMD P/F I N(S)=1         +62 ms
19:01:33.501 In  CMD P/F I N(S)=1         +62 ms
19:01:33.563 In  CMD P/F RR    N(R)=1     +62 ms
19:01:33.625 In  CMD P/F RR    N(R)=1     +62 ms
19:01:33.687 In  CMD P/F RR    N(R)=1     +62 ms
19:01:33.749 In  CMD P/F RR    N(R)=1     +62 ms
19:01:33.811 In  CMD P/F RR    N(R)=1     +62 ms   <- 9th and last try
19:01:34.163 Out ... our answers, all at once
19:01:34.163 In  RSP P/F DM               "I have no such session"
19:01:40.253 Out CMD P/F SABM  -> UA      link back up, uptime reset
```

**IW2OHX-14's entire patience is about 0.6 s**: T1 ≈ 62 ms, N2 ≈ 10.
That is not a misconfiguration — (X)Net adapts T1 to the measured link,
and over AXUDP on a LAN the measurement is a couple of milliseconds. It
polls nine times in 540 ms and then deletes the session locally. No
DISC is sent, so the first thing we see is a `DM` to our next frame.

**IR2UFV normally answers in 17.5 ms** (median over 78 inbound I-frames;
p90 22.3 ms). The teardowns are caused by a rare outlier — one stall of
roughly a second — landing inside that 0.6 s budget.

### It is the outlier, not the average

22 h capture, pre-GA (`/tmp/chk-14.pcap`), same peer:

| inbound poll bursts (≥3 polls in 2 s) | outcome |
|---|---|
| 6 bursts that reached **9 polls** | **DM/DISC within 60 s** |
| 5 bursts of 4–8 polls | survived |

Reply latency over that run: median 334 ms, p90 611 ms, max 845 ms —
because a burst's later polls are all answered in the same late batch,
which inflates the distribution. The useful number is the one above:
17.5 ms normally, ~1 s when it goes wrong.

### The stall is triggered by a route-table change, not by load

The frame we failed to answer was a **single CE record** (PID=0xCE,
15 bytes, one destination: `WA2UPK`). Bulk CE batches of 259 bytes on
the same link are answered in 20 ms. gw was idle throughout — load
0.13, both linbpq processes under 1 % CPU.

Candidates for the ~1 s, in order of plausibility:

1. **The work a *table-changing* record triggers** —
   `flex_learned_add` → `flex_advertise_check` → `flex_advertise_drain`
   per peer, and the walks around `FlexNetCode.c:5477`. Only records
   that change the table take this path, which matches "rare".
2. **`flex_path_cache_save()`** (`FlexNetCode.c:4133`) — 187 entries,
   15 KB, `fopen`/`fprintf`×n/`fflush`/`fclose`/`rename`, run inside
   `FlexNet_Timer` on the BPQ thread. Rate-limited to one write per
   300 s by `g_path_cache_dirty`, so it cannot explain a frequent
   stall, but it can explain an occasional one on an SD card.
3. Console output — **ruled out as the main cause**: the debug build
   writes only 53 bytes/s to `nohup.out` on average.

**Running now to settle it:** `tools/proc_sampler.py` samples
`/proc/<pid>/{stat,wchan}` 10×/s. If `utime+stime` advances by ~1 s
across the next stall it is a compute loop; if it does not, `wchan`
names what the node was blocked on. `tools/ack_latency.py` prints that
correlation next to each slow ack.

## Control: production IW2OHX-13 never gets polled

Same Pi, same peer, silent build, `udp port 10093`: **zero** inbound
polls and zero poll bursts in the control window, ack latency median
17.5 ms. `-13` is a leaf and exchanges far less with `-14`, so this is
suggestive rather than conclusive — but it does say the stall is not a
property of the host.

## IW2OHX-12 is unchanged and is not ours

Both `-12` teardowns in the watch so far were peer-initiated with **no
slow ack from us** (ack latency max 24 ms, zero polls unanswered).
This is PC/Flexnet's own 60 s teardown tick, already settled in
[`README.md`](README.md) §3. Nothing here changes that.

## Two side findings

### We send XID to peers that cannot parse it — one extra reset per restart

At every start-up BPQ sets up its crosslinks with an **XID** (AX.25 2.2
parameter negotiation, `L2Code.c:2069` → `L2SENDXID`). (X)Net answers
`FRMR` with the "invalid control field" bit and then `DISC`; we fall
back to SABM and the link comes up. Cost: one guaranteed teardown per
peer per restart, plus a FlexNet re-INIT and a cost-ring reseed.

Observed at every restart in the captures: 2026-09-18 09:06:22Z,
10:15:40Z, 13:52:52Z; 2026-09-19 07:19:23Z; 2026-09-19 09:55:08Z.

The fallback flag `ROUTE->noV2point2` is per-ROUTE and in RAM, so it is
relearned the hard way after each restart.

**Fix:** `OnlyVer2point0=1` in `bpq32.cfg` (`config.c:316`,
`cMain.c:925` → `SUPPORT2point2 = 0`). Neither (X)Net nor PC/Flexnet
does 2.2, so nothing is lost on this node.

### The IW2OHX-4 removal is not complete

`bpq32.cfg` still carries `MAP IW2OHX-4 192.168.1.203 UDP 10075 B F`
(line 319) and the locked route `IW2OHX-4,161,2,0,0,0,0` (line 328).
The L2 session is gone — the capture shows **only outbound NODES UI
broadcasts** to 192.168.1.203, no session frames — but `-4` can
re-establish at any moment and contaminate the experiment. Remove both
lines if the removal is meant to hold.

## How to read the watch

```bash
ssh iw2ohx-gw 'bash /tmp/linkwatch-report.sh'     # tools/linkwatch-report.sh
```

Collectors (all under `/tmp` on gw, tag `no4`):

| what | where |
|---|---|
| captures, one per peer | `/tmp/no4-mon-{14,12,4}/link.pcap*` |
| `FL` poll + connect probes | `/tmp/no4-linkstab/` |
| `/proc` sampler | `/tmp/no4-linkstab/proc-sample.csv` |
| control, production -13 | `/tmp/ctl-13/link.pcap*` |

## Measurement notes

- **Decode the C bits.** `axudp_teardown.py` ignores them by design, and
  without them a poll and the answer to a poll are the same frame.
  `tools/frames.py` prints CMD/RSP and is what made the burst readable.
- **`linkstab`'s connect probes perturb what they measure.** The
  19:01:33 teardown happened 6 s into a `C IW2OHX-14` probe. The link
  also reset at ~18:17 local with no linkstab running, so the
  instability is real — but do not quote a teardown rate without
  saying whether probes were running.
- Do not read a mid-session XID off a capture that spans a restart:
  the frames before it belong to the previous process.
