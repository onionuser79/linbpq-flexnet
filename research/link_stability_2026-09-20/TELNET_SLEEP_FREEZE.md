# The 1 s stall is a telnet disconnect freezing the whole node

**Found 2026-09-20 on IR2UFV (v2.2.0 GA, `-4` link disabled).
It is not a FlexNet defect, and it is not the peer.**

`POLL_BUDGET_AND_STALLS.md` left one question open: IW2OHX-14's retry
budget is ~0.6 s, our ack latency is 17.5 ms median, and a rare **~1 s
stall** lands inside that budget and costs the session. *Which code path
costs the second* was unproven. This settles it.

## Answer

**Closing a telnet session makes LinBPQ sleep one second on its main
thread while holding the global `Semaphore`.** For that second nothing
is polled, no AX.25 frame is read, and no I-frame is acked.

```
#5  Sleep (ms=1000)         compatbits.c:178
#6  TelnetPoll (Port=1)     TelnetV6.c:2521      <- if (TCP->DisconnectOnClose)
#7  ExtProc (fn=1, port=1)  TelnetV6.c:1047
#8  EXTRX (...)             cMain.c:473
#9  TIMERINTERRUPT ()       cMain.c:2611
#10 main ()                 LinBPQ.c:1792        <- GetSemaphore(&Semaphore, 2)
```

The main loop takes the semaphore at `LinBPQ.c:1790` *before* calling
`TIMERINTERRUPT()`, so everything reached from there runs under the
lock — including the telnet port's `Sleep(1000)`.

## Evidence

Five independent measurements, all agreeing.

### 1. The lock is held for a flat ~1002 ms, ~once a minute

`tools/sem_watch.py` reads BPQ's `struct SEM` out of `/proc/<pid>/mem`
at 200 Hz (read-only, no ptrace). Over 2.98 h:

| | |
|---|---|
| holds ≥ 60 ms | **214** |
| rate | **71.8 / h** |
| duration | **1000.8 – 1003.0 ms**, every one |
| holder | `LinBPQ.c:1790`, every one |
| gaps that are exactly 60.0 s | **146 / 213** |

A flat 1002 ms is not a workload. It is a `Sleep(1000)`.

### 2. gdb names the callee

`tools/sem_stack.py` watches the same flag and attaches gdb for one
batch backtrace the moment a hold passes 80 ms — a threshold no healthy
hold ever reached in 3 h. **4 catches out of 4** gave the stack above,
`TelnetPoll (Port=1) at TelnetV6.c:2521`, two of them exactly
**60.006 s** apart.

### 3. The freezes are telnet disconnects

`linkstab` polls `FL` over telnet on 127.0.0.1:2525 every 60 s and
closes the session each time. **167 of 214 freezes land within 3 s of an
FL poll** (median offset 1.83 s). The remaining ~47 match its connect
probes, which also open and close a session.

The instrument was causing most of what it measured.

### 4. Every slow ack is inside a freeze

Cross-checking the AXUDP captures against the freeze intervals:

| slow ack | inside a freeze? |
|---|---|
| `-14` 23:51:08.829, 866.6 ms | **yes** — 23:51:08.681 … 23:51:09.684 |
| `-14` 23:51:08.958, 737.9 ms | **yes** — same freeze |
| `-14` 00:10:45.568, 9-poll burst | **yes** — 00:10:44.836 … 00:10:45.834 |

### 5. Controlled A/B — the branch, isolated

Production **IW2OHX-13** was never modified and still has
`DisconnectOnClose=1`, so it serves as the control. `sem_watch` attached
to it read-only, no restart:

| node | `DisconnectOnClose` | telnet closes | 1 s freezes |
|---|---|---|---|
| IW2OHX-13 (control) | `1` | 0 — 25 s idle | **0** |
| IW2OHX-13 (control) | `1` | **2** | **2** (997.3 ms, 1000.6 ms) |
| IR2UFV (treated) | `0` | **10** | **0** |

One freeze per telnet close, none without, none once the directive is
cleared.

## Why this breaks the links

(X)Net adapts T1 to the measured RTT; over AXUDP on a LAN that collapses
to ~62 ms, and with N2 ≈ 9 its **entire patience is ~0.6 s**
([`../link_stability_2026-09-19/POLL_BUDGET_AND_STALLS.md`](../link_stability_2026-09-19/POLL_BUDGET_AND_STALLS.md)).
A 1 s freeze is longer than that, so the peer deletes the session
locally and silently — no DISC — and the first thing we see is a `DM` to
our next frame.

Nothing about this is specific to FlexNet. Any AX.25 peer with a
sub-second retry budget, on any LinBPQ node that also serves telnet,
is exposed.

### It corrects the "PC/Flexnet 60 s tick"

`../link_stability_2026-09-19/README.md` §4 recorded that every `-12`
DISC lands an exact multiple of 60 s after our INIT, and concluded the
timer was PC/Flexnet's and not ours. **The 60 s cadence is ours** — it is
the `FL` poll interval driving the freeze. The conclusion that no timer
*of ours* changed it was correct only because the knob was in the telnet
port, not in FlexNet.

Note the previous run's baseline was measured with `linkstab` running at
`--fl-interval 60`, i.e. with the stressor active throughout.

## The fix

### Applied — config, zero risk

```
PORT
      ID=Telnet
        DisconnectOnClose=0
```

Applied to IR2UFV 2026-09-20 (backup `bpq32.cfg.pre-telnetsleep-2026-09-20`).
Read at init only, so it needs a restart. Verify as with
`OnlyVer2point0`: the parser's "not recognised" list must contain only
the `FLEXNET*` directives.

With `=0` the node answers a closing session with
`Disconnected from Node - Telnet Session kept` and never sleeps. The
client closes its own socket, which is what every monitoring client here
already does.

### Not closed by the config alone

`TelnetV6.c:2513` has a second `Sleep(1000)` under the same lock, for
`sockptr->Signon[0] || sockptr->ClientSession` — set only on **outbound**
telnet connects (`TelnetV6.c:2830-2847`, `:5815`). IR2UFV makes none
(`CMS` commented out, `FALLBACKTORELAY=0`), which is why all four
catches were 2521. A node doing telnet forwarding or CMS fallback would
still freeze on each outbound session close.

The general fix is to drop the lock around the sleep, the pattern
`LinBPQ.c` already uses around `InitializeTNCEmulator`:

```c
FreeSemaphore(&Semaphore);
Sleep(1000);
GetSemaphore(&Semaphore, 2);
DataSocket_Disconnect(TNC, sockptr);
```

**Cost:** `TelnetV6.c` is upstream and not currently in the overlay, so
this widens the rebase conflict surface from five files to six. Worth
sending to G8BPQ rather than carrying locally.

### Tooling

`linkstab` should hold one persistent telnet session instead of
reconnecting every 60 s. The same applies to the station-dashboard and
`xnet-status` crons, which telnet in on a timer.

## Production IW2OHX-13 — measured, then fixed

`-13` had the same `DisconnectOnClose=1` (`/home/bpq/bpq32.cfg` line
280) and is polled over telnet `127.0.0.1:2323` by the station-dashboard
cron every 15 min (`station-dashboard/collector.py` `STATION_NODES`
includes `IW2OHX-13`, `IR2UFV` and `IW2OHX-15`), plus `pr-digi-gw` on
demand.

Measured on the live node before touching it: **one 1004.0 ms freeze at
08:30:35Z**, exactly when the cron reached `-13`, same call site — and a
second at 08:31:23Z from a manual session close. Steady-state exposure
is therefore ~4 freezes/h from the cron alone, each one long enough to
drop an AX.25 peer with a sub-second retry budget.

**Fixed 2026-09-20**: `DisconnectOnClose=0`, restarted
(backup `bpq32.cfg.pre-telnetsleep-2026-09-20`). Verified on the new
process: 4 telnet closes → **0 freezes**. The `-14` link came back and
rebuilt its table.

The station's own monitoring had been periodically deafening the node it
was monitoring.

### IW2OHX-15 — has the defect, but is not currently hurt by it

Checked 2026-09-20. `DisconnectOnClose=1` at line 260 of
`C:\Users\iw2ohx\AppData\Roaming\BPQ32\BPQ32.cfg` (telnet 2324), and
it is in the dashboard's 15-minute poll list. The defect applies:
`TelnetV6.c` is shared source, and **Windows BPQ32 has the identical
main-loop shape** — `Bpq32.c:3261` takes `GetSemaphore(&Semaphore, 2)`
immediately before `if (TIMERINTERRUPT())`, exactly as `LinBPQ.c:1790`
does.

It also carries an AXUDP link to the very peer with the 0.6 s budget
(`MAP IW2OHX-14 44.134.24.4 UDP 10091`, cfg line 363).

**But that link is stable.** From `-14`'s own `L *`:
`5:IW2OHX-15 … 4h 39m … 78 bit/s`. The reason is the same one already
recorded for `-13` in
[`../link_stability_2026-09-19/POLL_BUDGET_AND_STALLS.md`](../link_stability_2026-09-19/POLL_BUDGET_AND_STALLS.md):
a quiet leaf is rarely mid-transaction, so a 1 s freeze seldom lands
inside a poll burst. IR2UFV was the opposite — permanently exchanging
203 FlexNet routes with `-14`.

**Decision (operator, 2026-09-20): leave it until the next scheduled
restart.** The Windows station also runs BPQMail, the Winlink RMS and
the HF gateway, so a restart costs far more than on a digipeater, and
there is no measured link damage to justify one. This is settled — do
not re-open it or apply it unprompted.

When that restart happens, the whole job is:

1. `DisconnectOnClose=1` → `0` at line 260 of
   `C:\Users\iw2ohx\AppData\Roaming\BPQ32\BPQ32.cfg` (back the file
   up first; it is the live cfg, *not* the `_050001` copy).
2. Restart BPQ32; the directive is read at init only.
3. Verify: open and close a few telnet sessions on 2324 and confirm the
   node does not go deaf for a second each time. There is no
   `/proc/<pid>/mem` on Windows, so `sem_watch.py` cannot be used —
   check instead that `-14`'s `L *` uptime for `IW2OHX-15` keeps
   climbing across a dashboard cron run.

> Note the same caveat applies in reverse to `-13`: what is demonstrated
> there is that the *freeze* is gone, not that its link uptime improved
> — its `-14` link had already been up 5h 28m before the change.

## Method notes

- **`sem_watch.py` is the instrument that cracked this.** `/proc/pid/mem`
  + `struct SEM`'s `File`/`Line` turns "the node was asleep" into a call
  site, read-only and without stopping the process.
- `wchan=hrtimer_nanosleep` with `cpu +0 ms` says *sleeping*, not
  computing — that is what ruled out the compute-loop and
  `flex_path_cache_save()` candidates before gdb confirmed it.
- Trigger the gdb catch **low** (80 ms). The whole event is ~1 s and gdb
  needs a few hundred ms to attach.
- `pkill -f sem_stack.py` typed inline over ssh matches the ssh command
  line and kills the session (exit 255). Kill by pid — the same trap
  already recorded for `pkill -f /home/bpq-ufv/linbpq`.
- `linkstab.py` needs `LS_USER_UFV` / `LS_PW_UFV` in the environment or
  it exits immediately with a one-line message.
- Redirecting IR2UFV's console to the existing `/tmp/ir2ufv.console`
  failed with `Permission denied` even under sudo; a fresh path works.
- **`grep -c` exits 1 on a zero count**, so `grep -c X f | grep -q '^0$'`
  fails the whole pipeline under `pipefail` — it aborted the production
  script *after* the edit had already succeeded. Write the guard as
  `n=$(grep -c ... || true); [ "$n" = "0" ]`. Same trap as the v2.1.42
  release; it has now cost time twice.
- **Make a backup step conditional.** The aborted run would have
  re-taken the backup on retry and captured the already-edited file.
