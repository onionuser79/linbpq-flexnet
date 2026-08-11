#!/usr/bin/env bash
#
# Weekly upstream LinBPQ release watcher for linbpq-flexnet.
#
# Fetches g8bpq/linbpq (the 'upstream' git remote), compares its tip against
# the last-seen commit, and sends a Telegram digest when new commits appear.
# The digest reports how far ahead upstream is of the *rebase baseline* (the
# upstream commit linbpq-flexnet is currently rebased onto) and flags whether
# any of the source files we overlay were touched — i.e. whether a rebase is
# likely to conflict.
#
# Alerting is exactly-once per new upstream tip: after alerting, last_seen is
# advanced so the same commit is not re-announced every week. The baseline is
# NOT advanced automatically — run `check-upstream.sh --ack` after you finish a
# rebase to record the new baseline (and silence the "ahead" count).
#
# Runs on macmini via LaunchAgent net.iw2ohx.linbpq-upstream-watch (weekly).
# Reuses the iw2ohx-monitor Telegram credentials.
#
set -euo pipefail

REPO="/Users/marco.dimartino/ClaudeCode/Xnet_investigation_agent/linbpq-flexnet"
REMOTE="upstream"
BRANCH="master"
STATE_DIR="$HOME/.local/state/linbpq-upstream-watch"
STATE_FILE="$STATE_DIR/last_seen"
BASELINE_FILE="$REPO/tools/upstream-watch/baseline"
ENV_FILE="$HOME/.config/iw2ohx-monitor/telegram.env"
LOG_PREFIX="[upstream-watch]"

mkdir -p "$STATE_DIR"

send_telegram() {
    local text="$1"
    if [ ! -f "$ENV_FILE" ]; then
        echo "$LOG_PREFIX missing $ENV_FILE; cannot send" >&2
        return 0
    fi
    # shellcheck source=/dev/null
    . "$ENV_FILE"
    if [ -z "${TELEGRAM_BOT_TOKEN:-}" ] || [ -z "${TELEGRAM_CHAT_ID:-}" ]; then
        echo "$LOG_PREFIX missing token or chat_id; skipping send" >&2
        return 0
    fi
    curl -sS -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
        --data-urlencode "chat_id=${TELEGRAM_CHAT_ID}" \
        --data-urlencode "text=${text}" \
        --data-urlencode "parse_mode=HTML" \
        --data-urlencode "disable_web_page_preview=true" \
        >/dev/null || echo "$LOG_PREFIX telegram send failed" >&2
}

cd "$REPO"

# --ack: record the current upstream tip as the new rebase baseline. Run this
# right after you finish rebasing linbpq-flexnet onto a new LinBPQ release.
if [ "${1:-}" = "--ack" ]; then
    git fetch --quiet "$REMOTE" "$BRANCH"
    tip="$(git rev-parse "${REMOTE}/${BRANCH}")"
    printf '%s\n' "$tip" > "$BASELINE_FILE"
    printf '%s\n' "$tip" > "$STATE_FILE"
    echo "$LOG_PREFIX baseline + last_seen set to $tip"
    exit 0
fi

git fetch --quiet "$REMOTE" "$BRANCH"
tip="$(git rev-parse "${REMOTE}/${BRANCH}")"

last_seen=""
[ -f "$STATE_FILE" ] && last_seen="$(cat "$STATE_FILE")"

# Baseline: the upstream commit we are rebased onto. Falls back to the
# 6.0.25.30 rebase point (v2.1.35, 2026-06-02) if the file is missing.
baseline="45dc77a4e18c41ce91f844f1ce6ccd0a5fc44fb8"
[ -f "$BASELINE_FILE" ] && baseline="$(cat "$BASELINE_FILE")"

ts="$(date '+%Y-%m-%d %H:%M %Z')"

if [ "$tip" = "$last_seen" ]; then
    echo "$LOG_PREFIX $ts no change; upstream tip $tip"
    exit 0
fi

# New upstream tip since we last looked. Build the digest.
ahead="$(git rev-list --count "${baseline}..${tip}" 2>/dev/null || echo '?')"
short="$(git rev-parse --short "$tip")"
subject="$(git log -1 --format='%s' "$tip")"
cdate="$(git log -1 --format='%ci' "$tip")"

# Which of the files we overlay were touched upstream since the baseline?
# "Overlay" = a repo-tracked .c/.h that also exists in the upstream tree
# (new-only additions like flexnet_l3.c can never conflict, so skip them).
overlay=""
while IFS= read -r f; do
    if git cat-file -e "${REMOTE}/${BRANCH}:${f}" 2>/dev/null; then
        overlay="$overlay $f"
    fi
done < <(git ls-files '*.c' '*.h')

sensitive=""
if [ -n "$overlay" ]; then
    # shellcheck disable=SC2086
    sensitive="$(git diff --name-only "${baseline}..${tip}" -- $overlay 2>/dev/null \
        | awk 'NR>1{printf ", "} {printf "%s", $0} END{if (NR) print ""}')"
fi

text="<b>LinBPQ upstream update</b>
Checked ${ts}

g8bpq/linbpq is now <b>${ahead}</b> commit(s) ahead of the linbpq-flexnet rebase baseline.

Latest: <code>${short}</code> — ${subject}
Date: ${cdate}

<a href=\"https://github.com/g8bpq/linbpq/compare/${baseline}...${tip}\">compare on GitHub</a>"

if [ -n "$sensitive" ]; then
    text="${text}

⚠️ Touches files we overlay (rebase may conflict): ${sensitive}"
else
    text="${text}

No overlaid source files touched — a rebase should be clean."
fi

text="${text}

After rebasing, run on macmini:
<code>tools/upstream-watch/check-upstream.sh --ack</code>"

send_telegram "$text"
printf '%s\n' "$tip" > "$STATE_FILE"
echo "$LOG_PREFIX $ts alerted: tip $tip (${ahead} ahead of baseline; sensitive: ${sensitive:-none})"
