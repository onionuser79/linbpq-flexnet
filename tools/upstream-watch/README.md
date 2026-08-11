# upstream-watch — weekly LinBPQ release watcher

Watches the `g8bpq/linbpq` upstream and tells you (via Telegram) when John
Wiseman ships new commits, so you know when to consider rebasing
`linbpq-flexnet` onto a fresh LinBPQ release.

## What it does

Once a week the job:

1. `git fetch upstream master` in this repo (`upstream` = `g8bpq/linbpq`).
2. Compares the upstream tip against the **last-seen** commit it recorded.
3. If new, sends a Telegram digest containing:
   - how many commits upstream is **ahead of the rebase baseline**,
   - the latest commit (short hash + subject + date),
   - a GitHub **compare** link (`baseline...tip`),
   - whether any file we *overlay* was touched — i.e. whether a rebase is
     likely to **conflict** (`Cmd.c`, `L2Code.c`, `asmstrucs.h`, `bpqaxip.c`
     are the overlaid files; `flexnet_l3.*` / `FlexNetCode.c` are new-only and
     never conflict).
4. Advances **last-seen** so the same commit is not re-announced next week.

Alerting is **exactly-once per new upstream tip**. The rebase **baseline** is
never advanced automatically.

## Two pieces of state

| What | Where | Advanced |
|------|-------|----------|
| `last_seen` — last upstream tip observed | `~/.local/state/linbpq-upstream-watch/last_seen` (macmini-local) | automatically, each alert |
| `baseline` — upstream commit we are rebased onto | `tools/upstream-watch/baseline` (committed) | **manually**, via `--ack` |

The baseline starts at `45dc77a` (LinBPQ 6.0.25.30, the v2.1.35 rebase point,
2026-06-02).

## After you rebase

When you finish rebasing `linbpq-flexnet` onto a new LinBPQ release, record the
new baseline (this also silences the "ahead" count and resets last-seen):

```bash
tools/upstream-watch/check-upstream.sh --ack
```

Then commit the updated `baseline` file.

## Manual run / test

```bash
tools/upstream-watch/check-upstream.sh      # normal check (sends Telegram if new)
```

To force a re-announcement of the current tip, clear last-seen first:

```bash
rm -f ~/.local/state/linbpq-upstream-watch/last_seen
tools/upstream-watch/check-upstream.sh
```

## Scheduling (macmini)

LaunchAgent `net.iw2ohx.linbpq-upstream-watch` — Monday 08:30 local.

```bash
launchctl unload ~/Library/LaunchAgents/net.iw2ohx.linbpq-upstream-watch.plist 2>/dev/null
launchctl load   ~/Library/LaunchAgents/net.iw2ohx.linbpq-upstream-watch.plist
launchctl start  net.iw2ohx.linbpq-upstream-watch   # run now
```

Log: `tools/upstream-watch/check-upstream.log`.
Telegram credentials are reused from `~/.config/iw2ohx-monitor/telegram.env`.
