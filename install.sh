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
CONF="${CLAUDE_STATUS_CONF:-$HOME/.config/claude-status.conf}"   # WiFi transport config

# ---- demo mode ------------------------------------------------------------
if [ "${1:-}" = "--test" ]; then
  # shellcheck source=/dev/null
  [ -f "$CONF" ] && . "$CONF" 2>/dev/null || true
  url="${CLAUDE_STATUS_URL:-}"; tok="${CLAUDE_STATUS_TOKEN:-}"
  if [ -n "$url" ]; then
    for s in "work|1" "work|3" "perm|1" "perm|2" "off|1"; do
      echo "-> $s (HTTP)"; curl -sS -m 2 -H 'Content-Type: text/plain' --data-binary "$s" "$url${tok:+?t=$tok}" >/dev/null 2>&1 || true; sleep 1.5
    done
    echo "done."; exit 0
  fi
  [ -e "$PORT" ] || { echo "No device at $PORT — plug it in / finish install first."; exit 1; }
  for s in "work|1" "work|3" "perm|1" "perm|2" "off|1"; do
    echo "-> $s"; printf '%s\n' "$s" > "$PORT"; sleep 1.5
  done
  echo "done."; exit 0
fi

# ---- merge our hooks into ~/.claude/settings.json -------------------------
# Merge that PRESERVES the user's other hooks and fully OWNS ours: first strip
# every claude-status entry from ALL existing event keys (so keys we no longer use
# — e.g. a removed SessionStart — are cleaned up, and re-running is idempotent),
# then append our current entries for each event key in the snippet. Commands are
# rewritten to THIS checkout and shell-quoted (so a path with spaces works).
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
    # 1) strip OUR entries from every existing event key, then drop emptied keys.
    #    This cleans up any event we no longer wire (e.g. a removed SessionStart)
    #    while leaving the user'"'"'s foreign hooks untouched.
    | .hooks |= (
        with_entries(.value |= map(select(
          [.hooks[]?.command // ""] | any(test("/hooks/claude-status\\.sh")) | not
        )))
        | with_entries(select(.value | length > 0))
      )
    # 2) append our current entries for each event key in the snippet.
    | reduce ($ours[0] | keys[]) as $k (.;
        .hooks[$k] = ((.hooks[$k] // []) + $ours[0][$k])
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

# ---- WiFi transport setup -------------------------------------------------
#   ./install.sh --wifi [URL] [TOKEN]
# Points the hook at a networked device instead of the serial port. URL defaults
# to the mDNS name; TOKEN, if omitted, is read from the device over USB (the WiFi
# firmware answers a `token` command on the same serial link). Writes $CONF and
# merges the hooks. No sudo, no udev rule — the network path needs neither.
if [ "${1:-}" = "--wifi" ]; then
  URL="${2:-http://claude-status.local/state}"
  TOKEN="${3:-}"

  if [ -z "$TOKEN" ] && [ -e "$PORT" ]; then
    echo "Reading auth token from the device at $PORT (USB) ..."
    stty -F "$PORT" -hupcl clocal 115200 raw 2>/dev/null || true
    tmpr="$(mktemp)"
    ( timeout 4 cat "$PORT" > "$tmpr" 2>/dev/null ) &
    rpid=$!
    sleep 1
    printf 'token\n' > "$PORT" 2>/dev/null || true
    wait "$rpid" 2>/dev/null || true
    TOKEN="$(sed -n 's/^token=//p' "$tmpr" 2>/dev/null | tr -d '\r' | head -1)"
    rm -f "$tmpr"
    if [ -n "$TOKEN" ]; then
      echo "   got token ${TOKEN:0:4}…"
    else
      echo "   !! couldn't read a token. Is this the WiFi firmware, plugged in via USB?"
      echo "      You can pass it explicitly: ./install.sh --wifi '$URL' <TOKEN>"
    fi
  fi

  mkdir -p "$(dirname "$CONF")"
  {
    echo "# claude-status WiFi transport — written by install.sh --wifi"
    echo "CLAUDE_STATUS_URL=\"$URL\""
    if [ -n "$TOKEN" ]; then echo "CLAUDE_STATUS_TOKEN=\"$TOKEN\""; fi
  } > "$CONF"
  echo "   wrote $CONF"

  install_hooks
  jq '.hooks | keys' "$SETTINGS"
  echo
  echo "===================================================================="
  echo "WiFi transport configured -> $URL"
  echo "  * First-time / new network: hold BOOT ~2 s (or on first boot the device"
  echo "    opens a setup portal) and join the WiFi named 'claude-setup', then pick"
  echo "    your network. Old networks are remembered; it re-joins whichever is near."
  echo "  * Smoke-test:  $HERE/install.sh --test"
  echo "  * If claude-status.local doesn't resolve, use the IP the OLED shows:"
  echo "      ./install.sh --wifi 'http://<IP>/state'"
  echo "===================================================================="
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
