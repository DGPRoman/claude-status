#!/usr/bin/env bash
# One-command host setup for the Claude status display.
#
#   ./install.sh          full setup: jq + udev rule + permissions + Claude hooks
#   ./install.sh --hooks  (re)install ONLY the Claude Code hooks (no sudo needed)
#   ./install.sh --test   send a demo state sequence to the device
#
# The firmware is already on the board — this script never touches it. Every
# path is derived from THIS checkout's location, so it works no matter where the
# user put the folder or what their username is.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${CLAUDE_STATUS_PORT:-/dev/claude-status}"
SETTINGS="$HOME/.claude/settings.json"
HOOK="$HERE/hooks/claude-status.sh"

# ---- demo mode ------------------------------------------------------------
if [ "${1:-}" = "--test" ]; then
  [ -e "$PORT" ] || { echo "No device at $PORT — plug it in / finish install first."; exit 1; }
  for s in "start|1" "work|1" "perm|1" "done|1" "idle|1"; do
    echo "-> $s"; printf '%s\n' "$s" > "$PORT"; sleep 1.5
  done
  echo "done."; exit 0
fi

# ---- merge our hooks into ~/.claude/settings.json -------------------------
# Per-event merge that PRESERVES the user's other hooks: for each of our 8 event
# keys we keep every existing entry that isn't ours, drop any previous
# claude-status entry (so re-running is idempotent), then append ours. Commands
# are rewritten to THIS checkout and shell-quoted (so a path with spaces works).
install_hooks() {
  command -v jq >/dev/null 2>&1 || { echo "!! jq is required to merge hooks — install it, then re-run: ./install.sh --hooks" >&2; return 1; }
  chmod +x "$HOOK" 2>/dev/null || true       # both entry points guarantee +x
  mkdir -p "$HOME/.claude"
  [ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"

  if ! jq empty "$SETTINGS" 2>/dev/null; then
    echo "!! $SETTINGS is not valid JSON. Fix it (or move it aside) and re-run." >&2
    return 1
  fi

  local tmp_ours tmp_out
  tmp_ours="$(mktemp)"; tmp_out="$(mktemp)"
  trap 'rm -f "$tmp_ours" "$tmp_out"' RETURN

  # our hooks object, paths rewritten to this checkout and wrapped in quotes
  jq --arg hook "\"$HOOK\"" '
    del(._comment)
    | .hooks |= with_entries(
        .value |= map(.hooks |= map(.command |= sub("^.*/hooks/claude-status\\.sh"; $hook)))
      )
    | .hooks
  ' "$HERE/claude-hooks.snippet.json" > "$tmp_ours" || { echo "!! failed to build hooks from snippet" >&2; return 1; }

  # back up existing settings first; if the backup can't be written, STOP.
  if [ -s "$SETTINGS" ] && [ "$(cat "$SETTINGS")" != "{}" ]; then
    cp "$SETTINGS" "$SETTINGS.bak.$(date +%s)-$$" || { echo "!! could not back up $SETTINGS — aborting without changes" >&2; return 1; }
  fi

  jq --slurpfile ours "$tmp_ours" '
    .hooks = (.hooks // {})
    | reduce ($ours[0] | keys[]) as $k (.;
        .hooks[$k] = (
          ((.hooks[$k] // []) | map(select(
            [.hooks[]?.command // ""] | any(test("/hooks/claude-status\\.sh")) | not
          )))
          + $ours[0][$k]
        )
      )
  ' "$SETTINGS" > "$tmp_out" || { echo "!! merge failed" >&2; return 1; }

  mv "$tmp_out" "$SETTINGS"
  echo "   hooks merged into $SETTINGS (script: $HOOK)"
}

if [ "${1:-}" = "--hooks" ]; then
  echo "Installing Claude Code hooks only"
  install_hooks
  jq '.hooks | keys' "$SETTINGS"
  exit 0
fi

# ---- full setup -----------------------------------------------------------
echo "1) Installing jq (if missing)"
if command -v jq >/dev/null 2>&1; then
  echo "   already present."
elif command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update -qq || true
  sudo apt-get install -y jq || echo "   !! could not auto-install jq — install it manually, then run: ./install.sh --hooks"
else
  echo "   !! jq not found and this is not an apt-based distro."
  echo "      Install jq with your package manager (dnf/pacman/brew/...), then run: ./install.sh --hooks"
fi

echo "2) Installing udev rule -> $PORT   (needs sudo)"
sudo cp "$HERE/udev/99-claude-status.rules" /etc/udev/rules.d/99-claude-status.rules
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "3) Enabling Claude Code hooks"
install_hooks || echo "   (hooks not enabled — see the message above)"

echo
echo "===================================================================="
echo "Done. Now:"
echo "  * Plug the board in via USB-C, then check:  ls -l $PORT"
echo "  * Smoke-test the display:                   $HERE/install.sh --test"
echo "  * Hooks are live in new Claude Code sessions (open one and type)."
echo "===================================================================="
if [ -e "$PORT" ]; then
  echo "Device detected at $PORT:"; ls -l "$PORT"
else
  echo "(No device at $PORT yet — plug it in; the udev rule will create it.)"
fi
