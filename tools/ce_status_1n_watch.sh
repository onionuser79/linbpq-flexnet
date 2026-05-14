#!/usr/bin/env bash
#
# ce_status_1n_watch.sh
#
# Daily watcher for CE-STATUS-1n entries in the linbpq debug log on
# iw2ohx-gw. Alerts via Telegram when a previously-unseen digit appears,
# so we can decide whether Option B (per-digit handlers) is needed.
#
# - Source of truth on remote: /tmp/flexnet_axudp.log on iw2ohx-gw
#   (requires linbpq running a flexdebug build).
# - State on macmini: ~/.config/linbpq-flexnet-watch/seen_digits.txt
#   one digit per line; seeded with "2" (the only digit observed
#   before this watch started).
# - Telegram creds: ~/.config/iw2ohx-monitor/telegram.env (sourced).
# - Auto-disable after WATCH_UNTIL — last run sends a final summary
#   and the cron entry can be removed by hand.
#
# Run by macmini cron once per day. Idempotent.

set -euo pipefail

WATCH_DIR="${HOME}/.config/linbpq-flexnet-watch"
SEEN_FILE="${WATCH_DIR}/seen_digits.txt"
LOG_FILE="${WATCH_DIR}/watch.log"
TG_ENV="${HOME}/.config/iw2ohx-monitor/telegram.env"
REMOTE_LOG="/tmp/flexnet_axudp.log"
SSH_HOST="iw2ohx-gw"

WATCH_START="2026-05-14"
WATCH_UNTIL="2026-05-24"

mkdir -p "${WATCH_DIR}"
touch "${SEEN_FILE}" "${LOG_FILE}"

# shellcheck disable=SC1090
. "${TG_ENV}"

send_telegram() {
    local text="$1"
    curl -sS -o /dev/null \
        --max-time 15 \
        --data-urlencode "chat_id=${TELEGRAM_CHAT_ID}" \
        --data-urlencode "text=${text}" \
        --data-urlencode "parse_mode=Markdown" \
        "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
        || true
}

log() {
    printf '%s  %s\n' "$(date -u +%FT%TZ)" "$*" >> "${LOG_FILE}"
}

today="$(date +%F)"
log "run: today=${today} watch_until=${WATCH_UNTIL}"

# Pull all digits seen in the remote log (sorted unique).
remote_digits="$(
    ssh -o BatchMode=yes -o ConnectTimeout=10 "${SSH_HOST}" \
        "grep -oE 'CE-STATUS-1n:.*digit=[0-9]' '${REMOTE_LOG}' 2>/dev/null \
            | grep -oE 'digit=[0-9]' | awk -F= '{print \$2}' | sort -u" \
        2>/dev/null || true
)"

# Aggregate the count (single SSH round-trip). grep -c always prints
# the count even when 0 matches, so we just suppress its exit-1.
remote_count="$(
    ssh -o BatchMode=yes -o ConnectTimeout=10 "${SSH_HOST}" \
        "grep -c CE-STATUS-1n '${REMOTE_LOG}' 2>/dev/null; true" \
        2>/dev/null | head -n 1
)"
remote_count="${remote_count:-0}"

log "remote_count=${remote_count} digits_seen=${remote_digits//$'\n'/,}"

# Determine novel digits = remote_digits \ seen_digits.
novel=""
while IFS= read -r d; do
    [ -z "${d}" ] && continue
    if ! grep -Fxq "${d}" "${SEEN_FILE}"; then
        novel="${novel}${d} "
        echo "${d}" >> "${SEEN_FILE}"
    fi
done <<< "${remote_digits}"

novel="${novel% }"

if [ -n "${novel}" ]; then
    log "NOVEL digits: ${novel}"
    send_telegram "*linbpq-flexnet CE-STATUS-1n watch*
Novel digit(s) observed: \`${novel}\`
Total CE-STATUS-1n entries on iw2ohx-gw: ${remote_count}
All digits seen so far: \`$(tr '\n' ' ' < "${SEEN_FILE}" | sed 's/ $//')\`
Marco — consider whether Option B (per-digit handler) is now justified."
fi

# Kickoff confirmation on first ever run.
if [ ! -f "${WATCH_DIR}/.kickoff_sent" ]; then
    send_telegram "*linbpq-flexnet CE-STATUS-1n watch — started*
Window: ${WATCH_START} → ${WATCH_UNTIL} (10 days)
Source: iw2ohx-gw:${REMOTE_LOG} (flexdebug build)
Seeded digits: \`$(tr '\n' ' ' < "${SEEN_FILE}" | sed 's/ $//')\`
Telegram alerts will fire only when a previously-unseen digit appears."
    touch "${WATCH_DIR}/.kickoff_sent"
    log "kickoff telegram sent"
fi

# End-of-window summary + self-disable hint.
if [ "${today}" \> "${WATCH_UNTIL}" ] || [ "${today}" = "${WATCH_UNTIL}" ]; then
    if [ ! -f "${WATCH_DIR}/.final_sent" ]; then
        send_telegram "*linbpq-flexnet CE-STATUS-1n watch — final summary*
Window closed (${WATCH_START} → ${WATCH_UNTIL}).
Total CE-STATUS-1n entries observed: ${remote_count}
All digits seen: \`$(tr '\n' ' ' < "${SEEN_FILE}" | sed 's/ $//')\`
Action items for Marco:
- Switch iw2ohx-gw back to production build with \`./sync-and-build.sh all\` (optional).
- Remove the cron entry for this script.
- If only digit=2 was seen, Option A (generic classifier) is final. Otherwise consider Option B."
        touch "${WATCH_DIR}/.final_sent"
        log "final summary telegram sent"
    fi
fi
