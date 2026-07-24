#!/usr/bin/env bash
# Claude Code hook -> ESP32-C3 status display (USB serial), aggregating across
# concurrent sessions.
#
# Usage (from settings.json):  claude-status.sh <event>
#   event = work | perm | done | idle | start | end | notify
#
# The event payload (with .session_id, and for notifications .message) arrives
# as JSON on STDIN. "notify" is a catch-all for the Notification hook: we read
# the message and map it to idle (Claude is just waiting) or perm (something that
# actually needs your attention).
#
# Each session's current state is stored in its own file; on every event we pick
# the HIGHEST-PRIORITY state across all live sessions and count how many sessions
# share it, then send "state|count\n" to the device. So a CONFIRM in one session
# is never clobbered by WORK/DONE in another, and the display shows how many
# sessions are in the winning state.
#
# Priority: perm > work > done > start > idle. Stale sessions (no update within
# TTL) are pruned; SessionEnd removes a session immediately.
#
# Design rule: never block or fail the Claude session — every path exits 0.

EVENT="${1:-idle}"
PORT="${CLAUDE_STATUS_PORT:-/dev/claude-status}"
# Seconds until a silent session is treated as dead and pruned. Must exceed your
# longest single tool call: a session's file mtime only advances on a hook event,
# so a >TTL tool (big build, long test run) would otherwise be pruned mid-work.
TTL="${CLAUDE_STATUS_TTL:-3600}"

# per-user runtime dir (tmpfs, cleared on logout); fall back to /tmp
BASE="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
DIR="${CLAUDE_STATUS_DIR:-$BASE/claude-status}"
mkdir -p "$DIR" 2>/dev/null || { DIR="/tmp/claude-status-$(id -u)"; mkdir -p "$DIR" 2>/dev/null; }

payload="$(cat 2>/dev/null || true)"

# pull a field out of the JSON payload (empty if jq/field missing)
json() { [ -n "$payload" ] && command -v jq >/dev/null 2>&1 && jq -r "$1 // empty" <<<"$payload" 2>/dev/null || true; }

# Extract each field we need exactly once. .message is only needed for notify
# events (or when debugging), so we skip that jq call otherwise.
sid_raw="$(json '.session_id')"
msg_raw=""
if [ "$EVENT" = "notify" ] || [ -n "${CLAUDE_STATUS_DEBUG:-}" ]; then
  msg_raw="$(json '.message')"
fi

# Opt-in debug log (export CLAUDE_STATUS_DEBUG=1). Off by default so it never
# grows unbounded on a shipped install. Lives OUTSIDE the session-state set below.
if [ -n "${CLAUDE_STATUS_DEBUG:-}" ]; then
  printf '%s event=%-7s sid=%s msg=%q\n' "$(date '+%H:%M:%S')" "$EVENT" \
    "$(printf '%s' "$sid_raw" | cut -c1-8)" "$msg_raw" >> "$DIR/events.log" 2>/dev/null || true
fi

# Resolve the Notification catch-all from its message. Permission prompts are
# handled immediately by the PermissionRequest hook, so we IGNORE the (delayed,
# ~5-7s) permission Notification here — otherwise it would re-raise CONFIRM after
# you already answered. "waiting for input" -> idle; an EMPTY/absent message is
# non-actionable -> idle (this also avoids a spurious CONFIRM when jq is missing);
# any other message -> perm (best-effort attention).
if [ "$EVENT" = "notify" ]; then
  msg="$(printf '%s' "$msg_raw" | tr 'A-Z' 'a-z')"
  case "$msg" in
    "")                                                      EVENT="idle" ;;
    *"waiting for your input"*|*"waiting for input"*|*idle*) EVENT="idle" ;;
    *permission*)                                            exit 0 ;;
    *)                                                       EVENT="perm" ;;
  esac
fi

# per-session filename (safe chars only). jq-free sed fallback so a missing jq
# does not collapse every session into one shared "default" file.
sid="$(printf '%s' "$sid_raw" | tr -cd 'A-Za-z0-9._-')"
if [ -z "$sid" ] && [ -n "$payload" ]; then
  sid="$(printf '%s' "$payload" | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1 | tr -cd 'A-Za-z0-9._-')"
fi
[ -n "$sid" ] || sid="default"

# record (or clear, on end) this session's state
if [ "$EVENT" = "end" ]; then
  rm -f "$DIR/$sid" 2>/dev/null
else
  printf '%s' "$EVENT" > "$DIR/$sid" 2>/dev/null
fi

# priority of a state as a bare number (higher wins); 0 = unknown/ignored
prio() {
  case "$1" in
    perm)  echo 5 ;;
    work)  echo 4 ;;
    done)  echo 3 ;;
    start) echo 2 ;;
    idle)  echo 1 ;;
    *)     echo 0 ;;
  esac
}

# Aggregate + write under a lock so concurrent hooks don't interleave. If we
# can't get the lock quickly, skip this round rather than proceed unlocked (our
# own state is already recorded above; the next event will reflect it).
exec 9>"$DIR/.lock" 2>/dev/null
if flock -w 2 9 2>/dev/null; then
  now="$(date +%s)"
  best="idle"; bestp=0; count=0
  for f in "$DIR"/*; do
    [ -f "$f" ] || continue
    case "${f##*/}" in events.log|.lock) continue ;; esac  # not session-state files
    m="$(stat -c %Y "$f" 2>/dev/null)"
    [ -n "$m" ] || continue                                # stat failed: keep, don't prune
    if [ "$((now - m))" -gt "$TTL" ]; then rm -f "$f" 2>/dev/null; continue; fi
    s="$(cat "$f" 2>/dev/null)"
    [ -n "$s" ] || continue                                # empty (e.g. mid-write): ignore
    p="$(prio "$s")"
    if [ "$p" -gt "$bestp" ]; then
      bestp="$p"; best="$s"; count=1
    elif [ "$p" -eq "$bestp" ] && [ "$p" -ne 0 ]; then     # equal prio == same state
      count="$((count + 1))"
    fi
  done
  [ "$bestp" -eq 0 ] && { best="idle"; count=0; }

  # Send to the device. `clocal` so open() never waits on carrier, and `timeout`
  # so a hung/non-draining device can never block the hook (and thus the session)
  # indefinitely. The device may be absent — that's fine.
  if [ -e "$PORT" ]; then
    timeout 1 bash -c '
      stty -F "$1" -hupcl clocal 115200 raw 2>/dev/null || true
      printf "%s|%s\n" "$2" "$3" > "$1"
    ' _ "$PORT" "$best" "$count" 2>/dev/null || true
  fi
fi

exit 0
